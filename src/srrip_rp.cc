/**
 * SRRIP: Static Re-Reference Interval Prediction. See srrip_rp.hh for the
 * description of the algorithm and for why the reset/touch split matters.
 */

#include "mem/cache/replacement_policies/srrip_rp.hh"

#include <cassert>
#include <memory>

#include "base/logging.hh"
#include "params/SRRIPRP.hh"

namespace gem5
{

namespace replacement_policy
{

SRRIP::SRRIP(const Params &p)
  : Base(p),
    maxRRPV(static_cast<uint8_t>((1 << p.num_bits) - 1))
{
    fatal_if(p.num_bits <= 0, "num_bits must be greater than zero");
    fatal_if(p.num_bits > 8, "num_bits above 8 does not fit the counter");
}

void
SRRIP::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    std::shared_ptr<SRRIPReplData> data =
        std::static_pointer_cast<SRRIPReplData>(replacement_data);

    // Nothing lives here any more. Park the counter at the distant end so
    // that even if the valid check below were removed, this entry would
    // still be an obvious first choice.
    data->valid = false;
    data->rrpv = maxRRPV;
}

void
SRRIP::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    // A hit. The line has just demonstrated reuse, so predict the next
    // reference is imminent and give it the full lifetime.
    std::static_pointer_cast<SRRIPReplData>(replacement_data)->rrpv = 0;
}

void
SRRIP::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    std::shared_ptr<SRRIPReplData> data =
        std::static_pointer_cast<SRRIPReplData>(replacement_data);

    // Insertion. maxRRPV - 1 is the "long" re-reference interval from the
    // paper: the line is not predicted to be reused soon, but it is given
    // one aging round to prove otherwise before it becomes evictable.
    //
    // Inserting at 0 here would reproduce LRU-like behaviour and would throw
    // away the scan resistance that is the entire reason for the policy.
    data->rrpv = maxRRPV > 0 ? maxRRPV - 1 : 0;
    data->valid = true;
}

ReplaceableEntry*
SRRIP::getVictim(const ReplacementCandidates& candidates) const
{
    assert(candidates.size() > 0);

    // An entry holding nothing is always the cheapest thing to take.
    for (const auto& candidate : candidates) {
        if (!std::static_pointer_cast<SRRIPReplData>(
                candidate->replacementData)->valid) {
            return candidate;
        }
    }

    // Otherwise look for a line predicted to be re-referenced furthest away.
    // The textbook formulation is "scan for maxRRPV; if none, increment every
    // candidate and scan again". Repeated scanning is wasteful, so instead
    // find the largest RRPV present in one pass and age everything by exactly
    // the amount needed to bring that line up to maxRRPV. The resulting
    // victim and the resulting counter values are identical to the repeated
    // form, because aging applies the same increment to every candidate.
    ReplaceableEntry* victim = candidates[0];
    uint8_t highest = 0;

    for (const auto& candidate : candidates) {
        const uint8_t rrpv = std::static_pointer_cast<SRRIPReplData>(
            candidate->replacementData)->rrpv;
        if (rrpv > highest) {
            highest = rrpv;
            victim = candidate;
        }
    }

    const uint8_t aging = maxRRPV - highest;
    if (aging > 0) {
        for (const auto& candidate : candidates) {
            std::static_pointer_cast<SRRIPReplData>(
                candidate->replacementData)->rrpv += aging;
        }
    }

    return victim;
}

std::shared_ptr<ReplacementData>
SRRIP::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new SRRIPReplData(maxRRPV));
}

} // namespace replacement_policy
} // namespace gem5
