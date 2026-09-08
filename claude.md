# CLAUDE.md — SRRIP/BRRIP in gem5 (weekend build)

## What this is

Implement two cache replacement policies (SRRIP and BRRIP) in gem5, sweep working-set size,
and produce one plot showing the crossover between them. Ship a public repo by Sunday night.

Not DRRIP. The runtime set-dueling mechanism is deliberately out of scope, and the README says
so plainly. The crossover this project measures *is* the argument for DRRIP, and characterizing
it precisely scores better in an interview than a half-working dueling implementation.

**Done means:**
1. `plots/crossover.png` — miss rate vs working-set size, three lines (LRU, SRRIP, BRRIP),
   crossover visible.
2. `README.md` opens with that plot and a one-paragraph statement of what it shows.
3. Repo public, commits show progression.

**Budget: ~16 active hours across Fri evening / Sat / Sun.** gem5 is already built, which
removes the usual Friday-night dead time. That freed time goes to the stretch goals in
priority order, not to expanding the core.

---

## Working agreement

Weekend pace. Even looser than the full-scope charter.

- **No check-ins.** Act on everything. Naming, layout, plot styling, parameter values not
  pinned below are all Claude's call.
- **Hard cut lines are law.** Each block below has a wall-clock deadline and a documented
  fallback. When the clock hits, take the fallback and move. A shipped repo with one honest
  plot beats an unfinished repo with better intentions.
- **25-minute timebox on any single error.** Then fall back or work around it.
- **Commit at every green state.** `git commit -m "srrip compiles"` is a fine commit.
- **`NOTES.md` is the scratchpad.** Stats field names, commands that worked, dead ends. Never
  formatted. It becomes the README on Sunday.
- **Scope creep is the primary failure mode here**, not technical difficulty. If something
  isn't on the list below, it isn't happening this weekend.

---

## Phase −1 — Verify the existing build (15 min, do this first)

gem5 is built, but confirm it's built for the right target:

```bash
ls ~/gem5/build/
./build/X86/gem5.opt --version
```

**`build/X86/` must exist.** `X86` is the *simulated* ISA, not the host. If the existing build
is `build/RISCV/` (plausible given the rv32soc work), you have two options:

- Rebuild X86 in the background now — `scons build/X86/gem5.opt -j$(nproc)` — and do Friday's
  benchmark work while it compiles. Costs nothing but wall clock.
- Or stay on RISCV and cross-compile the benchmarks with `riscv64-linux-gnu-gcc -static`.
  Works fine, but adds a toolchain dependency for no benefit.

Prefer X86. Host-native `gcc -static` is one less thing to debug.

Smoke test before going further:

```bash
./build/X86/gem5.opt configs/deprecated/example/se.py \
    -c tests/test-progs/hello/bin/x86/linux/hello
grep -i "simInsts\|Misses" m5out/stats.txt | head
```

If that produces stats, the environment is good.

---

## Friday evening (3h) — benchmarks and paper

### Read (45 min)

RRIP paper, sections 3 and 4 only. Skip the rest. You need exactly two ideas:

- **RRPV (Re-Reference Prediction Value)** — a small counter per cache line, usually 2 bits,
  predicting how soon that line will be used again. 0 means "reuse expected very soon."
  3 means "far away, or never." Eviction picks a line with RRPV 3.
- **SRRIP vs BRRIP differ only in where new lines are inserted.** SRRIP inserts at 2
  ("probably not soon, but give it one chance"). BRRIP inserts at 3 most of the time
  ("evict me first") and at 2 only occasionally. Everything else about the two is identical.

### Write the microbenchmarks (2h)

Three C files in `benchmarks/`, all compiled `-static`:

- **`stream.c`** — sequential scan of an array, repeated. Cache-hostile in a way no policy
  helps with (nothing is reused before eviction). Establishes the floor.
- **`chase.c`** — pointer chase over a randomly permuted linked list sized by `argv[1]` in KB.
  This is the workhorse. Sweeping its working set from well under the LLC to well over it is
  the entire experiment.
- **`mixed.c`** — alternates between a small hot loop and a large streaming phase. This is
  where insertion policy matters most, because a naive policy lets the streaming phase evict
  the hot data.

Requirements:
- Working-set size comes from `argv`, not a `#define`. You'll sweep it, and recompiling per
  point is a waste of the weekend.
- Each runs in well under a minute natively.
- `-O2 -static`. **`-static` is non-negotiable** — gem5's Syscall Emulation mode (SE mode)
  fakes the OS rather than booting Linux, and it handles the dynamic linker badly. Dynamically
  linked binaries fail in confusing ways that will eat an hour.
- `benchmarks/build.sh` builds all three.

Verify each one runs natively and under gem5 before going to bed.

**Cut line 11pm:** if `chase.c` is misbehaving, ship with `stream.c` and `mixed.c` and add a
simple array-traversal-with-stride benchmark instead. Don't debug pointer chasing at midnight.

---

## Saturday morning (4h) — config harness ⚠️ HIGHEST RISK

This block is the one most likely to eat the weekend. It's plumbing, not architecture, and it
has no intellectual payoff. Treat the cut line seriously.

**Two-level hierarchy, not three.** Your policy goes at L2, which you call the LLC in the
writeup. The three-level classic hierarchy is a meaningful chunk of config work and buys
nothing at this scope.

The gem5 standard library ships `PrivateL1PrivateL2CacheHierarchy`, but it does not expose the
replacement policy as a parameter. Don't fight it:

```bash
cp ~/gem5/src/python/gem5/components/cachehierarchies/classic/\
private_l1_private_l2_cache_hierarchy.py configs/hierarchy.py
```

Then edit your copy to accept `l2_replacement_policy` and pass it through to the L2 cache
constructor. This is a ~10-line change and much faster than writing a hierarchy from scratch.

`configs/run.py` needs:
- CLI flags: `--policy`, `--l2-size`, `--l2-assoc`, `--binary`, `--bench-arg`
- `TimingSimpleCPU` (no O3 this weekend — MPKI is a cache metric and doesn't need
  out-of-order execution to be meaningful)
- SE mode, single core
- Baseline L2: 1MB, 16-way. Small on purpose, so `chase.c` can overrun it quickly and each
  simulation stays short.

**Acceptance gate — do not proceed past this:** the same binary run with `--policy LRU` and
`--policy Random` must produce measurably different `l2.overallMisses`. If those numbers are
identical, your policy flag isn't reaching the cache, and every result you generate afterward
will be meaningless.

Find the real stats field names in `m5out/stats.txt` and write them into `NOTES.md`. They drift
between gem5 releases; don't trust any name from memory, including the ones in this document.

**Cut line 12:30pm:** if the stdlib hierarchy is fighting you, fall back to
`configs/deprecated/example/se.py`, which takes `--l2_size`, `--l2_assoc`, and `--caches`
directly. It's older and uglier, and it works. Note the choice in `NOTES.md` and move on.

---

## Saturday afternoon (4h) — SRRIP

Four files touched:

```
src/mem/cache/replacement_policies/srrip_rp.hh
src/mem/cache/replacement_policies/srrip_rp.cc
src/mem/cache/replacement_policies/ReplacementPolicies.py   # register the SimObject
src/mem/cache/replacement_policies/SConscript                # add the .cc to the build
```

Read `lru_rp.cc` and `base.hh` first to learn the interface shape. gem5 also ships its own RRIP
implementation — read it to understand the interface if you're stuck, but write yours
independently. Having built it is the entire point of the project.

Implement:

- **`SRRIPReplData`** — subclass of `ReplacementData` holding one RRPV field.
- **`reset()`** — fires on **insertion**. Set RRPV = 2.
- **`touch()`** — fires on a **hit**. Set RRPV = 0.
- **`invalidate()`** — set RRPV to max.
- **`getVictim()`** — scan candidates for RRPV == max. If none found, increment every
  candidate's RRPV and rescan. Repeat until a victim exists.
- **`numRRPVBits`** as a Python param, default 2.

**The single most important detail in this project:** `reset()` is insertion and `touch()` is a
hit. Reversing them produces a policy that compiles, runs, and generates entirely plausible
wrong numbers. Both SRRIP and BRRIP work by changing *where lines enter the cache*, not by
changing how hits are handled. If you get this backwards you will not notice until someone asks
you about it in an interview.

Rebuild is incremental — under a minute with ccache after touching one `.cc`.

**Acceptance:** `--policy SRRIP` runs, and on `mixed.c` it produces a different miss count than
LRU. Direction doesn't matter yet; different means the policy is live.

**Cut line 6pm.** SRRIP has no fallback. It's the project. If it isn't working by 6pm, drop
every stretch goal and spend Sunday morning on it.

---

## Sunday morning (3h) — BRRIP and data

### BRRIP (45 min)

Copy `srrip_rp.*` to `brrip_rp.*` and change exactly one thing: in `reset()`, insert at RRPV =
max with high probability, and at RRPV = max−1 with probability `btp`, default 1/32. Use gem5's
`random_mt` rather than `rand()`.

That's the whole diff. Register it the same way.

Why this works: when the working set exceeds the cache, inserting most lines as evict-me-first
means they cycle through fast, while the small retained fraction survives long enough to
actually be reused. It's thrash resistance by deliberately refusing to cache most of what you
see.

### Sweep and scrape (2h)

`scripts/sweep.sh`: for each policy in {LRU, SRRIP, BRRIP}, for each `chase.c` working set in
{256KB, 512KB, 1MB, 2MB, 4MB, 8MB, 16MB}, plus `stream.c` and `mixed.c` at one size each.

That's 3 × 9 = 27 runs. Run 8 in parallel with `xargs -P8`. Should finish in well under an hour.

`scripts/scrape.py`: walk the output directories, pull the stats fields you recorded in
`NOTES.md`, emit `results/data.csv` with columns `policy, benchmark, working_set_kb, misses,
insts, mpki, miss_rate`.

MPKI = misses per thousand instructions = `misses / (insts / 1000)`. It's the standard metric
for replacement policies because it normalizes across workloads of different lengths.

---

## Sunday afternoon (2h) — plot and README

**One plot.** `plots/crossover.png`: x-axis working-set size (log scale), y-axis L2 miss rate,
three lines. The L2 capacity marked with a vertical line. That single image is what most
viewers will look at, and it should make the story obvious without reading anything.

Expected shape: all three converge below capacity, LRU and SRRIP degrade sharply past capacity,
BRRIP degrades more gently. Where BRRIP crosses below SRRIP is the crossover.

**If the crossover doesn't appear:** first check that the working set actually exceeds L2 by a
comfortable margin (try 32MB). Second, check that `btp` isn't so low that BRRIP effectively
never retains anything. If it still doesn't appear, report what you actually measured and
discuss why. A negative result honestly analyzed is a real README. A fabricated crossover is a
disaster waiting for the interview.

**README structure:**

1. The plot, with a one-paragraph statement of what it shows and the measured numbers.
2. **Scope, stated plainly.** SRRIP and BRRIP implemented; DRRIP's set-dueling not built. Say
   that the crossover motivates DRRIP and describe in two sentences how dueling would resolve
   it. Bounded claims read as maturity; vague ones collapse under questioning.
3. **Methodology, including the shortcuts.** Two-level hierarchy with the policy at L2.
   `TimingSimpleCPU`, not out-of-order. Microbenchmarks, not SPEC.
4. **The warmup caveat** (if you skip the stretch goal below). Write it like this: caches start
   cold, so absolute miss rates include compulsory misses — misses on data that was never in
   the cache to begin with, which no replacement policy can prevent. Since every policy takes
   the identical set of compulsory misses, the *deltas between policies* remain valid even
   though absolute numbers are inflated.

That last paragraph is worth writing carefully. It converts a corner-cut into evidence that you
understand why warmup normally matters, and warmup is the methodology question an architecture
interviewer is most likely to ask.

---

## Stretch goals (only if core is done and committed)

Strictly in this order. Each is independently shippable. Do not start one until the previous is
committed and pushed.

### S1 — Warmup harness (~1.5h) — highest value

Replaces the caveat above with an actual solution. Use `SimpleSwitchableProcessor`: start in
`ATOMIC` with caches attached (fast, warms cache contents functionally), switch to
`TimingSimpleCPU` after ~50M instructions, call `m5.stats.reset()` at the switch, measure the
window after.

This is the single highest-value addition because it's the thing you'll be asked about.

### S2 — GAPBS BFS (~1.5h)

One real workload. `git clone` the GAP Benchmark Suite, build BFS with OpenMP disabled and
`-static`, run it on a small generated graph. Irregular pointer-chasing over a graph is exactly
the access pattern these policies target, and "evaluated on a graph workload" reads better than
"evaluated on microbenchmarks I wrote."

Skip PageRank. One real workload makes the point.

### S3 — Associativity sweep (~1h)

Re-run the baseline working-set point at 4-, 8-, and 16-way. Cheap, adds a second plot, shows
you thought about the design space. Lowest value of the three because it doesn't answer a
question anyone asked.

---

## Repo layout

```
srrip-gem5/
├── README.md
├── NOTES.md
├── src/                  # srrip_rp.*, brrip_rp.*, and the two registration diffs
├── configs/
│   ├── run.py
│   └── hierarchy.py      # modified copy of the stdlib hierarchy
├── benchmarks/
│   ├── build.sh
│   ├── stream.c
│   ├── chase.c
│   └── mixed.c
├── scripts/
│   ├── sweep.sh
│   ├── scrape.py
│   └── plot.py
├── results/data.csv      # commit this
└── plots/crossover.png
```

Commit `results/data.csv` and the plot. Someone skimming the repo should see the result without
running anything.

---

## Traps

1. **`reset()` = insertion, `touch()` = hit.** Reversing them yields a plausible, wrong policy.
2. **`-static` or SE mode fails cryptically.**
3. **`build/X86/`** regardless of host ISA. Confirm before starting.
4. **Stats field names drift between gem5 versions.** Confirm in `stats.txt`, record in `NOTES.md`.
5. **The LRU-vs-Random gate on Saturday morning is not optional.** Skipping it risks generating
   an entire dataset where the policy flag was silently ignored.
6. **gem5 ships an RRIP implementation.** Read it if stuck; write yours independently.
7. **`SConscript` registration is easy to forget** and produces a confusing "SimObject not
   found" error rather than a build failure.
8. **Don't build on `/mnt/c/`.** WSL2's 9p filesystem bridge makes gem5 rebuilds 5–10x slower.
   This differs from rv32soc, which lives on `/mnt/c` because it needs Windows-side Vivado.

---

## Resume line

> Implemented SRRIP and BRRIP cache replacement policies in gem5's C++ memory hierarchy;
> characterized the insertion-policy crossover across working-set sizes that motivates
> adaptive (DRRIP-style) selection.

Do not claim DRRIP. The scope statement in the README is doing real work — an interviewer who
sees precise, bounded claims trusts the rest of the repo more, not less.