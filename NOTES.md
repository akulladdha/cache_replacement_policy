# NOTES

Working scratchpad. Commands that worked, field names, dead ends, decisions.
Unformatted on purpose.

## Environment

- gem5 checkout at `~/gem5`, on the WSL2 native filesystem, not `/mnt/c`.
  Building on `/mnt/c` goes through the 9p bridge and is far slower.
- gem5 commit `cbf0eae213`.
- Host gcc 15.2.0. gem5 warns it officially supports up to 14.2. Built fine
  anyway; no errors traceable to the compiler version.
- Binary is `build/ALL/gem5.opt`, **not** `build/X86/gem5.opt`. Modern gem5
  builds one binary covering every ISA via Kconfig. The ISA is chosen at
  config time instead, with `isa=ISA.X86` on the processor. Anything written
  against the old per-ISA build layout needs adjusting.
- The repo itself lives on the Windows Desktop under OneDrive. Simulation
  output goes to `~/srrip-work/m5out` in the WSL home directory so that
  thousands of intermediate files never touch OneDrive. Only `results/` and
  `plots/` come back.
- `git init` fails on the OneDrive path from inside WSL with
  `could not write config file .git/config: Permission denied`. Running git
  from Windows against the same directory works fine. All commits made from
  the Windows side.
- Python on Ubuntu is PEP 668 managed, so `pip install matplotlib` refuses.
  Used a venv at `~/srrip-work/venv`.

## Stats field names (confirmed, do not trust from memory)

Read out of a real `stats.txt` from this build:

```
board.cache_hierarchy.l2-cache-0.overallMisses::total
board.cache_hierarchy.l2-cache-0.overallAccesses::total
board.cache_hierarchy.l2-cache-0.overallMissRate::total
simInsts
simSeconds
```

The `board.cache_hierarchy.` prefix comes from the stdlib board and hierarchy
object names. Rename the hierarchy attribute and these all change.

## Acceptance gate (Saturday morning, the one that is not optional)

Same binary, `chase 4096`, 1 MiB 16-way L2:

| policy | overallMisses | simInsts |
|---|---|---|
| LRU | 1581765 | 6117956 |
| Random | 1550485 | 6117956 |

Different miss counts, identical instruction counts. The `--policy` flag
reaches the cache. Gate passed, safe to generate a dataset.

Second gate, after the policies were compiled in, same configuration:

| policy | overallMisses |
|---|---|
| LRU | 1581765 |
| Random | 1550485 |
| SRRIP | 1582343 |
| BRRIP | 1578749 |

All four distinct. SRRIP lands essentially on top of LRU here, which is the
expected result at 4x overcommit: SRRIP has scan resistance but no thrash
resistance, so on a cyclic reference pattern much larger than the cache it
behaves like LRU. BRRIP is the only one that improves. That is the whole
thesis of the project showing up in the very first four-row table.

## Implementation notes

- `reset()` is **insertion**, `touch()` is a **hit**. Written at the top of
  `srrip_rp.hh` in capital letters because reversing them compiles, runs, and
  produces plausible wrong numbers.
- `invalidate()` is non-const in `base.hh`; `touch()` and `reset()` are const.
  There are also `(replacement_data, PacketPtr)` overloads which default to
  forwarding to the single-argument versions. Not needed here.
- gem5 already ships `brrip_rp.*` and a `BRRIPRP` SimObject. Mine are named
  `brrip_custom_rp.*` and `BRRIPCustomRP` so both live in the same binary and
  nothing upstream is clobbered. SRRIP had no name collision.
- `BRRIPCustom` inherits from `SRRIP` and overrides only `reset()`. Copying
  the file would have worked equally well, but inheritance states the actual
  claim: the only difference between the two policies is where lines enter.
- The old global `random_mt` no longer exists in this gem5. The current API
  is `Random::RandomPtr rng = Random::genRandom();` then
  `rng->random<unsigned>(1, 100)`. Found by grepping `random_rp.cc`.
- `getVictim()` uses the aging-delta shortcut rather than literal
  increment-and-rescan. One pass finds the largest RRPV present, then every
  candidate is aged by `maxRRPV - largest`. Same victim, same resulting
  counters, one pass instead of up to four.
- Added a `valid` flag to the replacement data so an empty way is taken
  before a way holding live data. Not part of the paper; it matters while
  the cache is still filling.

## Validation against gem5's own RRIP

`--policy SRRIP_REF` and `--policy BRRIP_REF` in `configs/run.py` select
gem5's upstream `BRRIPRP` instead of mine. Not part of the dataset, purely a
correctness check.

Result: **bit-identical miss counts** on both workloads.

| run | my policy | gem5 reference |
|---|---|---|
| mixed, SRRIP | 444024 | 444024 |
| mixed, BRRIP | 444026 | 444026 |
| chase 1280 KiB, SRRIP | 2817337 | 2817337 |
| chase 1280 KiB, BRRIP | 1218098 | 1218098 |

The comparison only works with `hit_priority=True` on gem5's side. Its
default is `False`, which is the paper's *frequency priority* variant: a hit
decrements the RRPV by one instead of setting it to zero. That is a genuinely
different policy, and comparing against it made my correct implementation look
broken by about 26k hits. Worth remembering: when validating against a
reference, check you configured the reference to be the same algorithm.

Also confirms gem5 implements SRRIP as BRRIP with `btp=100`, which matches
how the paper describes the relationship.

## Two benchmark bugs that produced convincing garbage

Both of these produced clean-looking numbers that were meaningless. Neither
showed up as an error.

**1. gcc collapsed the repeated read passes.** `stream.c` and `mixed.c` both
re-read an array N times and accumulate into a sum. That is a pure reduction
with no side effects, so at `-O2` gcc is entitled to run the loop once and
multiply, and it does. The benchmarks therefore had no reuse at all, so every
policy scored identically and it looked like a boring-but-plausible result.
Fixed with an empty `__asm__ __volatile__("" ::: "memory")` between passes.
Caught by noticing the L2 access count was far lower than the arithmetic said
it should be.

**2. A sequentially scanned hot set made LRU and SRRIP identical to the
unit.** After fix 1, `mixed.c` still showed LRU and SRRIP producing
*bit-identical* miss counts at every hot-set size, while BRRIP differed by 2.
Exactly equal counts between two different policies is the tell: the victim
decisions never diverged. Walking the hot set in a fixed scattered order
instead of index order fixed it, and the three policies then separated
cleanly. Same working set, same reuse pattern, same instruction count, just
without the index-order structure.

The general lesson, and the reason the LRU-vs-Random gate exists: a
replacement-policy experiment fails silently. There is no crash and no
warning, just numbers that are wrong in a believable way.

## What mixed.c actually shows

Not what was planned. The intent was "insertion policy protects the hot set,
so BRRIP wins". The measurement says the opposite: **BRRIP is consistently
worse on mixed.c, and worse by more as the hot set grows.**

| hot set | LRU | SRRIP | BRRIP |
|---|---|---|---|
| 64 KiB | 0.9129 | 0.9131 | 0.9151 |
| 128 KiB | 0.8436 | 0.8444 | 0.8567 |
| 256 KiB | 0.7408 | 0.7424 | 0.7735 |
| 512 KiB | 0.6122 | 0.6189 | 0.6695 |

This is right, and it is more useful than the planned result. BRRIP inserts
almost everything at the distant interval, so a hot set that is being brought
back into the cache cannot accumulate: each newly inserted hot line is itself
the next eviction candidate, and only the small `btp` fraction survives long
enough to be hit and promoted. A policy that refuses to retain most of what it
sees cannot build up a working set it should have kept.

So chase.c and mixed.c are the two sides of the same argument, which is the
argument for DRRIP. Kept both.

## Config decisions

- Two-level hierarchy. The policy sits at L2, which is the last level, so L2
  is what the writeup calls the LLC.
- Copied the stdlib `PrivateL1PrivateL2CacheHierarchy` into
  `configs/hierarchy.py` and edited it. The stdlib version hard-codes L2
  associativity at 4 and gives no way to reach the replacement policy.
  Converting the relative `....isas` style imports to absolute `gem5.*`
  imports was the only other change needed to run it as a standalone config.
- **Prefetchers off, both L1D and L2.** The stdlib attaches a
  `StridePrefetcher` to each. An L1 prefetcher generates its own requests
  down to L2, so with it on, part of the L2 access stream being measured was
  generated by the prefetcher rather than the benchmark. The L2 stats even
  show a `::cache_hierarchy.l1d-cache-0.prefetcher` requestor column, which
  is how this got noticed. Turning them off is the difference between
  measuring the replacement policy and measuring the prefetcher.
- `TimingSimpleCPU`, single core, SE mode. No O3: MPKI is a property of the
  access stream, and an in-order core generates the same stream far faster.
- Baseline L2 1 MiB, 16-way, deliberately small so `chase.c` overruns it
  quickly and each simulation stays short.

## Benchmarks

- All `-O2 -static`. Static is not negotiable under SE mode.
- `chase.c` nodes are padded to 64 bytes so one node occupies exactly one
  cache line and a request for N KiB touches exactly N KiB of distinct lines.
- Sattolo shuffle rather than Fisher-Yates, because Sattolo guarantees a
  single cycle covering every node. Fisher-Yates can produce several short
  disjoint cycles, in which case the chase would only ever touch one of them
  and the working set would silently be smaller than requested.
- Step count is fixed and independent of working-set size, so every sweep
  point executes very nearly the same number of instructions and MPKI stays
  comparable across the x axis.
- Own xorshift PRNG rather than `rand()`, so the permutation is identical
  across libc versions and runs reproduce.

## Sweep

`scripts/sweep.sh`, 39 runs, 8 at a time via `xargs -P8`.

Working-set points are clustered around the 1 MiB capacity
(128, 256, 512, 768, 1024, 1536, 2048, 3072, 4096, 8192, 16384 KiB) rather
than spread evenly in log space. The first smoke tests showed that at 4x
overcommit every policy sits above 96 percent miss rate and the lines are
almost on top of each other. The interesting region is within a factor of
about two of capacity, so that is where the points went.

## Warmup (stretch goal S1)

`--warmup-insts N` on `configs/run.py`. Starts on `CPUTypes.ATOMIC` via
`SimpleSwitchableProcessor`, switches to `TIMING` after N instructions, calls
`m5.stats.reset()` at the switch. Wired through
`Simulator(on_exit_event={ExitEvent.MAX_INSTS: handler})` plus
`simulator.schedule_max_insts(N)`. The handler must `yield` so the simulator
treats it as a generator and carries on.

Order inside the handler matters: `processor.switch()` first, then
`m5.stats.reset()`. Reset first and the switch itself lands in the measured
window.

Verified it actually works rather than assuming: no-warmup run of
`chase 1280` reports 8,336,428 `simInsts`, the warmed run reports 6,336,340,
and 8,336,428 - 6,336,340 = 2,000,088, which is the 2,000,000 warmup plus the
few instructions it takes to reach the exit event. gem5 also prints
"switching cpus".

Warmup sweep lives in a separate output tree (`~/srrip-work/m5out-warm`) and
a separate CSV so both datasets survive.

Findings: below capacity the miss rate drops more than 10x (0.0036 -> 0.0001),
confirming those were nearly all compulsory misses. Past capacity LRU goes to
a flat 1.0000. The SRRIP-minus-BRRIP gap moves by under 0.007 everywhere,
so the cold-cache deltas were trustworthy.

Caveat recorded in the README: at 16 MiB the fixed 4M-instruction warmup no
longer covers the whole list-building phase, so that point reads 0.9716
instead of 1.0000. Does not touch the transition region.
