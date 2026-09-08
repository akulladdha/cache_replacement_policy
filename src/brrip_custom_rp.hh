/**
 * BRRIP: Bimodal Re-Reference Interval Prediction.
 *
 * Named BRRIPCustom rather than BRRIP because gem5 already ships a policy
 * called BRRIPRP. This one is written independently for this project, and
 * carrying a distinct name keeps both available in the same binary so the
 * two can be compared if anyone wants to.
 *
 * BRRIP differs from SRRIP in exactly one place: where a new line enters.
 * Everything else, the hit behaviour, the aging, the victim search, is
 * inherited unchanged from SRRIP. Expressing that as inheritance rather than
 * as a copied file is deliberate, because "the only difference is the
 * insertion position" is the entire content of the idea.
 *
 * SRRIP inserts every line at max minus one. That works well when the
 * working set fits, but when the working set is larger than the cache it
 * still thrashes: every line gets its one chance, ages out before the
 * program laps back around to it, and is gone by the time it is needed.
 *
 * BRRIP inserts at the maximum RRPV, evict-me-first, for the large majority
 * of insertions, and at max minus one only occasionally, with probability
 * btp. The effect is that most incoming lines cycle through a small part of
 * the cache quickly while a small retained fraction survives long enough to
 * actually be re-referenced. It is thrash resistance by deliberately
 * declining to cache most of what it sees, which is the counterintuitive
 * part and the reason the policy works.
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_BRRIP_CUSTOM_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_BRRIP_CUSTOM_RP_HH__

#include <memory>

#include "base/random.hh"
#include "mem/cache/replacement_policies/srrip_rp.hh"

namespace gem5
{

struct BRRIPCustomRPParams;

namespace replacement_policy
{

class BRRIPCustom : public SRRIP
{
  protected:
    /**
     * Bimodal throttle parameter, as a percentage. This is the share of
     * insertions that get the short "one chance" interval instead of the
     * distant one. The paper's value is 1/32, so roughly 3 percent.
     *
     * Set this to 100 and the policy degenerates into plain SRRIP, which is
     * a useful sanity check.
     */
    const unsigned btp;

    /** Random source for the bimodal decision. */
    Random::RandomPtr rng = Random::genRandom();

  public:
    typedef BRRIPCustomRPParams Params;
    BRRIPCustom(const Params &p);
    ~BRRIPCustom() = default;

    /**
     * Insertion, and the only method BRRIP overrides. With probability btp
     * insert at max minus one exactly as SRRIP would; otherwise insert at
     * the maximum RRPV, which makes the line the next candidate for
     * eviction.
     */
    void reset(const std::shared_ptr<ReplacementData>& replacement_data) const
                                                                     override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_BRRIP_CUSTOM_RP_HH__
