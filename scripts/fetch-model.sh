#!/usr/bin/env bash
# Download the Demucs v4 weights used by Flayr Chop's stem separator into the
# folder chop::stems::defaultModelPath() reads from.
#
#   scripts/fetch-model.sh            # htdemucs 4-stem (default, 84 MB)
#   scripts/fetch-model.sh 6s         # htdemucs_6s (adds guitar + piano, 55 MB)
#   scripts/fetch-model.sh ft         # htdemucs_ft bag of 4 models (4 x 84 MB)
#   scripts/fetch-model.sh all
#   scripts/fetch-model.sh --dir /some/folder [4s|6s|ft|all]
#
# Source (4s mirrored on github.com/FlayrLabs/flayr-chop releases): ggml float16 repack of Meta's MIT-licensed Demucs weights, made with
# sevagh/demucs.cpp's convert script, hosted at
# https://huggingface.co/datasets/Retrobear/demucs.cpp . Every file is checked
# against a pinned SHA-256 before it is moved into place. The weights are not
# part of the git repository.
set -euo pipefail

BASE_URL="https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main"
# The default 4-stem model is mirrored on Flayr Chop's own releases.
MIRROR_4S="https://github.com/FlayrLabs/flayr-chop/releases/download/model-htdemucs-4s-v1"

dest=""
which="4s"
while [ $# -gt 0 ]; do
  case "$1" in
    --dir) dest="$2"; shift 2 ;;
    4s|6s|ft|all) which="$1"; shift ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ -z "$dest" ]; then
  case "$(uname -s)" in
    Darwin) dest="$HOME/Library/Application Support/Flayr Chop/models" ;;
    MINGW*|MSYS*|CYGWIN*)
      [ -n "${APPDATA:-}" ] || { echo "APPDATA is not set" >&2; exit 1; }
      dest="$(cygpath -u "$APPDATA" 2>/dev/null || echo "$APPDATA")/Flayr Chop/models" ;;
    *) dest="${XDG_DATA_HOME:-$HOME/.local/share}/Flayr Chop/models" ;;
  esac
fi

# name  size  sha256
MODELS_4S="ggml-model-htdemucs-4s-f16.bin 83994361 72b17c42d308982ddb5069bc3bf48b81a5aac4cb6516e4366c0fa7cef6df0064"
MODELS_6S="ggml-model-htdemucs-6s-f16.bin 54855129 09704f4ceae204e56e77d5eefd6ac71d7275be81fd507e6913371d59abcee856"
MODELS_FT="ggml-model-htdemucs_ft_drums-4s-f16.bin 83994361 cda268755cc80deb5f4616a99f9d09f26bc5ce3882aa35e0bc504708a12604bb
ggml-model-htdemucs_ft_bass-4s-f16.bin 83994361 e6ded215aa15fb4eff7a37f3a51fd596ee17c94ae0ddb7ef3f2f6ecf8b86cdb5
ggml-model-htdemucs_ft_other-4s-f16.bin 83994361 cbc315571584d512e7853c9d40a499182393a7f4155cb8723a9040a685c2435b
ggml-model-htdemucs_ft_vocals-4s-f16.bin 83994361 19186500a45a551a034d96e9500415ebe73c8bd570bf55337ddc8cc8f53a9120"

case "$which" in
  4s) list="$MODELS_4S" ;;
  6s) list="$MODELS_6S" ;;
  ft) list="$MODELS_FT" ;;
  all) list="$MODELS_4S
$MODELS_6S
$MODELS_FT" ;;
esac

sha256() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
  else sha256sum "$1" | awk '{print $1}'; fi
}

mkdir -p "$dest"

# free-space check (KiB) with a 50 MB margin
need_kb=0
while read -r name size sum; do
  [ -f "$dest/$name" ] && [ "$(sha256 "$dest/$name")" = "$sum" ] && continue
  need_kb=$(( need_kb + size / 1024 + 1 ))
done <<< "$list"
free_kb=$(df -Pk "$dest" | awk 'NR==2 {print $4}')
if [ "$need_kb" -gt 0 ] && [ "$free_kb" -lt $(( need_kb + 51200 )) ]; then
  echo "not enough disk space in $dest: need $((need_kb / 1024)) MB, have $((free_kb / 1024)) MB" >&2
  exit 1
fi

while read -r name size sum; do
  target="$dest/$name"
  if [ -f "$target" ] && [ "$(sha256 "$target")" = "$sum" ]; then
    echo "ok       $target"
    continue
  fi
  echo "fetching $name ($((size / 1048576)) MB)"
  part="$target.part"
  url="$BASE_URL/$name"
  [ "$name" = "ggml-model-htdemucs-4s-f16.bin" ] && url="$MIRROR_4S/$name"
  curl -fL --retry 3 --retry-delay 2 --progress-bar -o "$part" "$url"
  got="$(sha256 "$part")"
  if [ "$got" != "$sum" ]; then
    rm -f "$part"
    echo "checksum mismatch for $name (got $got)" >&2
    exit 1
  fi
  mv -f "$part" "$target"
  echo "ok       $target"
done <<< "$list"
