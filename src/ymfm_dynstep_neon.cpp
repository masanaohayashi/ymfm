// NEON per-sample phase step. Selected by ymfm_condition.h; this file compiles
// to nothing on other targets.

#include "ymfm_dynstep.h"
#include "ymfm_phase.h"

#if YMFM_HAVE_VECTOR_DYNSTEP

namespace ymfm
{

namespace
{

// Eight operators at a time, as a pair of quads rather than a loop of two, so
// both halves can issue in the same cycle.
struct pair32
{
	uint32x4_t lo, hi;
};

struct pairs32
{
	int32x4_t lo, hi;
};

inline pair32 load_u(uint32_t const *p)
{
	return {vld1q_u32(p), vld1q_u32(p + 4)};
}

inline pair32 splat(uint32_t v)
{
	return {vdupq_n_u32(v), vdupq_n_u32(v)};
}

inline pair32 add(pair32 a, pair32 b)
{
	return {vaddq_u32(a.lo, b.lo), vaddq_u32(a.hi, b.hi)};
}

inline pair32 sub(pair32 a, pair32 b)
{
	return {vsubq_u32(a.lo, b.lo), vsubq_u32(a.hi, b.hi)};
}

inline pair32 bit_or(pair32 a, pair32 b)
{
	return {vorrq_u32(a.lo, b.lo), vorrq_u32(a.hi, b.hi)};
}

inline pair32 bit_and(pair32 a, pair32 b)
{
	return {vandq_u32(a.lo, b.lo), vandq_u32(a.hi, b.hi)};
}

inline pair32 select(pair32 mask, pair32 a, pair32 b)
{
	return {vbslq_u32(mask.lo, a.lo, b.lo), vbslq_u32(mask.hi, a.hi, b.hi)};
}

inline pair32 eq(pair32 a, pair32 b)
{
	return {vceqq_u32(a.lo, b.lo), vceqq_u32(a.hi, b.hi)};
}

inline pair32 ge_u(pair32 a, pair32 b)
{
	return {vcgeq_u32(a.lo, b.lo), vcgeq_u32(a.hi, b.hi)};
}

inline pair32 lt_s(pairs32 a, int32_t b)
{
	int32x4_t const v = vdupq_n_s32(b);
	return {vcltq_s32(a.lo, v), vcltq_s32(a.hi, v)};
}

inline pair32 ge_s(pairs32 a, int32_t b)
{
	int32x4_t const v = vdupq_n_s32(b);
	return {vcgeq_s32(a.lo, v), vcgeq_s32(a.hi, v)};
}

inline pairs32 as_s(pair32 v)
{
	return {vreinterpretq_s32_u32(v.lo), vreinterpretq_s32_u32(v.hi)};
}

inline pair32 as_u(pairs32 v)
{
	return {vreinterpretq_u32_s32(v.lo), vreinterpretq_u32_s32(v.hi)};
}

// variable per-lane shift; a negative count shifts right
inline pair32 shift_u(pair32 v, pair32 count)
{
	return {vshlq_u32(v.lo, vreinterpretq_s32_u32(count.lo)),
		vshlq_u32(v.hi, vreinterpretq_s32_u32(count.hi))};
}

inline pairs32 shift_s(pairs32 v, pair32 count)
{
	return {vshlq_s32(v.lo, vreinterpretq_s32_u32(count.lo)),
		vshlq_s32(v.hi, vreinterpretq_s32_u32(count.hi))};
}

inline pair32 mul(pair32 a, pair32 b)
{
	return {vmulq_u32(a.lo, b.lo), vmulq_u32(a.hi, b.hi)};
}

// One gather per quad. Three instructions a lane against one scalar load, but
// it replaces a call that also decodes registers and branches twice.
inline uint32x4_t gather(uint32_t const *table, uint32x4_t index)
{
	uint32x4_t out = vdupq_n_u32(table[vgetq_lane_u32(index, 0)]);
	out = vsetq_lane_u32(table[vgetq_lane_u32(index, 1)], out, 1);
	out = vsetq_lane_u32(table[vgetq_lane_u32(index, 2)], out, 2);
	out = vsetq_lane_u32(table[vgetq_lane_u32(index, 3)], out, 3);
	return out;
}

}

void dyn_step_clock(dyn_step_block const &block)
{
	// Both LFOs' PM values are the same for every operator, and the
	// sensitivity that scales them is per channel, so the whole contribution
	// is one eight-lane value that serves every slot. The manual's magnitudes
	// correspond to shifting the 200-cent value by five down to one up, which
	// the caller has already turned into a signed shift; one instruction
	// covers both directions.
	pairs32 const pm_value = {
		vdupq_n_s32(int8_t(block.lfo_raw_pm)), vdupq_n_s32(int8_t(block.lfo_raw_pm))};
	pairs32 const pm2_value = {
		vdupq_n_s32(int8_t(block.lfo_raw_pm >> 8)), vdupq_n_s32(int8_t(block.lfo_raw_pm >> 8))};
	pair32 const pm_total = add(
		bit_and(as_u(shift_s(pm_value, load_u(block.pm_shift))), load_u(block.pm_live)),
		bit_and(as_u(shift_s(pm2_value, load_u(block.pm2_shift))), load_u(block.pm2_live)));

	pair32 const zero = splat(0);
	pair32 const octave_range = splat(768);

	for (uint32_t base = 0; base < block.count; base += 8)
	{
		pairs32 const eff = as_s(add(load_u(&block.eff_base[base]),
			add(load_u(&block.delta[base]), pm_total)));
		pair32 const octave = load_u(&block.octave[base]);

		// Over and underflow move the octave instead. The minimum delta is
		// -512, so it can only underflow by one; the maximum is +512+608, so
		// it can overflow by two.
		pair32 const under = lt_s(eff, 0);
		pair32 const over = ge_s(eff, 768);

		pair32 const eff_under = add(as_u(eff), octave_range);
		pair32 const oct_under = sub(octave, splat(1));
		pair32 const clamp_under = bit_and(under, eq(octave, zero));

		pair32 const eff_over1 = sub(as_u(eff), octave_range);
		pair32 const again = ge_u(eff_over1, octave_range);
		pair32 const eff_over = select(again, sub(eff_over1, octave_range), eff_over1);
		pair32 const oct_over1 = select(again, add(octave, splat(1)), octave);
		pair32 const clamp_over = bit_and(over, ge_u(oct_over1, splat(7)));
		pair32 const oct_over = add(oct_over1, splat(1));

		pair32 index = select(under, eff_under, select(over, eff_over, as_u(eff)));
		pair32 const octave_out = select(under, oct_under, select(over, oct_over, octave));

		// the clamped lanes are replaced below, but their index still has to
		// be inside the table for the gather
		index = select(ge_u(index, octave_range), splat(767), index);

		pair32 const looked_up = {
			gather(g_phase_step_table, index.lo), gather(g_phase_step_table, index.hi)};
		pair32 const octave_shift = {
			veorq_u32(octave_out.lo, vdupq_n_u32(7)), veorq_u32(octave_out.hi, vdupq_n_u32(7))};
		pair32 shifted = shift_u(looked_up, {vnegq_s32(vreinterpretq_s32_u32(octave_shift.lo)),
			vnegq_s32(vreinterpretq_s32_u32(octave_shift.hi))});
		shifted = select(clamp_under, splat(g_phase_step_table[0] >> 7), shifted);
		shifted = select(clamp_over, splat(g_phase_step_table[767]), shifted);

		// detune by keycode, then the frequency multiplier, which is an x.4
		pair32 const tuned = add(shifted, load_u(&block.detune[base]));
		pair32 out = shift_u(mul(tuned, load_u(&block.multiple[base])), splat(uint32_t(-4)));

		// Fixed-frequency mode replaces all of that; its registers occupy the
		// same space as detune and multiple, so those do not apply. The plain
		// step has too little resolution for the range, so a per-operator
		// sub-step carries twelve more bits. Skipped entirely when no operator
		// is in that mode, which is the usual case.
		if (block.any_fixed)
		{
			pair32 const previous = load_u(&block.substep[base]);
			pair32 const substep = add(previous, load_u(&block.fix_rate[base]));
			pair32 const fixed = load_u(&block.fix_mask[base]);
			pair32 const kept = select(fixed, bit_and(substep, splat(0xfff)), previous);
			vst1q_u32(&block.substep[base], kept.lo);
			vst1q_u32(&block.substep[base + 4], kept.hi);
			out = select(fixed, shift_u(substep, splat(uint32_t(-12))), out);
		}

		vst1q_u32(&block.step[base], out.lo);
		vst1q_u32(&block.step[base + 4], out.hi);
	}
}

}

#endif
