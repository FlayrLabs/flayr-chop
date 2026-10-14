#!/usr/bin/env bash
# Runs the fuzz targets built with -DFLAYR_CHOP_BUILD_FUZZERS=ON (see cmake/Fuzzers.cmake).
#
#   scripts/fuzz.sh BUILD_DIR [SECONDS_PER_TARGET] [TARGET...]
#
# Targets: sample_load state_restore model_parser stem_cache (default: all). Each target gets its
# generated seeds plus the committed regression inputs (tests/fuzz/regressions/<target>), and a
# corpus that persists between runs under $FLAYR_CHOP_FUZZ_DIR (default: a folder in $TMPDIR).
# A crash, timeout or memory blow-up stops that target and leaves the input in <dir>/<target>/artifacts.
set -euo pipefail

BUILD="${1:?usage: scripts/fuzz.sh BUILD_DIR [SECONDS] [TARGET...]}"
SECONDS_PER_TARGET="${2:-60}"
shift $(( $# >= 2 ? 2 : $# ))
TARGETS=("$@")
if [ ${#TARGETS[@]} -eq 0 ]; then
    TARGETS=(sample_load state_restore model_parser stem_cache)
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${FLAYR_CHOP_FUZZ_DIR:-${TMPDIR:-/tmp}/flayr-chop-fuzz}"
mkdir -p "$WORK"

# Per-allocation limit for ASan (libFuzzer also gets -malloc_limit_mb); RSS limit for both engines.
MALLOC_LIMIT_MB="${FLAYR_CHOP_FUZZ_MALLOC_MB:-512}"
export ASAN_OPTIONS="max_allocation_size_mb=${MALLOC_LIMIT_MB}:allocator_may_return_null=0:detect_leaks=0:handle_abort=1:${ASAN_OPTIONS:-}"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:suppressions=$(cd "$(dirname "$0")/.." && pwd)/tests/fuzz/ubsan.supp:${UBSAN_OPTIONS:-}"

status=0
for t in "${TARGETS[@]}"; do
    bin="$BUILD/fuzz_$t"
    [ -x "$bin" ] || bin="$(find "$BUILD" -type f -perm -u+x -name "fuzz_$t" | head -1)"
    if [ -z "$bin" ] || [ ! -x "$bin" ]; then
        echo "fuzz_$t not found under $BUILD" >&2
        exit 2
    fi
    case "$t" in
        sample_load)   dict=audio.dict; maxlen=65536 ;;
        state_restore) dict=state.dict; maxlen=32768 ;;
        model_parser)  dict=model.dict; maxlen=262144 ;;
        stem_cache)    dict=audio.dict; maxlen=131072 ;;
        *) echo "unknown target $t" >&2; exit 2 ;;
    esac
    dir="$WORK/$t"
    mkdir -p "$dir/corpus" "$dir/artifacts"
    rm -rf "$dir/seeds"
    (cd "$dir" && "$bin" -write_seeds=seeds >/dev/null)
    seeds=("$dir/seeds")
    if [ -d "$ROOT/tests/fuzz/regressions/$t" ]; then
        seeds+=("$ROOT/tests/fuzz/regressions/$t")
    fi
    echo "== fuzz_$t for ${SECONDS_PER_TARGET}s (corpus $dir/corpus)"
    if ! (cd "$dir" && "$bin" -max_total_time="$SECONDS_PER_TARGET" -timeout=20 -rss_limit_mb=2048 \
            -malloc_limit_mb="$MALLOC_LIMIT_MB" -max_len="$maxlen" -dict="$ROOT/tests/fuzz/dict/$dict" \
            -artifact_prefix="$dir/artifacts/" -print_final_stats=1 "$dir/corpus" "${seeds[@]}" 2>&1 | tee "$dir/last-run.log" | tail -n 40); then
        status=1
    fi
    if [ -n "$(ls -A "$dir/artifacts")" ]; then
        echo "!! fuzz_$t found something: $(ls "$dir/artifacts")" >&2
        status=1
    fi
done
exit $status
