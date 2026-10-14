#!/usr/bin/env bash
# scripts/verify-release-assets.sh against a fake gh: it must vouch for attested CI files and the
# local DMG, and fail closed (no SHA256SUMS.txt) on anything else.
#   tests/release_verify_test.sh path/to/scripts/verify-release-assets.sh
set -uo pipefail
SCRIPT="$1"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
TAG=v9.8.7
COMMIT=0123456789abcdef0123456789abcdef01234567
failures=0
check() { if [[ "$1" == "$2" ]]; then echo "ok   $3"; else echo "FAIL $3 (got $1, want $2)"; failures=$((failures + 1)); fi; }

# The fake gh: 'attestation verify --help' lists the flags (or not, as an old gh); 'attestation
# verify FILE ...' passes or fails by the file's contents, and must be asked for everything.
cat > "$T/gh" <<'GH'
#!/usr/bin/env bash
if [[ "$*" == *"--help"* ]]; then
    [[ -n "${FAKE_OLD_GH:-}" ]] && { echo "--signer-workflow --format"; exit 0; }
    echo "--signer-workflow --source-ref --source-digest --deny-self-hosted-runners --cert-oidc-issuer --format"; exit 0
fi
args="$*"
for need in "--repo FlayrLabs/flayr-chop" "--signer-workflow FlayrLabs/flayr-chop/.github/workflows/release.yml" \
            "--source-ref refs/tags/v9.8.7" "--source-digest 0123456789abcdef0123456789abcdef01234567" \
            "--deny-self-hosted-runners" "--cert-oidc-issuer https://token.actions.githubusercontent.com"; do
    [[ "$args" == *"$need"* ]] || { echo "fake gh: missing $need" >&2; exit 3; }
done
file="$3"
case "$(cat "$file")" in
    GOOD*) echo 0123456789abcdef0123456789abcdef01234567 ;;
    OTHER*) echo ffffffffffffffffffffffffffffffffffffffff ;;
    EMPTY*) ;;
    *) echo "fake gh: no attestation" >&2; exit 1 ;;
esac
GH
chmod +x "$T/gh"

run() { # name, then files as name=content; returns the script's status, leaves $T/out
    rm -rf "$T/dl" "$T/out"; mkdir -p "$T/dl" "$T/out"
    for spec in "$@"; do printf '%s' "${spec#*=}" > "$T/dl/${spec%%=*}"; done
    printf 'DMG' > "$T/FlayrChop-9.8.7-macOS.dmg"
    GH="$T/gh" bash "$SCRIPT" "$TAG" "$COMMIT" "$T/dl" "$T/out" "$T/FlayrChop-9.8.7-macOS.dmg" >/dev/null 2>&1
}

run FlayrChop-9.8.7-Linux-x64.tar.gz=GOOD1 FlayrChop-9.8.7-Windows-Setup.exe=GOOD2; status=$?
check "$status" 0 "attested files and the DMG are vouched for"
check "$(wc -l < "$T/out/SHA256SUMS.txt" 2>/dev/null | tr -d ' ')" 3 "SHA256SUMS.txt lists all three"
check "$(grep -c "$(printf GOOD1 | shasum -a 256 | cut -d' ' -f1)" "$T/out/SHA256SUMS.txt" 2>/dev/null)" 1 "checksum is of the verified bytes"

run FlayrChop-9.8.7-Linux-x64.tar.gz=GOOD1 FlayrChop-9.8.7-Windows-Setup.exe=TAMPERED; status=$?
check "$([[ $status -ne 0 && ! -e "$T/out/SHA256SUMS.txt" ]] && echo closed)" closed "an unattested file stops it, no checksums"
run FlayrChop-9.8.7-Linux-x64.tar.gz=OTHER; status=$?
check "$([[ $status -ne 0 && ! -e "$T/out/SHA256SUMS.txt" ]] && echo closed)" closed "an attestation for another commit stops it"
run FlayrChop-9.8.7-Linux-x64.tar.gz=EMPTY; status=$?
check "$([[ $status -ne 0 && ! -e "$T/out/SHA256SUMS.txt" ]] && echo closed)" closed "an attestation without a source digest stops it"
run; status=$?
check "$([[ $status -ne 0 && ! -e "$T/out/SHA256SUMS.txt" ]] && echo closed)" closed "nothing to verify stops it"
run FlayrChop-9.8.7-macOS.dmg=GOOD9 FlayrChop-9.8.7-Linux-x64.tar.gz=GOOD1; status=$?
check "$([[ $status -ne 0 && ! -e "$T/out/SHA256SUMS.txt" ]] && echo closed)" closed "a DMG already on the draft stops it"
FAKE_OLD_GH=1 run FlayrChop-9.8.7-Linux-x64.tar.gz=GOOD1; status=$?
check "$([[ $status -ne 0 && ! -e "$T/out/SHA256SUMS.txt" ]] && echo closed)" closed "a gh too old for the checks stops it"
rm -rf "$T/dl" "$T/out"; mkdir -p "$T/dl" "$T/out"; printf GOOD1 > "$T/real"; ln -s "$T/real" "$T/dl/FlayrChop-9.8.7-Linux-x64.tar.gz"
GH="$T/gh" bash "$SCRIPT" "$TAG" "$COMMIT" "$T/dl" "$T/out" >/dev/null 2>&1; status=$?
check "$([[ $status -ne 0 && ! -e "$T/out/SHA256SUMS.txt" ]] && echo closed)" closed "a link in the download stops it"

echo "release_verify_test: $failures failures"
[[ $failures -eq 0 ]]
