// Scalar envelope update: the reference the vector versions are checked
// against, and the implementation on any target without one of its own.
// Note the guard is "no vector envelope", not "no intrinsics at all": a
// universal binary has a slice for each architecture, and a slice whose
// instruction set has no hand-written version still has to link.

#include "ymfm_eg.h"
#include "ymfm.h"

#if !YMFM_HAVE_VECTOR_EG

namespace ymfm
{

void eg_clock(eg_block const &block, uint32_t env_counter)
{
	uint32_t const count = block.count;
	for (uint32_t op = 0; op < count; op++)
	{
		uint32_t state = block.state[op];
		uint32_t atten = block.atten[op];
		uint32_t rate = block.cur_rate[op];
		uint32_t packed = block.cur_inc[op];

		// attack->decay, then decay->sustain; the second has to see the result
		// of the first, because a sustain level of 0 skips decay entirely
		if (state == EG_ATTACK && atten == 0)
		{
			state = EG_DECAY;
			rate = block.rate_of[EG_DECAY * count + op];
			packed = block.inc_of[EG_DECAY * count + op];
		}
		if (state == EG_DECAY && atten >= block.sustain[op])
		{
			state = EG_SUSTAIN;
			rate = block.rate_of[EG_SUSTAIN * count + op];
			packed = block.inc_of[EG_SUSTAIN * count + op];
		}

		uint32_t const rate_shift = rate >> 2;
		uint32_t const shifted = env_counter << rate_shift;
		bool const clocking = (shifted & 0x7ff) == 0;

		uint32_t const relevant = (shifted >> (rate_shift > 11 ? rate_shift : 11)) & 7;
		uint32_t const increment = (packed >> (4 * relevant)) & 0xf;

		// attack is the only one that increases; rates of 62/63 do not
		// increment if changed after the initial key on
		uint32_t const attack_delta = (rate < 62) ? ((~atten * increment) >> 4) : 0u;
		uint32_t const attacked = uint16_t(atten + attack_delta);

		uint32_t decayed = atten + increment;
		decayed = (decayed >= 0x400) ? 0x3ffu : decayed;

		uint32_t const updated = (state == EG_ATTACK) ? attacked : decayed;
		atten = clocking ? updated : atten;

		if (block.has_reverb && clocking && state == EG_RELEASE && atten >= 0xc0)
		{
			state = EG_REVERB;
			rate = block.rate_of[EG_REVERB * count + op];
			packed = block.inc_of[EG_REVERB * count + op];
		}

		block.atten[op] = atten;
		block.state[op] = state;
		block.cur_rate[op] = rate;
		block.cur_inc[op] = packed;
	}
}

}

#endif
