#!/usr/bin/env bash
#
# Fetch and build GAPBS BFS for gem5 SE mode, and generate the input graphs.
#
# GAPBS is the GAP Benchmark Suite (Beamer, Asanovic and Patterson). BFS over
# a Kronecker graph is the suite's standard irregular workload: random
# neighbour lookups, a sequential walk of each vertex's edge list, and a
# frontier and parent array that are reused across levels. That mix is what
# makes it a useful complement to the single-pattern microbenchmarks.
#
# Built serial (no OpenMP) and -static, because SE mode runs one thread and
# handles the dynamic loader badly.
#
# Graphs are generated natively and written as serialized .sg files, so the
# simulation reads a finished graph instead of spending most of its
# instructions building one.
#
# Usage:
#     scripts/gapbs_setup.sh [workdir]
#
# Defaults to ~/srrip-work. Produces <workdir>/gapbs/bfs and
# <workdir>/graphs/kron<scale>.sg for scales 10 to 18.
set -euo pipefail

WORK="${1:-$HOME/srrip-work}"
# Pinned so every graph and every instruction count reproduces. Generation
# is deterministic for a given GAPBS version, not across versions.
GAPBS_COMMIT="${GAPBS_COMMIT:-941462c}"
SCALES="${SCALES:-10 11 12 13 14 15 16 17 18}"

mkdir -p "$WORK"
cd "$WORK"

if [[ ! -d gapbs ]]; then
    git clone https://github.com/sbeamer/gapbs.git
fi
cd gapbs
git checkout -q "$GAPBS_COMMIT"

# Overriding CXX_FLAGS replaces the Makefile's own, which is how -static gets
# in. SERIAL=1 keeps -fopenmp out.
make bfs converter SERIAL=1 CXX_FLAGS="-std=c++11 -O3 -Wall -static"

mkdir -p "$WORK/graphs"
for scale in $SCALES; do
    out="$WORK/graphs/kron${scale}.sg"
    if [[ ! -s "$out" ]]; then
        # Kronecker graph, 2^scale vertices, GAPBS default degree 16.
        ./converter -g "$scale" -b "$out"
    fi
done

ls -l "$WORK/graphs"
