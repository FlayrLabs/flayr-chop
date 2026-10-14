#!/usr/bin/env bash
# ctest: scripts/verify-release-assets.sh against a fake `gh` (no network), one passing
# scenario and one per way an asset or its provenance can be wrong. Every failure must
# exit non-zero and leave the output directory empty (no partial SHA256SUMS.txt).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERIFY="$ROOT/scripts/verify-release-assets.sh"
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
        wd="sha256:$WIN_SHA"; [[ "$SCENARIO" == release-digest-mismatch ]] && wd="sha256:0000"
        extra=""; [[ "$SCENARIO" == extra-asset ]] && extra=',{"name":"FlayrChop-evil.pkg","digest":"sha256:00"}'
        lin=",{\"name\":\"$LIN\",\"digest\":\"sha256:$LIN_SHA\"}"; [[ "$SCENARIO" == missing-asset ]] && lin=""
        echo "[{\"name\":\"$WIN\",\"digest\":\"$wd\"}$lin$extra]"
        ;;
    "run download")
        dir="$(arg_after --dir "$@")"; mkdir -p "$dir/windows" "$dir/linux"
        cp "$FIX/$WIN" "$dir/windows/"; cp "$FIX/$LIN" "$dir/linux/"
        [[ "$SCENARIO" == run-artifact-differs ]] && printf 'rebuilt elsewhere\n' > "$dir/windows/$WIN"
        exit 0
        ;;
    "release download")
        dir="$(arg_after --dir "$@")"; pattern="$(arg_after --pattern "$@")"
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
    local scenario="$1" expect="$2" out="$T/out-$1"
    mkdir -p "$out"
    : > "$T/calls"
    write_env "$scenario"
    if SCENARIO="$scenario" PATH="$T/bin:$PATH" "$VERIFY" "$TAG" "$SHA" "$VERSION" "$T/FlayrChop-$VERSION-macOS.dmg" "$out" \
           > "$T/log-$scenario" 2>&1; then rc=0; else rc=$?; fi
    if [[ "$expect" == pass ]]; then
        if [[ $rc -eq 0 ]] && grep -q "$WIN_SHA  $WIN" "$out/SHA256SUMS.txt" && grep -q "$LIN_SHA  $LIN" "$out/SHA256SUMS.txt" \
             && grep -q "$(sha "$T/FlayrChop-$VERSION-macOS.dmg")  FlayrChop-$VERSION-macOS.dmg" "$out/SHA256SUMS.txt" \
             && grep -q -- "--signer-workflow FlayrLabs/flayr-chop/.github/workflows/release.yml --source-ref refs/tags/$TAG --source-digest $SHA --deny-self-hosted-runners --cert-oidc-issuer https://token.actions.githubusercontent.com" "$T/calls"; then
            echo "  [PASS] $scenario: verified, SHA256SUMS.txt over the verified bytes, strict flags used"
        else
            echo "  [FAIL] $scenario: expected success"; cat "$T/log-$scenario"; failures=$((failures + 1))
        fi
    else
        if [[ $rc -ne 0 && -z "$(ls -A "$out")" ]]; then
            echo "  [PASS] $scenario: refused ($(tail -1 "$T/log-$scenario"))"
        else
            echo "  [FAIL] $scenario: expected refusal with no output (rc=$rc)"; cat "$T/log-$scenario"; failures=$((failures + 1))
        fi
    fi
}

run_case ok pass
for s in old-gh no-run extra-asset missing-asset release-digest-mismatch swapped-download run-artifact-differs \
         attestation-fails wrong-source-digest wrong-ref self-hosted other-run wrong-subject workflow-dispatch; do
    run_case "$s" fail
done

# gh settings in the environment are refused, not passed on.
for var in GH_HOST GH_REPO; do
    out="$T/out-env-$var"; mkdir -p "$out"; write_env ok
    if env "$var=evil.example" PATH="$T/bin:$PATH" "$VERIFY" "$TAG" "$SHA" "$VERSION" "$T/FlayrChop-$VERSION-macOS.dmg" "$out" \
           > "$T/log-env" 2>&1; then rc=0; else rc=$?; fi
    if [[ $rc -ne 0 && -z "$(ls -A "$out")" ]]; then echo "  [PASS] $var set: refused"; else
        echo "  [FAIL] $var set: expected refusal"; failures=$((failures + 1)); fi
done

echo
[[ $failures -eq 0 ]] && echo "ALL PASSED" || { echo "FAILURES ($failures)"; exit 1; }
