/**
 * BRRIP: Bimodal Re-Reference Interval Prediction. See brrip_custom_rp.hh.
 *
 * The whole policy is the reset() below. Everything else comes from SRRIP.
 */

#include "mem/cache/replacement_policies/brrip_custom_rp.hh"

#include <memory>

#include "params/BRRIPCustomRP.hh"

namespace gem5
{

namespace replacement_policy
{

BRRIPCustom::BRRIPCustom(const Params &p)
  : SRRIP(p),
    btp(p.btp)
{
}

void
BRRIPCustom::reset(const std::shared_ptr<ReplacementData>& replacement_data)
                                                                        const
{
    std::shared_ptr<SRRIPReplData> data =
        std::static_pointer_cast<SRRIPReplData>(replacement_data);

    // The bimodal insertion decision, and the only difference from SRRIP.
    //
    // btp percent of the time, behave exactly like SRRIP and give the line
    // one aging round to prove itself. The rest of the time insert at the
    // maximum RRPV, so the line is evicted on the next miss to this set
    // unless it happens to be hit first.
    //
    // Under a working set that fits, the distant insertions cost little,
    // because hits promote lines to RRPV zero and they stay. Under a working
    // set that does not fit, the small retained fraction is what keeps the
    // cache from being completely churned, and that is where BRRIP pulls
    // ahead of both SRRIP and LRU.
    if (rng->random<unsigned>(1, 100) <= btp) {
        data->rrpv = maxRRPV > 0 ? maxRRPV - 1 : 0;
    } else {
        data->rrpv = maxRRPV;
    }

    data->valid = true;
}

} // namespace replacement_policy
} // namespace gem5
