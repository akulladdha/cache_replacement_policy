#!/usr/bin/env bash
#
# Run GAPBS BFS under every policy across a range of graph sizes.
#
# The microbenchmarks each isolate one access pattern. This is the check on
# whether the conclusions drawn from them survive contact with a real
# workload that mixes several patterns at once.
#
# Measurement window. Each run does five BFS trials from different source
# vertices. Loading the graph and the first trial run on the atomic core as
# warmup, then the stats are reset and the remaining four trials are
# measured on TimingSimpleCPU. So the measured window starts with the graph
# already in the cache hierarchy, as far as it fits, and contains no
# cold-start misses.
#
# The warmup length is the instruction count of a one-trial run, measured
# once per graph in a calibration pass. BFS here is single-threaded and
# deterministic, so its instruction stream does not depend on the cache
# policy and one calibration serves every policy.
#
# Usage:
#     scripts/bfs_sweep.sh [outdir] [jobs]
#
# Needs scripts/gapbs_setup.sh to have been run first.
#
# Environment overrides:
#     WORK       where gapbs/ and graphs/ live          (default ~/srrip-work)
#     GEM5_ROOT  gem5 checkout                          (default ~/gem5)
#     POLICIES   space-separated policy list            (default LRU SRRIP BRRIP DRRIP)
#     SCALES     Kronecker scales to run                (default 10 to 18)
#     TRIALS     measured trials after the warmup one   (default 4)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="${1:-$HOME/srrip-work/m5out-bfs}"
JOBS="${2:-8}"

WORK="${WORK:-$HOME/srrip-work}"
GEM5_ROOT="${GEM5_ROOT:-$HOME/gem5}"
GEM5_BIN="${GEM5_BIN:-$GEM5_ROOT/build/ALL/gem5.opt}"
POLICIES=(${POLICIES:-LRU SRRIP BRRIP DRRIP})
SCALES=(${SCALES:-10 11 12 13 14 15 16 17 18})
TRIALS="${TRIALS:-4}"

BFS="$WORK/gapbs/bfs"
GRAPHS="$WORK/graphs"
RUN_PY="$REPO_ROOT/configs/run.py"

if [[ ! -x "$BFS" ]]; then
    echo "error: $BFS not found; run scripts/gapbs_setup.sh" >&2
    exit 1
fi

mkdir -p "$OUTDIR/calibrate"

# The "=" form matters. GAPBS flags start with "-", and argparse would
# otherwise read "--bench-arg -f" as a missing value followed by a new flag.
bench_args() {
    # bench_args <graph> <trials>
    echo "--bench-arg=-f --bench-arg=$1 --bench-arg=-n --bench-arg=$2"
}

# --- Pass 1: calibrate the warmup length per graph. ------------------------
CALJOBS="$(mktemp)"
trap 'rm -f "$CALJOBS" "${JOBFILE:-}"' EXIT
for scale in "${SCALES[@]}"; do
    dir="$OUTDIR/calibrate/kron$scale"
    if [[ -s "$dir/stats.txt" ]]; then
        continue
    fi
    echo "$GEM5_BIN --outdir=$dir $RUN_PY --policy LRU --binary $BFS \
$(bench_args "$GRAPHS/kron$scale.sg" 1)" >> "$CALJOBS"
done
if [[ -s "$CALJOBS" ]]; then
    echo "==> calibrating $(wc -l < "$CALJOBS") graphs"
    xargs -a "$CALJOBS" -P "$JOBS" -I{} bash -c '{} > /dev/null 2>&1 || echo "FAILED: {}" >&2'
fi

# --- Pass 2: the measured runs. ---------------------------------------------
JOBFILE="$(mktemp)"
for scale in "${SCALES[@]}"; do
    graph="$GRAPHS/kron$scale.sg"
    warmup="$(awk '$1 == "simInsts" {print $2}' \
        "$OUTDIR/calibrate/kron$scale/stats.txt")"
    if [[ -z "$warmup" ]]; then
        echo "error: no calibration for kron$scale" >&2
        exit 1
    fi
    # Tag by graph file size in KiB, so the x axis can be read against the
    # L2 capacity the same way as chase.c. The file is the CSR offsets plus
    # the neighbour array; BFS also touches a 4-byte-per-vertex parent array
    # and a frontier, so the true footprint is somewhat larger.
    kib=$(( $(stat -c %s "$graph") / 1024 ))
    for policy in "${POLICIES[@]}"; do
        tag="bfs-${policy}-${kib}"
        if [[ "${FORCE:-0}" != "1" && -s "$OUTDIR/$tag/stats.txt" ]]; then
            continue
        fi
        echo "$GEM5_BIN --outdir=$OUTDIR/$tag $RUN_PY --policy $policy \
--binary $BFS --warmup-insts $warmup \
$(bench_args "$graph" $((TRIALS + 1)))" >> "$JOBFILE"
    done
done

total="$(wc -l < "$JOBFILE")"
if [[ "$total" -eq 0 ]]; then
    echo "==> every point already has results; nothing to do (FORCE=1 to redo)"
    exit 0
fi
echo "==> $total simulations, $JOBS at a time, output under $OUTDIR"
start=$SECONDS
xargs -a "$JOBFILE" -P "$JOBS" -I{} bash -c '{} > /dev/null 2>&1 || echo "FAILED: {}" >&2'
echo "==> sweep finished in $((SECONDS - start))s"
