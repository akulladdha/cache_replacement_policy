#!/usr/bin/env bash
#
# Copy the SRRIP and BRRIP sources into a gem5 checkout, register them as
# SimObjects, add them to the build, and rebuild gem5.
#
# The script is idempotent: running it twice does not duplicate the
# registration entries, so it is safe to re-run after editing a policy.
#
# Usage:
#     scripts/install_policies.sh [gem5_root] [build_target]
#
# Defaults to ~/gem5 and the ALL build. Modern gem5 builds a single
# build/ALL/gem5.opt covering every ISA; older versions build per-ISA targets
# such as build/X86/gem5.opt. Pass the target explicitly if yours differs.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GEM5_ROOT="${1:-$HOME/gem5}"
BUILD_TARGET="${2:-ALL}"

RP_DIR="$GEM5_ROOT/src/mem/cache/replacement_policies"

if [[ ! -d "$RP_DIR" ]]; then
    echo "error: $RP_DIR not found; is $GEM5_ROOT a gem5 checkout?" >&2
    exit 1
fi

echo "==> copying policy sources into $RP_DIR"
cp "$REPO_ROOT/src/srrip_rp.hh"        "$RP_DIR/srrip_rp.hh"
cp "$REPO_ROOT/src/srrip_rp.cc"        "$RP_DIR/srrip_rp.cc"
cp "$REPO_ROOT/src/brrip_custom_rp.hh" "$RP_DIR/brrip_custom_rp.hh"
cp "$REPO_ROOT/src/brrip_custom_rp.cc" "$RP_DIR/brrip_custom_rp.cc"

# --- Registration 1: the SimObject declarations. ---------------------------
PY="$RP_DIR/ReplacementPolicies.py"
if grep -q "class SRRIPRP" "$PY"; then
    echo "==> ReplacementPolicies.py already declares SRRIPRP, refreshing it"
    # Drop everything from our marker onward, then re-append. Keeps the file
    # correct if the snippet changed.
    python3 - "$PY" <<'PYEOF'
import sys
path = sys.argv[1]
marker = "# --- srrip-gem5 project additions ---"
text = open(path).read()
if marker in text:
    text = text[: text.index(marker)].rstrip() + "\n"
    open(path, "w").write(text)
PYEOF
fi

{
    echo ""
    echo ""
    echo "# --- srrip-gem5 project additions ---"
    cat "$REPO_ROOT/src/ReplacementPolicies.py.snippet"
} >> "$PY"
echo "==> registered SRRIPRP and BRRIPCustomRP in ReplacementPolicies.py"

# --- Registration 2: the build. --------------------------------------------
# Two edits: add the SimObject names to the SimObject() call so the params
# headers get generated, and add each .cc as a Source so it gets compiled.
SC="$RP_DIR/SConscript"
python3 - "$SC" <<'PYEOF'
import sys

path = sys.argv[1]
text = open(path).read()

missing = [n for n in ("SRRIPRP", "BRRIPCustomRP") if f"'{n}'" not in text]
if missing:
    added = "".join(f", '{n}'" for n in missing)
    text = text.replace("'WeightedLRURP'])", "'WeightedLRURP'" + added + "])", 1)

for src in ("srrip_rp.cc", "brrip_custom_rp.cc"):
    line = f"Source('{src}')\n"
    if line not in text:
        text = text.replace(
            "Source('bip_rp.cc')\n", "Source('bip_rp.cc')\n" + line, 1
        )

open(path, "w").write(text)
PYEOF
echo "==> added sources and SimObject names to SConscript"

# --- Build. ----------------------------------------------------------------
echo "==> building build/$BUILD_TARGET/gem5.opt"
cd "$GEM5_ROOT"
scons "build/$BUILD_TARGET/gem5.opt" -j"$(nproc)"

echo "==> done"
