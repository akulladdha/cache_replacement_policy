"""
Run one microbenchmark under one L2 replacement policy.

Usage (from the gem5 root, with this repo checked out alongside):

    build/ALL/gem5.opt --outdir=<out> path/to/configs/run.py \
        --policy SRRIP --binary path/to/benchmarks/bin/chase --bench-arg 4096

Design choices, all deliberate and all documented in the README:

* Two-level hierarchy, not three. The policy under test sits at L2, which is
  the last level here and is what the writeup calls the LLC. A third level
  would be a meaningful chunk of extra config work and would not change the
  comparison.
* ``TimingSimpleCPU``, not out-of-order. MPKI is a property of the cache and
  the access stream. An in-order core produces the same stream and simulates
  far faster, which is what makes a 27-point sweep practical.
* Syscall Emulation mode, single core, statically linked binaries.
* The L2 prefetcher is off. With it on, the miss counts would partly reflect
  prefetcher accuracy rather than the replacement decision.
"""

import argparse
import os
import sys

import m5
from m5.objects import (
    BRRIPRP,
    LRURP,
    RandomRP,
)

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from hierarchy import ConfigurableL2Hierarchy


def build_replacement_policy(name: str, num_bits: int, btp: int):
    """
    Map a --policy string onto a replacement policy SimObject.

    SRRIPRP and BRRIPCustomRP are the two policies implemented for this
    project. LRURP and RandomRP are gem5 built-ins: LRU is the baseline the
    plot compares against, and Random exists purely so the harness can be
    validated. If LRU and Random produce identical miss counts, the policy
    parameter is not reaching the cache and every later result is worthless.
    """
    name = name.upper()
    if name in ("SRRIP", "BRRIP"):
        # Imported lazily so that LRU and Random still work against a stock
        # gem5 binary. If these names are missing, the policies were never
        # compiled in and the error below says so directly instead of
        # surfacing as an opaque import failure at startup.
        try:
            from m5.objects import (
                BRRIPCustomRP,
                SRRIPRP,
            )
        except ImportError as exc:
            raise ImportError(
                "SRRIPRP/BRRIPCustomRP are not in this gem5 binary. Run "
                "scripts/install_policies.sh to copy the sources into the "
                "gem5 tree and rebuild."
            ) from exc
        if name == "SRRIP":
            return SRRIPRP(num_bits=num_bits)
        return BRRIPCustomRP(num_bits=num_bits, btp=btp)
    if name == "LRU":
        return LRURP()
    if name == "RANDOM":
        return RandomRP()
    if name in ("BRRIP_REF", "SRRIP_REF"):
        # gem5's own RRIP implementation, used only to validate mine against
        # a known-good reference. Not part of the reported dataset.
        #
        # hit_priority=True is the setting that matches my touch(), which
        # sets RRPV to zero on a hit. gem5's default of False instead
        # decrements the counter, which is the paper's frequency-priority
        # variant and a genuinely different policy. Comparing against the
        # wrong variant makes a correct implementation look broken.
        #
        # gem5 implements SRRIP as its BRRIP with btp=100, that is, always
        # insert at the long interval and never at the distant one.
        return BRRIPRP(
            num_bits=num_bits,
            btp=100 if name == "SRRIP_REF" else btp,
            hit_priority=True,
        )
    raise ValueError(
        f"unknown policy {name!r}; expected LRU, Random, SRRIP or BRRIP"
    )


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--policy",
        required=True,
        help="L2 replacement policy: LRU, Random, SRRIP or BRRIP",
    )
    parser.add_argument(
        "--binary", required=True, help="path to the statically linked binary"
    )
    parser.add_argument(
        "--bench-arg",
        action="append",
        default=[],
        help="argument passed to the benchmark; repeat for several",
    )
    parser.add_argument("--l2-size", default="1MiB", help="L2 cache size")
    parser.add_argument(
        "--l2-assoc", type=int, default=16, help="L2 associativity"
    )
    parser.add_argument("--l1d-size", default="32KiB", help="L1 data size")
    parser.add_argument(
        "--l1i-size", default="32KiB", help="L1 instruction size"
    )
    parser.add_argument(
        "--num-bits",
        type=int,
        default=2,
        help="width of the RRPV counter in bits (SRRIP and BRRIP)",
    )
    parser.add_argument(
        "--btp",
        type=int,
        default=3,
        help="BRRIP bimodal throttle: percent of insertions that get the "
        "near-immediate RRPV instead of distant",
    )
    parser.add_argument(
        "--warmup-insts",
        type=int,
        default=0,
        help="if non-zero, run this many instructions on an atomic core to "
        "warm the caches, then switch to TimingSimpleCPU and reset stats "
        "before measuring",
    )
    return parser.parse_args()


def main():
    args = parse_args()

    replacement_policy = build_replacement_policy(
        args.policy, args.num_bits, args.btp
    )

    warmup = args.warmup_insts > 0

    if warmup:
        # Stretch goal S1. Start on an atomic core, which executes fast and
        # still moves data through the cache hierarchy, so cache contents are
        # warm by the time we switch. Then switch to the timing core and
        # reset stats, so the measured window contains no compulsory misses
        # from the cold start.
        processor = SimpleSwitchableProcessor(
            starting_core_type=CPUTypes.ATOMIC,
            switch_core_type=CPUTypes.TIMING,
            num_cores=1,
            isa=ISA.X86,
        )
    else:
        processor = SimpleProcessor(
            cpu_type=CPUTypes.TIMING, num_cores=1, isa=ISA.X86
        )

    cache_hierarchy = ConfigurableL2Hierarchy(
        l1d_size=args.l1d_size,
        l1i_size=args.l1i_size,
        l2_size=args.l2_size,
        l2_assoc=args.l2_assoc,
        l2_replacement_policy=replacement_policy,
        l2_prefetcher=None,
    )

    board = SimpleBoard(
        clk_freq="3GHz",
        processor=processor,
        memory=SingleChannelDDR3_1600(size="3GiB"),
        cache_hierarchy=cache_hierarchy,
    )

    board.set_se_binary_workload(
        BinaryResource(local_path=os.path.abspath(args.binary)),
        arguments=list(args.bench_arg),
    )

    if warmup:

        def on_warmup_done():
            # Order matters. Switch first so the timing core owns the run,
            # then zero the counters so the reported numbers describe only
            # the measured window.
            processor.switch()
            m5.stats.reset()
            yield False

        simulator = Simulator(
            board=board,
            on_exit_event={ExitEvent.MAX_INSTS: on_warmup_done()},
        )
        simulator.schedule_max_insts(args.warmup_insts)
    else:
        simulator = Simulator(board=board)

    simulator.run()

    print(
        f"run.py: policy={args.policy} l2={args.l2_size} "
        f"assoc={args.l2_assoc} warmup={args.warmup_insts} "
        f"exit={simulator.get_last_exit_event_cause()}"
    )


if __name__ == "__m5_main__" or __name__ == "__main__":
    main()
