#!/bin/bash
# Tempo / key evaluation on real files (no audio is stored in the repo).
#
#   tests/eval/run_eval.sh [--stems] [--baseline DIR] [--rows FILE] [--labels FILE]
#
#   --stems         also separate music + mix rows with build/stems_cli and score analyseWithStems()
#                   against analyse() on the same rows (slow: about 3x realtime)
#   --baseline DIR  score the Analysis.h found in DIR instead of Source/ (built with -DEVAL_BASELINE,
#                   so an engine without the candidate / stem API, such as v1.0.0, still compiles)
#   --rows FILE     write one line per file (truth, estimate, candidates) for error analysis
#   --rate HZ       convert sources to this rate (default 44100; 48000 is what most real material is)
#   --excerpts      tempo on untrimmed 5..20 s excerpts instead of the files as cut (see eval_runner.cpp)
#
# Sources are converted once with afconvert into $EVAL_SCRATCH (default: $TMPDIR/flayr-chop-eval) and
# reused; delete that folder when done. Files missing on this machine are skipped and counted.
# Requires macOS (afconvert) and clang++.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
LABELS="$HERE/labels.tsv"
# A private label list (with personal files) takes over when present.
[ -f "$HERE/labels.local.tsv" ] && LABELS="$HERE/labels.local.tsv"
SCRATCH="${EVAL_SCRATCH:-${TMPDIR:-/tmp}/flayr-chop-eval}"
STEMS=0
HEADER_DIR="$REPO/Source"
EXTRA=()
ROWS=""
RUNOPTS=()
RATE=44100
while [ $# -gt 0 ]; do
    case "$1" in
        --stems) STEMS=1 ;;
        --baseline) HEADER_DIR="$(cd "$2" && pwd)"; EXTRA=(-DEVAL_BASELINE); shift ;;
        --rows) ROWS="$2"; shift ;;
        --excerpts) RUNOPTS+=(--excerpts) ;;
        --rate) RATE="$2"; shift ;;
        --labels) LABELS="$2"; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done

MUSIC_ROOT="$HOME/Music"
APPLE_ROOT="/Library/Audio/Apple Loops"
WAV="$SCRATCH/wav_$RATE" STEMDIR="$SCRATCH/stems_$RATE" MIXDIR="$SCRATCH/mix_$RATE"
mkdir -p "$WAV" "$STEMDIR" "$MIXDIR"

RUNNER="$SCRATCH/eval_runner"
[ ${#EXTRA[@]} -gt 0 ] && RUNNER="$SCRATCH/eval_runner_baseline"
clang++ -O2 -std=c++17 -Wall -Wextra -Wshadow ${EXTRA[@]+"${EXTRA[@]}"} -I"$HEADER_DIR" "$HERE/eval_runner.cpp" -o "$RUNNER"

# Convert one source (root, relative path) to a cached float WAV; prints the WAV path or nothing.
convert() {
    local root="$1" rel="$2" src out
    if [ "$root" = music ]; then src="$MUSIC_ROOT/$rel"; else src="$APPLE_ROOT/$rel"; fi
    out="$WAV/$(md5 -q -s "$root/$rel").wav"
    if [ ! -s "$out" ]; then
        [ -f "$src" ] || return 0
        afconvert -f WAVE -d LEF32@$RATE -c 2 "$src" "$out" 2>/dev/null || { rm -f "$out"; return 0; }
    fi
    printf '%s' "$out"
}

MANIFEST="$SCRATCH/manifest_$RATE.tsv"
: > "$MANIFEST"
missing=0
while IFS=$'\t' read -r split root path bpm key kind source; do
    [ -z "$split" ] && continue
    case "$split" in \#*) continue ;; esac
    wav=""
    if [ "$root" = mix ]; then
        ok=1
        rest="$path"
        while :; do
            part="${rest%% + *}"
            w="$(convert apple "$part")"
            if [ -z "$w" ]; then ok=0; break; fi
            wav="${wav:+$wav + }$w"
            [ "$part" = "$rest" ] && break
            rest="${rest#* + }"
        done
        [ $ok = 1 ] || wav=""
    else
        wav="$(convert "$root" "$path")"
    fi
    if [ -z "$wav" ]; then missing=$((missing + 1)); continue; fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$split" "$root" "$path" "$bpm" "$key" "$kind" "$source" "$wav" >> "$MANIFEST"
done < "$LABELS"
echo "files: $(wc -l < "$MANIFEST" | tr -d ' ') usable, $missing missing on this machine"

ROWARGS=(${RUNOPTS[@]+"${RUNOPTS[@]}"})
[ -n "$ROWS" ] && ROWARGS+=(--rows "$ROWS")

if [ $STEMS = 0 ]; then
    "$RUNNER" "$MANIFEST" ${ROWARGS[@]+"${ROWARGS[@]}"}
    exit 0
fi

# Stem-aware path: music + mix rows only (single-instrument Apple Loops gain nothing from separation).
STEMS_CLI="$REPO/build/stems_cli"
[ -x "$STEMS_CLI" ] || { echo "build the stems_cli target first (cmake --build build --target stems_cli)" >&2; exit 1; }
"$RUNNER" "$MANIFEST" --write-mix "$MIXDIR"
SM="$SCRATCH/manifest_stems_$RATE.tsv"
: > "$SM"
JOBS="$SCRATCH/stem_jobs_$RATE.txt"
: > "$JOBS"
idx=-1
while IFS=$'\t' read -r split root path bpm key kind source wav; do
    idx=$((idx + 1))
    [ "$root" = apple ] && continue
    if [ "$root" = mix ]; then in="$MIXDIR/$idx.wav"; else in="${wav}"; fi
    sd="$STEMDIR/$(md5 -q -s "$root/$path")"
    [ -s "$sd/Vocals.wav" ] || printf '%s\0%s\0' "$in" "$sd" >> "$JOBS"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$split" "$root" "$path" "$bpm" "$key" "$kind" "$source" "$wav" "$sd" >> "$SM"
done < "$MANIFEST"
# Separate what is not cached yet, three files at a time (Vocals.wav is written last, so it marks a
# complete set).
xargs -0 -n 2 -P "${EVAL_STEM_JOBS:-3}" sh -c '"$0" "$1" "$2" > /dev/null 2>&1 || echo "stems failed: $1" >&2' "$STEMS_CLI" < "$JOBS"
# Keep only rows whose stems exist.
grep -v -e '^$' "$SM" | while IFS=$'\t' read -r split root path bpm key kind source wav sd; do
    [ -s "$sd/Vocals.wav" ] && printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$split" "$root" "$path" "$bpm" "$key" "$kind" "$source" "$wav" "$sd"
done > "$SM.ok"
mv "$SM.ok" "$SM"
echo "== analyse() on the stem rows (mix only)"
"$RUNNER" "$SM"
echo "== analyseWithStems()"
"$RUNNER" "$SM" --stems ${ROWARGS[@]+"${ROWARGS[@]}"}
