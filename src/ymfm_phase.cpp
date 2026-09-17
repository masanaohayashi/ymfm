// Scalar phase accumulation. Compiled where no hand-written vector version
// exists for the target; see ymfm_condition.h.

#include "ymfm_phase.h"

#if !YMFM_HAVE_VECTOR_PHASE

namespace ymfm
{

void phase_clock(uint32_t *phase, uint32_t const *step, uint32_t count)
{
	for (uint32_t opnum = 0; opnum < count; opnum++)
		phase[opnum] += step[opnum];
}

}

#endif
