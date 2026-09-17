// NEON envelope update. Same function as the scalar version in ymfm_eg.cpp,
// selected by ymfm_condition.h; this file compiles to nothing on other targets.

#include "ymfm_eg.h"
#include "ymfm.h"

#if YMFM_HAVE_VECTOR_EG

namespace ymfm
{

namespace
{

// Eight operators at a time, as a pair of quads rather than a loop of two:
// both halves of every step are independent, so the core can put them down
// separate SIMD pipes in the same cycle. On a 256-bit target the same pair
// collapses into one register.
struct quad_pair
{
	uint32x4_t lo, hi;
};

inline quad_pair load_pair(uint32_t const *p)
{
	return {vld1q_u32(p), vld1q_u32(p + 4)};
}

inline void store_pair(uint32_t *p, quad_pair v)
{
	vst1q_u32(p, v.lo);
	vst1q_u32(p + 4, v.hi);
}

inline quad_pair select_pair(quad_pair mask, quad_pair a, quad_pair b)
{
	return {vbslq_u32(mask.lo, a.lo, b.lo), vbslq_u32(mask.hi, a.hi, b.hi)};
}

inline bool any_set(quad_pair mask)
{
	return vmaxvq_u32(vorrq_u32(mask.lo, mask.hi)) != 0;
}

}

void eg_clock(eg_block const &block, uint32_t env_counter)
{
	uint32_t const count = block.count;

	uint32x4_t const counter = vdupq_n_u32(env_counter);
	uint32x4_t const zero    = vdupq_n_u32(0);
	uint32x4_t const k_att   = vdupq_n_u32(EG_ATTACK);
	uint32x4_t const k_dec   = vdupq_n_u32(EG_DECAY);
	uint32x4_t const k_sus   = vdupq_n_u32(EG_SUSTAIN);
	uint32x4_t const k_rel   = vdupq_n_u32(EG_RELEASE);
	uint32x4_t const k_rev   = vdupq_n_u32(EG_REVERB);
	uint32x4_t const seven   = vdupq_n_u32(7);
	uint32x4_t const frac    = vdupq_n_u32(0x7ff);
	uint32x4_t const nibble  = vdupq_n_u32(0xf);
	uint32x4_t const k62     = vdupq_n_u32(62);
	uint32x4_t const k400    = vdupq_n_u32(0x400);
	uint32x4_t const k3ff    = vdupq_n_u32(0x3ff);
	uint32x4_t const k16bit  = vdupq_n_u32(0xffff);
	uint32x4_t const kc0     = vdupq_n_u32(0xc0);
	int32x4_t  const eleven  = vdupq_n_s32(11);

	for (uint32_t base = 0; base < count; base += 8)
	{
		quad_pair atten   = load_pair(&block.atten[base]);
		quad_pair state   = load_pair(&block.state[base]);
		quad_pair sustain = load_pair(&block.sustain[base]);
		quad_pair rate    = load_pair(&block.cur_rate[base]);
		quad_pair packed  = load_pair(&block.cur_inc[base]);

		// attack->decay, then decay->sustain
		quad_pair to_decay = {
			vandq_u32(vceqq_u32(state.lo, k_att), vceqq_u32(atten.lo, zero)),
			vandq_u32(vceqq_u32(state.hi, k_att), vceqq_u32(atten.hi, zero))};
		state = select_pair(to_decay, {k_dec, k_dec}, state);

		quad_pair to_sustain = {
			vandq_u32(vceqq_u32(state.lo, k_dec), vcgeq_u32(atten.lo, sustain.lo)),
			vandq_u32(vceqq_u32(state.hi, k_dec), vcgeq_u32(atten.hi, sustain.hi))};
		state = select_pair(to_sustain, {k_sus, k_sus}, state);

		// The rate and its packed increment word only change when the state
		// does, which is rare, so they are carried per operator and refreshed
		// here rather than selected out of a per-state table every sample.
		// This test is uniform across the lanes, not per-lane, so it stays a
		// branch.
		quad_pair const changed = {
			vorrq_u32(to_decay.lo, to_sustain.lo),
			vorrq_u32(to_decay.hi, to_sustain.hi)};
		if (any_set(changed))
		{
			rate = select_pair(to_decay, load_pair(&block.rate_of[EG_DECAY * count + base]), rate);
			packed = select_pair(to_decay, load_pair(&block.inc_of[EG_DECAY * count + base]), packed);
			rate = select_pair(to_sustain, load_pair(&block.rate_of[EG_SUSTAIN * count + base]), rate);
			packed = select_pair(to_sustain, load_pair(&block.inc_of[EG_SUSTAIN * count + base]), packed);
		}

		// env_counter shifted into 5.11 fixed point; a non-zero fraction means
		// this operator is not due to clock, which was an early return and is
		// a mask here
		int32x4_t const shiftL = vreinterpretq_s32_u32(vshrq_n_u32(rate.lo, 2));
		int32x4_t const shiftH = vreinterpretq_s32_u32(vshrq_n_u32(rate.hi, 2));
		uint32x4_t const countL = vshlq_u32(counter, shiftL);
		uint32x4_t const countH = vshlq_u32(counter, shiftH);
		quad_pair const clocking = {
			vceqq_u32(vandq_u32(countL, frac), zero),
			vceqq_u32(vandq_u32(countH, frac), zero)};

		// the original max() of the shift, then three bits out of the result
		uint32x4_t const relL = vandq_u32(vshlq_u32(countL, vnegq_s32(vmaxq_s32(shiftL, eleven))), seven);
		uint32x4_t const relH = vandq_u32(vshlq_u32(countH, vnegq_s32(vmaxq_s32(shiftH, eleven))), seven);

		// pick the 4-bit increment out of the packed word
		quad_pair const increment = {
			vandq_u32(vshlq_u32(packed.lo, vnegq_s32(vreinterpretq_s32_u32(vshlq_n_u32(relL, 2)))), nibble),
			vandq_u32(vshlq_u32(packed.hi, vnegq_s32(vreinterpretq_s32_u32(vshlq_n_u32(relH, 2)))), nibble)};

		// attack is the only one that increases; rates of 62/63 do not
		// increment if changed after the initial key on
		quad_pair const attacked = {
			vandq_u32(vaddq_u32(atten.lo, vbslq_u32(vcltq_u32(rate.lo, k62),
				vshrq_n_u32(vmulq_u32(vmvnq_u32(atten.lo), increment.lo), 4), zero)), k16bit),
			vandq_u32(vaddq_u32(atten.hi, vbslq_u32(vcltq_u32(rate.hi, k62),
				vshrq_n_u32(vmulq_u32(vmvnq_u32(atten.hi), increment.hi), 4), zero)), k16bit)};

		uint32x4_t decayL = vaddq_u32(atten.lo, increment.lo);
		uint32x4_t decayH = vaddq_u32(atten.hi, increment.hi);
		decayL = vbslq_u32(vcgeq_u32(decayL, k400), k3ff, decayL);
		decayH = vbslq_u32(vcgeq_u32(decayH, k400), k3ff, decayH);

		quad_pair const updated = {
			vbslq_u32(vceqq_u32(state.lo, k_att), attacked.lo, decayL),
			vbslq_u32(vceqq_u32(state.hi, k_att), attacked.hi, decayH)};
		atten = select_pair(clocking, updated, atten);

		if (block.has_reverb)
		{
			quad_pair const to_reverb = {
				vandq_u32(vandq_u32(clocking.lo, vceqq_u32(state.lo, k_rel)), vcgeq_u32(atten.lo, kc0)),
				vandq_u32(vandq_u32(clocking.hi, vceqq_u32(state.hi, k_rel)), vcgeq_u32(atten.hi, kc0))};
			state = select_pair(to_reverb, {k_rev, k_rev}, state);
			if (any_set(to_reverb))
			{
				rate = select_pair(to_reverb, load_pair(&block.rate_of[EG_REVERB * count + base]), rate);
				packed = select_pair(to_reverb, load_pair(&block.inc_of[EG_REVERB * count + base]), packed);
			}
		}

		store_pair(&block.atten[base], atten);
		store_pair(&block.state[base], state);
		store_pair(&block.cur_rate[base], rate);
		store_pair(&block.cur_inc[base], packed);
	}
}

}

#endif
