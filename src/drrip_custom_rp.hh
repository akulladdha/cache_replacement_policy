/**
 * DRRIP: Dynamic Re-Reference Interval Prediction.
 *
 * Named DRRIPCustom rather than DRRIP because gem5 already ships a DRRIPRP,
 * built on its generic DuelingRP. That one keeps a separate copy of the RRIP
 * state for each competing policy. This one follows the paper more literally:
 * there is a single RRPV per line, hits and aging are exactly SRRIP's, and the
 * only thing that changes from set to set is where a new line is inserted.
 *
 * SRRIP and BRRIP each win in a different regime. SRRIP is right when the
 * working set fits or there is a hot set worth keeping; BRRIP is right when
 * the working set is a little larger than the cache and SRRIP would thrash.
 * Which regime a program is in is not known until it runs, and it can change
 * while it runs. DRRIP picks at run time using set dueling:
 *
 *   - A few "leader" sets always insert the SRRIP way, and an equal number
 *     always insert the BRRIP way. Both groups are spread evenly across the
 *     cache.
 *   - A saturating counter, PSEL, goes up on every miss in an SRRIP leader
 *     and down on every miss in a BRRIP leader. Whichever group misses more
 *     pushes the counter towards its own end.
 *   - Every other set, the "followers", inserts the way the counter says is
 *     currently missing less.
 *
 * The sampled sets cost a small fraction of the cache (with the defaults, 64
 * of 1024 sets, a sixteenth) running a policy that may be the wrong one; in
 * return the other fifteen sixteenths run the right one.
 *
 * gem5 does not tell reset() which set a line belongs to, but getVictim()
 * sees the candidates, and each candidate knows its set. getVictim() is also
 * called exactly once per miss, immediately before the new line is inserted
 * into the way it returns. So the whole dueling decision happens there: count
 * the miss, choose the insertion mode, and leave the choice in the victim's
 * replacement data for reset() to read a moment later.
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_DRRIP_CUSTOM_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_DRRIP_CUSTOM_RP_HH__

#include <cstdint>
#include <memory>

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/brrip_custom_rp.hh"

namespace gem5
{

struct DRRIPCustomRPParams;

namespace replacement_policy
{

class DRRIPCustom : public BRRIPCustom
{
  protected:
    /**
     * SRRIP's per-line state plus the insertion mode chosen for this line by
     * the getVictim() call that selected it.
     *
     * The flag is deliberately left alone by invalidate(). The cache
     * invalidates the victim between getVictim() and reset(), and the
     * decision has to survive that.
     */
    struct DRRIPReplData : SRRIPReplData
    {
        /** True: insert the BRRIP way. False: insert the SRRIP way. */
        bool insertBimodal;

        DRRIPReplData(uint8_t max_rrpv)
          : SRRIPReplData(max_rrpv), insertBimodal(false) {}
    };

    /** The role a set plays in the duel. */
    enum class SetRole { SRRIPLeader, BRRIPLeader, Follower };

    /**
     * Sets per constituency. Each constituency holds exactly one SRRIP
     * leader and one BRRIP leader, so this is also the cache's set count
     * divided by the number of leaders per policy.
     */
    const unsigned constituencySize;

    /** Largest value PSEL can hold, (2 ** psel_bits) - 1. */
    const unsigned maxPSEL;

    /**
     * Followers insert the BRRIP way once PSEL reaches this value, that is,
     * once its top bit is set.
     */
    const unsigned pselThreshold;

    /**
     * Policy selector. Rises on SRRIP-leader misses, falls on BRRIP-leader
     * misses. Mutable because gem5 declares getVictim() const, and the
     * counter is policy-wide state rather than per-line state.
     */
    mutable unsigned psel;

    /** Classify a set as SRRIP leader, BRRIP leader, or follower. */
    SetRole roleOf(uint32_t set) const;

    struct DRRIPStats : public statistics::Group
    {
        DRRIPStats(statistics::Group *parent);

        statistics::Scalar srripLeaderMisses;
        statistics::Scalar brripLeaderMisses;
        statistics::Scalar followerSRRIPInserts;
        statistics::Scalar followerBRRIPInserts;
        statistics::Scalar psel;
        statistics::Formula followerBRRIPFraction;
    };

    /** Mutable for the same reason as psel. */
    mutable DRRIPStats stats;

  public:
    typedef DRRIPCustomRPParams Params;
    DRRIPCustom(const Params &p);
    ~DRRIPCustom() = default;

    /**
     * Insertion. Reads the decision getVictim() left in the line and inserts
     * exactly as SRRIP or exactly as BRRIP would.
     */
    void reset(const std::shared_ptr<ReplacementData>& replacement_data) const
                                                                     override;

    /**
     * Select the victim exactly as SRRIP does, then record this miss against
     * the set's leader group, if any, and decide how the incoming line will
     * be inserted.
     */
    ReplaceableEntry* getVictim(const ReplacementCandidates& candidates) const
                                                                     override;

    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_DRRIP_CUSTOM_RP_HH__
