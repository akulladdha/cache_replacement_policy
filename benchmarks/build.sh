#!/usr/bin/env bash
# Build all three microbenchmarks.
#
# -static is not optional. gem5 Syscall Emulation mode fakes the operating
# system instead of booting one, and its handling of the dynamic loader is
# poor enough that dynamically linked binaries fail in ways that look like
# simulator bugs. Static binaries sidestep the whole problem.
set -euo pipefail

cd "$(dirname "$0")"
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O2 -static -Wall}

mkdir -p bin
for src in stream.c chase.c mixed.c; do
    out="bin/${src%.c}"
    echo "  CC  $src -> $out"
    $CC $CFLAGS -o "$out" "$src"
done

echo "built:"
ls -l bin
