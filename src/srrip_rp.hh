/**
 * SRRIP: Static Re-Reference Interval Prediction.
 *
 * Written from the description in Jaleel et al., "High Performance Cache
 * Replacement Using Re-Reference Interval Prediction (RRIP)", ISCA 2010,
 * sections 3 and 4. gem5 ships its own RRIP implementation in brrip_rp.*;
 * this one was written independently, which is the point of the exercise.
 * It is named SRRIPRP so that it sits alongside the upstream BRRIPRP rather
 * than replacing it.
 *
 * The idea in one paragraph. Every cache line carries a small counter, the
 * Re-Reference Prediction Value, predicting how soon that line will be used
 * again. Zero means "expected to be re-referenced very soon". The maximum
 * value means "expected far in the future, or never". Eviction picks a line
 * predicted to be re-referenced furthest away, that is, one at the maximum
 * RRPV. If no line is at the maximum, every line is aged until one is.
 *
 * The part that actually matters is where a newly inserted line starts.
 * SRRIP inserts at max minus one: not "keep me", but "give me one chance to
 * prove myself". A line that is hit before it ages out drops to zero and
 * earns a full lifetime. A line that is never re-referenced ages out quickly
 * and leaves. That single choice is what gives SRRIP scan resistance that
 * LRU does not have, because under LRU a scan of new lines inserted at MRU
 * walks the entire cache and evicts everything useful on the way through.
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_SRRIP_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_SRRIP_RP_HH__

#include <cstdint>
#include <memory>

#include "mem/cache/replacement_policies/base.hh"

namespace gem5
{

struct SRRIPRPParams;

namespace replacement_policy
{

class SRRIP : public Base
{
  protected:
    /**
     * Per-line replacement state: one RRPV counter plus a validity flag.
     *
     * The validity flag is not strictly part of the RRIP algorithm. It is
     * here so that getVictim can hand back a line that holds nothing at all
     * in preference to evicting a line that holds live data, which matters
     * while the cache is still filling.
     */
    struct SRRIPReplData : ReplacementData
    {
        /** Re-Reference Prediction Value. Lower means sooner. */
        uint8_t rrpv;

        /** False until the entry has been inserted, true until invalidated. */
        bool valid;

        SRRIPReplData(uint8_t max_rrpv) : rrpv(max_rrpv), valid(false) {}
    };

    /**
     * Largest value an RRPV can hold, that is, (2 ** num_bits) - 1. With the
     * paper's default of 2 bits this is 3.
     */
    const uint8_t maxRRPV;

  public:
    typedef SRRIPRPParams Params;
    SRRIP(const Params &p);
    ~SRRIP() = default;

    /**
     * Mark the entry as holding nothing, so it is chosen before any line
     * that holds real data.
     */
    void invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
                                                                    override;

    /**
     * Called on a cache hit. Set RRPV to zero: this line was just proven
     * useful, so predict it will be used again soon.
     */
    void touch(const std::shared_ptr<ReplacementData>& replacement_data) const
                                                                     override;

    /**
     * Called on insertion, not on a hit. Set RRPV to max minus one.
     *
     * This is the single most important line in the file, and it is the one
     * that is easy to get backwards. reset() fires when a line enters the
     * cache; touch() fires when a resident line is hit. Swapping the two
     * yields a policy that compiles, runs, and reports entirely plausible
     * numbers that are wrong. Both SRRIP and BRRIP work by changing where
     * lines enter, not by changing what happens on a hit.
     */
    void reset(const std::shared_ptr<ReplacementData>& replacement_data) const
                                                                     override;

    /**
     * Pick a victim: an invalid line if there is one, otherwise a line at
     * the maximum RRPV, aging all candidates until such a line exists.
     */
    ReplaceableEntry* getVictim(const ReplacementCandidates& candidates) const
                                                                     override;

    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_SRRIP_RP_HH__
