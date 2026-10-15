#!/usr/bin/env bash
# Verifies the CI-built assets on a draft release and writes SHA256SUMS.txt for exactly
# the bytes it verified. Used by release-macos.sh --upload; fails closed: any missing
# tool, unexpected asset or failed check exits non-zero and writes nothing.
#
#   scripts/verify-release-assets.sh <tag> <tag commit sha> <version> <local DMG> <out dir>
#
# For each CI asset (the Windows installer and the Linux tarball):
#   1. the tag's release.yml run is found: a push of refs/tags/<tag> at the tag commit,
#      completed successfully;
#   2. the asset is downloaded once, into a fresh directory, and never again;
#   3. its bytes must equal the release's own recorded digest and the artifact that run
#      uploaded (gh run download), so it came from that run;
#   4. gh attestation verify must pass with --signer-workflow release.yml, --source-ref
#      refs/tags/<tag>, --source-digest <tag commit>, --deny-self-hosted-runners and the
#      GitHub Actions OIDC issuer, and the attestation itself must name that commit, ref,
#      run, a GitHub-hosted runner, a push trigger, and this file's name and SHA-256.
# Any other FlayrChop-* asset on the release stops the script. The DMG is hashed from
# the local file that release-macos.sh built, notarized and is about to upload.
set -euo pipefail

[[ $# -eq 5 ]] || { echo "usage: $0 <tag> <tag commit sha> <version> <local DMG> <out dir>" >&2; exit 2; }
TAG="$1" TAG_SHA="$2" VERSION="$3" DMG="$4" OUT="$5"
REPO="FlayrLabs/flayr-chop" # pinned: no environment override

# gh runs sealed: no GH_HOST, GH_REPO or other gh setting from the environment can point it at
# another host or repository (refused outright), only PATH, HOME (its stored login) and a token.
for var in GH_HOST GH_REPO GH_CONFIG_DIR GH_ENTERPRISE_TOKEN GITHUB_ENTERPRISE_TOKEN GH_DEBUG HTTPS_PROXY HTTP_PROXY ALL_PROXY; do
    [[ -z "${!var:-}" ]] || { echo "error: $var is set; unset it for a release" >&2; exit 1; }
done
GH_BIN="$(command -v gh)" || { echo "error: gh is not installed" >&2; exit 1; }
gh() {
    env -i "PATH=$PATH" "HOME=$HOME" ${GH_TOKEN:+"GH_TOKEN=$GH_TOKEN"} ${GITHUB_TOKEN:+"GITHUB_TOKEN=$GITHUB_TOKEN"} "$GH_BIN" "$@"
}
WORKFLOW_PATH=".github/workflows/release.yml"
ISSUER="https://token.actions.githubusercontent.com"

die() { echo "error: $*" >&2; exit 1; }

for tool in jq shasum; do
    command -v "$tool" >/dev/null || die "$tool is not installed"
done
[[ "$TAG_SHA" =~ ^[0-9a-f]{40}$ ]] || die "not a commit SHA: $TAG_SHA"
[[ -f "$DMG" ]] || die "no DMG at $DMG"
[[ -d "$OUT" && -z "$(ls -A "$OUT")" ]] || die "$OUT must be an existing, empty directory"

# gh must support every flag used below (gh >= 2.67 or so); otherwise stop.
help="$(gh attestation verify --help 2>&1)" || die "gh has no 'attestation verify' (too old?)"
for flag in --signer-workflow --source-ref --source-digest --deny-self-hosted-runners --cert-oidc-issuer --format; do
    grep -q -- "$flag" <<< "$help" || die "this gh does not support attestation verify $flag; update gh"
done

sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }

DMG_NAME="$(basename "$DMG")"
WINDOWS="FlayrChop-$VERSION-Windows-Setup.exe"
LINUX="FlayrChop-$VERSION-Linux-x64.tar.gz"
CI_ASSETS=("$WINDOWS" "$LINUX")

# 1. The tag's release workflow run.
runs="$(gh run list --repo "$REPO" --workflow release.yml --event push --limit 100 \
        --json databaseId,headSha,headBranch,status,conclusion)" || die "could not list release.yml runs"
RUN_ID="$(jq -r --arg sha "$TAG_SHA" --arg tag "$TAG" \
    '[.[] | select(.headSha == $sha and .headBranch == $tag and .status == "completed" and .conclusion == "success")]
     | if length == 0 then "" else (max_by(.databaseId) | .databaseId | tostring) end' <<< "$runs")"
[[ -n "$RUN_ID" ]] || die "no successful release.yml run for $TAG at $TAG_SHA"
echo "    release.yml run $RUN_ID built $TAG at $TAG_SHA"

# Only the expected assets may be on the draft.
assets="$(gh release view "$TAG" --repo "$REPO" --json assets --jq '.assets')" || die "could not read release $TAG"
while IFS= read -r name; do
    case "$name" in
        "$WINDOWS"|"$LINUX"|"$DMG_NAME"|SHA256SUMS.txt) ;;
        *) die "unexpected asset on release $TAG: $name (remove it first)" ;;
    esac
done < <(jq -r '.[].name' <<< "$assets")

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir "$WORK/release" "$WORK/run"

# 2 + 3. One download of each asset, checked against the release digest and the run's artifacts.
gh run download "$RUN_ID" --repo "$REPO" --dir "$WORK/run" >/dev/null || die "could not download run $RUN_ID's artifacts"
for name in "${CI_ASSETS[@]}"; do
    recorded="$(jq -r --arg n "$name" '.[] | select(.name == $n) | .digest // empty' <<< "$assets")"
    [[ -n "$recorded" ]] || die "$name is not on release $TAG (or has no recorded digest)"
    gh release download "$TAG" --repo "$REPO" --pattern "$name" --dir "$WORK/release" >/dev/null || die "could not download $name"
    file="$WORK/release/$name"
    [[ -f "$file" && ! -L "$file" ]] || die "$name did not download as a regular file"
    digest="$(sha256 "$file")"
    [[ "$recorded" == "sha256:$digest" ]] || die "$name: downloaded bytes ($digest) differ from the release's digest ($recorded)"
    from_run="$(find "$WORK/run" -type f -name "$name" | head -2)"
    [[ -n "$from_run" && "$(wc -l <<< "$from_run")" -eq 1 ]] || die "$name is not (exactly once) among run $RUN_ID's artifacts"
    [[ "$(sha256 "$from_run")" == "$digest" ]] || die "$name on the release is not the file run $RUN_ID built"

    # 4. Provenance, as strictly as gh allows, then the attestation's own claims.
    json="$(gh attestation verify "$file" --repo "$REPO" \
                --signer-workflow "$REPO/$WORKFLOW_PATH" \
                --source-ref "refs/tags/$TAG" --source-digest "$TAG_SHA" \
                --deny-self-hosted-runners --cert-oidc-issuer "$ISSUER" \
                --format json)" || die "$name: build provenance attestation does not verify"
    jq -e --arg sha "$TAG_SHA" --arg ref "refs/tags/$TAG" --arg issuer "$ISSUER" --arg name "$name" \
          --arg digest "$digest" --arg run "^https://github\\.com/$REPO/actions/runs/$RUN_ID/attempts/[0-9]+$" '
        type == "array" and length > 0 and any(.[];
            .verificationResult.signature.certificate as $c
            | $c.sourceRepositoryDigest == $sha and $c.sourceRepositoryRef == $ref
              and $c.issuer == $issuer and $c.runnerEnvironment == "github-hosted"
              and $c.githubWorkflowTrigger == "push"
              and ($c.runInvocationURI | test($run))
              and any(.verificationResult.statement.subject[]?; .name == $name and .digest.sha256 == $digest))
    ' <<< "$json" >/dev/null || die "$name: the attestation does not name $TAG_SHA, refs/tags/$TAG, run $RUN_ID and this file"
    echo "    verified $name ($digest)"
done

# Checksums over exactly the verified bytes, plus the local DMG; written only now.
cp "$DMG" "$WORK/release/$DMG_NAME"
(cd "$WORK/release" && shasum -a 256 "$WINDOWS" "$LINUX" "$DMG_NAME") > "$WORK/SHA256SUMS.txt"
mv "$WORK/SHA256SUMS.txt" "$OUT/SHA256SUMS.txt"
echo "    wrote $OUT/SHA256SUMS.txt"
