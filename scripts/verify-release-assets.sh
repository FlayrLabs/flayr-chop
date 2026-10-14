#!/usr/bin/env bash
# Verifies the CI-built files on a draft release before Flayr Chop's checksums vouch for them.
#
#   scripts/verify-release-assets.sh TAG TAG_COMMIT DOWNLOAD_DIR OUT_DIR [LOCAL_FILE...]
#
# Every FlayrChop-* file in DOWNLOAD_DIR (just downloaded from the draft) is copied into OUT_DIR
# (which must be empty), and the copy must carry a build provenance attestation made by this repo's
# release workflow, for refs/tags/TAG at exactly TAG_COMMIT, on a GitHub-hosted runner, issued by
# GitHub Actions' OIDC provider. LOCAL_FILEs (the DMG built and notarized on this Mac) are copied
# in as they are. SHA256SUMS.txt is then written over exactly those bytes. Anything that doesn't
# verify, a gh too old to check all of that, a link, or nothing to verify at all stops the script
# before SHA256SUMS.txt exists: it fails closed.
#
# (The stem model release asset is not handled here: the plug-in checks it against the SHA-256
# pinned in Source/stems/Stems.cpp.)
set -euo pipefail

GH="${GH:-gh}"
REPO="FlayrLabs/flayr-chop"
WORKFLOW="$REPO/.github/workflows/release.yml"
ISSUER="https://token.actions.githubusercontent.com"

die() { echo "error: $*" >&2; exit 1; }
[[ $# -ge 4 ]] || die "usage: verify-release-assets.sh TAG TAG_COMMIT DOWNLOAD_DIR OUT_DIR [LOCAL_FILE...]"
TAG="$1"; TAG_COMMIT="$2"; DOWNLOAD="$3"; OUT="$4"; shift 4
[[ "$TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "not a release tag: $TAG"
[[ "$TAG_COMMIT" =~ ^[0-9a-f]{40}$ ]] || die "not a commit id: $TAG_COMMIT"
[[ -d "$OUT" && -z "$(ls -A "$OUT")" ]] || die "$OUT must be an empty directory"

help="$("$GH" attestation verify --help 2>&1)" || die "gh attestation isn't available (gh too old?)"
for flag in --signer-workflow --source-ref --source-digest --deny-self-hosted-runners --cert-oidc-issuer --format; do
    grep -q -- "$flag" <<<"$help" || die "gh is too old to check $flag; update gh"
done

shopt -s nullglob
assets=("$DOWNLOAD"/FlayrChop-*)
[[ ${#assets[@]} -gt 0 ]] || die "the draft has no CI-built files to verify"
for asset in "${assets[@]}"; do
    name="$(basename "$asset")"
    [[ -f "$asset" && ! -L "$asset" ]] || die "$name is not a regular file"
    cp -P "$asset" "$OUT/$name"
    # The copy is what gets verified and hashed: no second download, no window to swap it.
    digests="$("$GH" attestation verify "$OUT/$name" --repo "$REPO" --signer-workflow "$WORKFLOW" \
        --source-ref "refs/tags/$TAG" --source-digest "$TAG_COMMIT" --deny-self-hosted-runners \
        --cert-oidc-issuer "$ISSUER" --format json \
        --jq '.[].verificationResult.signature.certificate.sourceRepositoryDigest')" \
        || { rm -f "$OUT/$name"; die "$name has no valid provenance attestation from $TAG's release workflow"; }
    [[ -n "$digests" ]] || { rm -f "$OUT/$name"; die "$name: the attestation names no source commit"; }
    while read -r d; do
        [[ "$d" == "$TAG_COMMIT" ]] || { rm -f "$OUT/$name"; die "$name was attested for commit $d, not $TAG_COMMIT"; }
    done <<<"$digests"
    echo "    verified $name"
done
for local in "$@"; do
    [[ -f "$local" && ! -L "$local" ]] || die "$local is not a regular file"
    [[ ! -e "$OUT/$(basename "$local")" ]] || die "$(basename "$local") is already on the draft; not vouching for that copy"
    cp -P "$local" "$OUT/"
done
(cd "$OUT" && shasum -a 256 FlayrChop-* > SHA256SUMS.txt.part && mv SHA256SUMS.txt.part SHA256SUMS.txt && cat SHA256SUMS.txt)
