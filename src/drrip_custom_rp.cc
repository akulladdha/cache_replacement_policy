/**
 * DRRIP: Dynamic Re-Reference Interval Prediction. See drrip_custom_rp.hh.
 *
 * Victim selection, hits and aging all come from SRRIP unchanged. The two
 * insertion behaviours come from SRRIP and BRRIPCustom unchanged. What is new
 * here is only the choice between them.
 */

#include "mem/cache/replacement_policies/drrip_custom_rp.hh"

#include <memory>

#include "base/logging.hh"
#include "params/DRRIPCustomRP.hh"

namespace gem5
{

namespace replacement_policy
{

DRRIPCustom::DRRIPCustom(const Params &p)
  : BRRIPCustom(p),
    constituencySize(p.constituency_size),
    maxPSEL((1u << p.psel_bits) - 1),
    pselThreshold(1u << (p.psel_bits - 1)),
    // Start one below the threshold, so followers begin on SRRIP. SRRIP is
    // the safer default: it is never much worse than LRU, whereas BRRIP on a
    // workload that fits can be noticeably worse.
    psel(pselThreshold - 1),
    stats(this)
{
    fatal_if(p.psel_bits < 1 || p.psel_bits > 16,
             "psel_bits must be between 1 and 16");
    // An even constituency guarantees the two leader positions computed in
    // roleOf() never land on the same set.
    fatal_if(constituencySize < 2 || constituencySize % 2 != 0,
             "constituency_size must be an even number of at least 2");
}

DRRIPCustom::SetRole
DRRIPCustom::roleOf(uint32_t set) const
{
    // The paper's complement-select scheme. Split the set index into a
    // constituency number and an offset within that constituency. The SRRIP
    // leader of constituency k sits at offset k, and the BRRIP leader at the
    // complementary offset C - 1 - k (both mod C). Leaders therefore move
    // around from one constituency to the next instead of always sitting at
    // offset 0, so a strided access pattern cannot happen to land on, or
    // avoid, every leader of one policy at once.
    const uint32_t k = (set / constituencySize) % constituencySize;
    const uint32_t offset = set % constituencySize;

    if (offset == k) {
        return SetRole::SRRIPLeader;
    }
    if (offset == constituencySize - 1 - k) {
        return SetRole::BRRIPLeader;
    }
    return SetRole::Follower;
}

void
DRRIPCustom::reset(const std::shared_ptr<ReplacementData>& replacement_data)
                                                                        const
{
    // Insertion. The decision was made in getVictim(); here it is only
    // carried out, by calling the exact insertion code of whichever policy
    // won. Neither branch adds anything of its own.
    if (std::static_pointer_cast<DRRIPReplData>(
            replacement_data)->insertBimodal) {
        BRRIPCustom::reset(replacement_data);
    } else {
        SRRIP::reset(replacement_data);
    }
}

ReplaceableEntry*
DRRIPCustom::getVictim(const ReplacementCandidates& candidates) const
{
    ReplaceableEntry* victim = SRRIP::getVictim(candidates);

    // getVictim() runs once per miss, so this is where misses are counted.
    // Only leader-set misses move the counter; follower misses tell us
    // nothing about which policy is better, since followers are already
    // running the current winner.
    bool bimodal;
    switch (roleOf(victim->getSet())) {
      case SetRole::SRRIPLeader:
        if (psel < maxPSEL) {
            psel++;
        }
        stats.srripLeaderMisses++;
        bimodal = false;
        break;

      case SetRole::BRRIPLeader:
        if (psel > 0) {
            psel--;
        }
        stats.brripLeaderMisses++;
        bimodal = true;
        break;

      default:
        // PSEL high means the SRRIP leaders have been missing more, so the
        // followers should insert the BRRIP way.
        bimodal = psel >= pselThreshold;
        if (bimodal) {
            stats.followerBRRIPInserts++;
        } else {
            stats.followerSRRIPInserts++;
        }
        break;
    }

    stats.psel = psel;

    std::static_pointer_cast<DRRIPReplData>(
        victim->replacementData)->insertBimodal = bimodal;

    return victim;
}

std::shared_ptr<ReplacementData>
DRRIPCustom::instantiateEntry()
{
    return std::shared_ptr<ReplacementData>(new DRRIPReplData(maxRRPV));
}

DRRIPCustom::DRRIPStats::DRRIPStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(srripLeaderMisses, statistics::units::Count::get(),
             "Misses in sets that always insert the SRRIP way"),
    ADD_STAT(brripLeaderMisses, statistics::units::Count::get(),
             "Misses in sets that always insert the BRRIP way"),
    ADD_STAT(followerSRRIPInserts, statistics::units::Count::get(),
             "Follower-set insertions made the SRRIP way"),
    ADD_STAT(followerBRRIPInserts, statistics::units::Count::get(),
             "Follower-set insertions made the BRRIP way"),
    ADD_STAT(psel, statistics::units::Count::get(),
             "Policy selector value at the most recent miss"),
    ADD_STAT(followerBRRIPFraction, statistics::units::Ratio::get(),
             "Share of follower insertions made the BRRIP way",
             followerBRRIPInserts /
                 (followerSRRIPInserts + followerBRRIPInserts))
{
}

} // namespace replacement_policy
} // namespace gem5
