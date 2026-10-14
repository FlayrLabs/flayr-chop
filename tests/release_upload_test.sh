#!/usr/bin/env bash
# ctest: scripts/upload-release.sh (verification, upload, and the checks of the draft
# afterwards) against a fake `gh` (no network): one passing scenario and one per way the
# release or the uploaded files can be wrong. Every failure must exit non-zero.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UPLOAD="$ROOT/scripts/upload-release.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

TAG=v9.9.9 VERSION=9.9.9 SHA=0123456789abcdef0123456789abcdef01234567 RUN=4242
WIN="FlayrChop-$VERSION-Windows-Setup.exe" LIN="FlayrChop-$VERSION-Linux-x64.tar.gz"
mkdir -p "$T/fixtures" "$T/bin"
printf 'windows installer bytes\n' > "$T/fixtures/$WIN"
printf 'linux tarball bytes\n' > "$T/fixtures/$LIN"
printf 'notarized dmg bytes\n' > "$T/FlayrChop-$VERSION-macOS.dmg"
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
WIN_SHA="$(sha "$T/fixtures/$WIN")" LIN_SHA="$(sha "$T/fixtures/$LIN")"

# The fake gh: behaviour switched by $SCENARIO, every call logged to $T/calls.
cat > "$T/bin/gh" <<FAKE_HEAD
#!/usr/bin/env bash
set -euo pipefail
# verify-release-assets.sh runs gh under env -i: the scenario and fixtures come from files.
T="$T"
source "\$T/fake-env"
FAKE_HEAD
cat >> "$T/bin/gh" <<'FAKE'
echo "$*" >> "$T/calls"
arg_after() { local want="$1"; shift; while [[ $# -gt 0 ]]; do [[ "$1" == "$want" ]] && { echo "$2"; return; }; shift; done; }
case "$1 $2" in
    "attestation verify")
        if [[ "${3:-}" == "--help" ]]; then
            if [[ "$SCENARIO" == old-gh ]]; then echo "--signer-workflow --source-ref --format"; else
            echo "--signer-workflow --source-ref --source-digest --deny-self-hosted-runners --cert-oidc-issuer --format"; fi
            exit 0
        fi
        file="$3"; name="$(basename "$file")"; digest="$(shasum -a 256 "$file" | cut -d' ' -f1)"
        [[ "$SCENARIO" == attestation-fails ]] && { echo "verification failed" >&2; exit 1; }
        src="$SHA"; ref="refs/tags/$TAG"; runner=github-hosted; run="$RUN"; trigger=push
        case "$SCENARIO" in
            wrong-source-digest) src=ffffffffffffffffffffffffffffffffffffffff ;;
            wrong-ref) ref=refs/heads/main ;;
            self-hosted) runner=self-hosted ;;
            other-run) run=1 ;;
            wrong-subject) digest=0000 ;;
            workflow-dispatch) trigger=workflow_dispatch ;;
        esac
        cat <<JSON
[{"verificationResult":{"signature":{"certificate":{"issuer":"https://token.actions.githubusercontent.com",
  "sourceRepositoryDigest":"$src","sourceRepositoryRef":"$ref","runnerEnvironment":"$runner","githubWorkflowTrigger":"$trigger",
  "runInvocationURI":"https://github.com/FlayrLabs/flayr-chop/actions/runs/$run/attempts/1"}},
  "statement":{"subject":[{"name":"$name","digest":{"sha256":"$digest"}}]}}}]
JSON
        ;;
    "run list")
        sha="$SHA"; [[ "$SCENARIO" == no-run ]] && sha=ffffffffffffffffffffffffffffffffffffffff
        echo "[{\"databaseId\":$RUN,\"headSha\":\"$sha\",\"headBranch\":\"$TAG\",\"status\":\"completed\",\"conclusion\":\"success\"},
               {\"databaseId\":9999,\"headSha\":\"$SHA\",\"headBranch\":\"$TAG\",\"status\":\"completed\",\"conclusion\":\"failure\"}]"
        ;;
    "release view")
        if [[ "$*" == *isDraft* ]]; then [[ "$SCENARIO" == published ]] && echo false || echo true; exit 0; fi
        list="{\"name\":\"$WIN\",\"digest\":\"sha256:$WIN_SHA\"},{\"name\":\"$LIN\",\"digest\":\"sha256:$LIN_SHA\"}"
        if [[ -d "$T/uploaded" ]]; then # after the upload: what was uploaded, as the fake stored it
            for f in "$T/uploaded"/*; do
                d="$(shasum -a 256 "$f" | cut -d' ' -f1)"
                [[ "$SCENARIO" == post-digest && "$(basename "$f")" == *.dmg ]] && d=0000
                list="$list,{\"name\":\"$(basename "$f")\",\"digest\":\"sha256:$d\"}"
            done
            [[ "$SCENARIO" == post-extra ]] && list="$list,{\"name\":\"FlayrChop-evil.pkg\",\"digest\":\"sha256:00\"}"
        fi
        echo "[$list]"
        ;;
    "release upload")
        mkdir -p "$T/uploaded"; shift 3
        while [[ $# -gt 0 && "$1" != --* ]]; do cp "$1" "$T/uploaded/"; shift; done
        [[ "$SCENARIO" == post-sums ]] && printf 'swapped\n' >> "$T/uploaded/SHA256SUMS.txt"
        exit 0
        ;;
    "run download")
        dir="$(arg_after --dir "$@")"; mkdir -p "$dir/windows" "$dir/linux"
        cp "$FIX/$WIN" "$dir/windows/"; cp "$FIX/$LIN" "$dir/linux/"
        [[ "$SCENARIO" == run-artifact-differs ]] && printf 'rebuilt elsewhere\n' > "$dir/windows/$WIN"
        exit 0
        ;;
    "release download")
        dir="$(arg_after --dir "$@")"; pattern="$(arg_after --pattern "$@")"
        if [[ "$pattern" == SHA256SUMS.txt ]]; then cp "$T/uploaded/SHA256SUMS.txt" "$dir/"; exit 0; fi
        cp "$FIX/$pattern" "$dir/"
        [[ "$SCENARIO" == swapped-download && "$pattern" == "$WIN" ]] && printf 'swapped\n' > "$dir/$pattern"
        exit 0
        ;;
    *) echo "fake gh: unexpected call: $*" >&2; exit 3 ;;
esac
FAKE
chmod +x "$T/bin/gh"
export T TAG SHA RUN WIN LIN WIN_SHA LIN_SHA FIX="$T/fixtures"
write_env() {
    { printf 'SCENARIO=%q\n' "$1"; for v in TAG SHA RUN WIN LIN WIN_SHA LIN_SHA; do printf '%s=%q\n' "$v" "${!v}"; done
      printf 'FIX=%q\n' "$T/fixtures"; } > "$T/fake-env"
}

failures=0
run_case() {
    local scenario="$1" expect="$2"
    : > "$T/calls"; rm -rf "$T/uploaded"
    write_env "$scenario"
    if PATH="$T/bin:$PATH" "$UPLOAD" "$TAG" "$SHA" "$VERSION" "$T/FlayrChop-$VERSION-macOS.dmg" > "$T/log-$scenario" 2>&1; then rc=0; else rc=$?; fi
    if [[ "$expect" == pass && $rc -eq 0 && -f "$T/uploaded/SHA256SUMS.txt" ]]; then
        echo "  [PASS] $scenario: uploaded and checked"
    elif [[ "$expect" == fail && $rc -ne 0 ]]; then
        echo "  [PASS] $scenario: refused ($(tail -1 "$T/log-$scenario"))"
    else
        echo "  [FAIL] $scenario: expected $expect (rc=$rc)"; cat "$T/log-$scenario"; failures=$((failures + 1))
    fi
}

run_case ok pass
for s in published attestation-fails post-extra post-digest post-sums; do
    run_case "$s" fail
done
# Nothing is uploaded when verification fails.
write_env attestation-fails; rm -rf "$T/uploaded"
PATH="$T/bin:$PATH" "$UPLOAD" "$TAG" "$SHA" "$VERSION" "$T/FlayrChop-$VERSION-macOS.dmg" > /dev/null 2>&1 || true
if [[ -d "$T/uploaded" ]]; then echo "  [FAIL] files were uploaded after a failed verification"; failures=$((failures + 1));
else echo "  [PASS] nothing uploaded after a failed verification"; fi

echo
[[ $failures -eq 0 ]] && echo "ALL PASSED" || { echo "FAILURES ($failures)"; exit 1; }
