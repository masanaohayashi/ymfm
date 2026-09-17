// NEON phase accumulation. Same function as the scalar version in
// ymfm_phase.cpp, selected by ymfm_condition.h; this file compiles to nothing
// on other targets.

#include "ymfm_phase.h"

#if YMFM_HAVE_VECTOR_PHASE

namespace ymfm
{

void phase_clock(uint32_t *phase, uint32_t const *step, uint32_t count)
{
	// Eight operators at a time, written as two quads rather than a loop of
	// two, so both halves can issue in the same cycle; on a 256-bit target the
	// pair collapses into one register. The chip's operator count is a
	// multiple of eight, so there is no tail.
	for (uint32_t opnum = 0; opnum < count; opnum += 8)
	{
		uint32x4_t const lo = vaddq_u32(vld1q_u32(&phase[opnum]), vld1q_u32(&step[opnum]));
		uint32x4_t const hi = vaddq_u32(vld1q_u32(&phase[opnum + 4]), vld1q_u32(&step[opnum + 4]));
		vst1q_u32(&phase[opnum], lo);
		vst1q_u32(&phase[opnum + 4], hi);
	}
}

}

#endif
