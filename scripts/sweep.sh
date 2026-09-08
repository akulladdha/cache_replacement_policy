#!/usr/bin/env bash
#
# Run the full policy sweep.
#
# For each of LRU, SRRIP and BRRIP: chase.c across a range of working-set
# sizes, plus stream.c and mixed.c at one size each. The working-set points
# are clustered around the 1 MiB L2 capacity, because that is where the
# policies actually diverge; far below capacity everything hits and far above
# capacity everything misses.
#
# Usage:
#     scripts/sweep.sh [outdir] [jobs]
#
# Environment overrides:
#     GEM5_ROOT     gem5 checkout                    (default ~/gem5)
#     GEM5_BIN      gem5 binary                      (default $GEM5_ROOT/build/ALL/gem5.opt)
#     L2_SIZE       L2 cache size                    (default 1MiB)
#     L2_ASSOC      L2 associativity                 (default 16)
#     WARMUP_INSTS  atomic-core warmup instructions  (default 0, meaning off)
#     CHASE_STEPS   pointer-chase iterations         (default 3000000)
#
# Simulation output is large and is written once per run, so keep it off a
# Windows-mounted filesystem if you are on WSL. The default outdir is under
# $HOME for that reason.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="${1:-$HOME/srrip-work/m5out}"
JOBS="${2:-8}"

GEM5_ROOT="${GEM5_ROOT:-$HOME/gem5}"
GEM5_BIN="${GEM5_BIN:-$GEM5_ROOT/build/ALL/gem5.opt}"
L2_SIZE="${L2_SIZE:-1MiB}"
L2_ASSOC="${L2_ASSOC:-16}"
WARMUP_INSTS="${WARMUP_INSTS:-0}"
CHASE_STEPS="${CHASE_STEPS:-3000000}"

BIN_DIR="${BIN_DIR:-$REPO_ROOT/benchmarks/bin}"
RUN_PY="$REPO_ROOT/configs/run.py"

if [[ ! -x "$GEM5_BIN" ]]; then
    echo "error: gem5 binary not found at $GEM5_BIN" >&2
    exit 1
fi
if [[ ! -x "$BIN_DIR/chase" ]]; then
    echo "error: benchmarks not built; run benchmarks/build.sh" >&2
    exit 1
fi

POLICIES=(LRU SRRIP BRRIP)
# Working sets in KiB.
#
# The spacing is deliberately uneven. A first pass on evenly spaced log points
# showed that the whole transition happens between 1 MiB and 3 MiB: at 1 MiB
# every policy still hits, and by 3 MiB every policy is above 98 percent miss
# rate. Spreading points evenly in log space puts almost none of them where
# the policies actually differ, so the sweep is dense from 1 MiB to 3 MiB and
# sparse elsewhere, with a few far-overcommitted points to show the tail.
CHASE_WS=(
    128 256 512 768
    1024 1152 1280 1408 1536 1664 1792
    2048 2304 2560 2816 3072
    4096 8192 16384
)

# Hot-set sizes for mixed.c, in KiB, against a fixed 2 MiB streaming array.
# These are all comfortably smaller than the 1 MiB L2, so a policy that
# protects them can hold the whole hot set resident across the scan.
MIXED_HOT=(64 128 256 512)

mkdir -p "$OUTDIR"

# Build the job list first, then hand the whole thing to xargs. Each line is
# one complete gem5 invocation.
JOBFILE="$(mktemp)"
trap 'rm -f "$JOBFILE"' EXIT

emit() {
    # emit <tag> <binary> <args...>
    local tag="$1"; shift
    local binary="$1"; shift
    local args=()
    for a in "$@"; do
        args+=(--bench-arg "$a")
    done
    # Skip a point that already has a completed stats.txt, so the sweep can be
    # extended with new working-set sizes without re-running everything. Set
    # FORCE=1 to re-run regardless.
    if [[ "${FORCE:-0}" != "1" && -s "$OUTDIR/$tag/stats.txt" ]]; then
        return
    fi
    echo "$GEM5_BIN --outdir=$OUTDIR/$tag $RUN_PY \
--policy $POLICY --binary $binary --l2-size $L2_SIZE --l2-assoc $L2_ASSOC \
--warmup-insts $WARMUP_INSTS ${args[*]}" >> "$JOBFILE"
}

for POLICY in "${POLICIES[@]}"; do
    for ws in "${CHASE_WS[@]}"; do
        emit "chase-${POLICY}-${ws}" "$BIN_DIR/chase" "$ws" "$CHASE_STEPS"
    done
    # stream.c is the control, at a single size well past the L2.
    emit "stream-${POLICY}-4096" "$BIN_DIR/stream" 4096 24

    # mixed.c: the swept variable is the hot-set size, with the streaming
    # array fixed at 2 MiB, twice the L2. The streaming array has to exceed
    # capacity or there is no eviction pressure at all and every policy
    # protects the hot set for free.
    for hot in "${MIXED_HOT[@]}"; do
        emit "mixed-${POLICY}-${hot}" "$BIN_DIR/mixed" 2048 "$hot" 10 4
    done
done

total="$(wc -l < "$JOBFILE")"
if [[ "$total" -eq 0 ]]; then
    echo "==> every point already has results; nothing to do (FORCE=1 to redo)"
    exit 0
fi
echo "==> $total simulations, $JOBS at a time, output under $OUTDIR"
echo "==> L2 $L2_SIZE ${L2_ASSOC}-way, warmup=$WARMUP_INSTS insts"

start=$SECONDS
# --halt-on-error 0 so one failed point does not abandon the rest of the
# sweep; scrape.py reports whatever is missing.
xargs -a "$JOBFILE" -P "$JOBS" -I{} bash -c '{} > /dev/null 2>&1 || echo "FAILED: {}" >&2'
echo "==> sweep finished in $((SECONDS - start))s"
