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

// eight register bytes widened to eight lanes
inline pair32 load_bytes(uint8_t const *p)
{
	uint16x8_t const wide = vmovl_u8(vld1_u8(p));
	return {vmovl_u16(vget_low_u16(wide)), vmovl_u16(vget_high_u16(wide))};
}

// one bitfield out of eight register bytes; the shift is a variable one, which
// is a single instruction either way
inline pair32 field(pair32 v, int shift, uint32_t mask)
{
	int32x4_t const s = vdupq_n_s32(-shift);
	uint32x4_t const m = vdupq_n_u32(mask);
	return {vandq_u32(vshlq_u32(v.lo, s), m), vandq_u32(vshlq_u32(v.hi, s), m)};
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

// The shift amount that applies the LFO's pitch sensitivity. The manual's
// magnitudes correspond to shifting the 200-cent value by 5 down to 1 up, so
// below 6 it is a right shift and at 6 and above a left one; as a signed
// variable shift that is one instruction either way.
inline pair32 pm_shift_of(pair32 sens)
{
	pair32 const six = splat(6);
	pair32 const high = ge_u(sens, six);
	pair32 const amount = sub(sens, six);
	return select(high, add(amount, splat(1)), amount);
}

}

void dyn_step_clock(dyn_step_block const &block)
{
	// both LFOs' PM values are the same for every operator
	pairs32 const pm_value = {
		vdupq_n_s32(int8_t(block.lfo_raw_pm)), vdupq_n_s32(int8_t(block.lfo_raw_pm))};
	pairs32 const pm2_value = {
		vdupq_n_s32(int8_t(block.lfo_raw_pm >> 8)), vdupq_n_s32(int8_t(block.lfo_raw_pm >> 8))};

	// the sensitivities are per channel, and a slot's eight operators are its
	// eight channels in order, so the same eight bytes serve every slot
	pair32 const sens = field(load_bytes(block.pm_sens_reg), 4, 7);
	pair32 const sens2 = field(load_bytes(block.pm2_sens_reg), 4, 7);
	pair32 const pm_shift = pm_shift_of(sens);
	pair32 const pm2_shift = pm_shift_of(sens2);
	pair32 const zero = splat(0);
	pair32 const pm_quiet = eq(sens, zero);
	pair32 const pm2_quiet = eq(sens2, zero);
	pair32 const pm_live = {vmvnq_u32(pm_quiet.lo), vmvnq_u32(pm_quiet.hi)};
	pair32 const pm2_live = {vmvnq_u32(pm2_quiet.lo), vmvnq_u32(pm2_quiet.hi)};

	pair32 const pm_delta = bit_and(as_u(shift_s(pm_value, pm_shift)), pm_live);
	pair32 const pm2_delta = bit_and(as_u(shift_s(pm2_value, pm2_shift)), pm2_live);

	for (uint32_t base = 0; base < block.count; base += 8)
	{
		// coarse detune, as the manual's cents converted into 1/64ths
		pair32 const detune2 = field(load_bytes(&block.detune2_reg[base]), 6, 3);
		pair32 delta = zero;
		delta = select(eq(detune2, splat(1)), splat((600 * 64 + 50) / 100), delta);
		delta = select(eq(detune2, splat(2)), splat((781 * 64 + 50) / 100), delta);
		delta = select(eq(detune2, splat(3)), splat((950 * 64 + 50) / 100), delta);
		delta = add(add(delta, pm_delta), pm2_delta);

		// The keycode in bits 6-9 is gappy, mapping 12 values over 16 in each
		// octave; multiplying the 4-bit value by 3/4 removes the gaps so the
		// delta can be added, and the 6-bit fraction goes back underneath.
		pair32 const bf = load_u(&block.block_freq[base]);
		pair32 const octave = field(bf, 10, 7);
		pair32 const code = sub(field(bf, 6, 0xf), field(bf, 8, 3));
		pairs32 const eff = as_s(add(bit_or(shift_u(code, splat(6)), field(bf, 0, 0x3f)), delta));

		// over and underflow move the octave instead; the minimum delta is
		// -512 so it can only underflow by one, and the maximum is +512+608 so
		// it can overflow by two
		pair32 const under = lt_s(eff, 0);
		pair32 const over = ge_s(eff, 768);
		pair32 const seven_sixty_eight = splat(768);

		pair32 const eff_under = add(as_u(eff), seven_sixty_eight);
		pair32 const oct_under = sub(octave, splat(1));
		pair32 const clamp_under = bit_and(under, eq(octave, zero));

		pair32 const eff_over1 = sub(as_u(eff), seven_sixty_eight);
		pair32 const again = ge_u(eff_over1, seven_sixty_eight);
		pair32 const eff_over = select(again, sub(eff_over1, seven_sixty_eight), eff_over1);
		pair32 const oct_over1 = select(again, add(octave, splat(1)), octave);
		pair32 const clamp_over = bit_and(over, ge_u(oct_over1, splat(7)));
		pair32 const oct_over = add(oct_over1, splat(1));

		pair32 index = select(under, eff_under, select(over, eff_over, as_u(eff)));
		pair32 const octave_out = select(under, oct_under, select(over, oct_over, octave));

		// the clamped lanes are replaced below, but their index still has to
		// be inside the table for the gather
		index = bit_and(index, splat(0x3ff));
		index = select(ge_u(index, splat(768)), splat(767), index);

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
		pair32 const pitched = shift_u(mul(tuned, load_u(&block.multiple[base])), splat(uint32_t(-4)));

		// Fixed-frequency mode replaces all of that. The fix registers occupy
		// the same space as detune and multiple, so those do not apply; the
		// plain step has too little resolution for the range, so a per
		// operator sub-step carries twelve more bits.
		pair32 const range = load_bytes(&block.range_reg[base]);
		pair32 fixed_freq = shift_u(field(range, 0, 0xf), splat(4));
		fixed_freq = select(eq(fixed_freq, zero), splat(8), fixed_freq);
		fixed_freq = bit_or(fixed_freq, field(load_bytes(&block.fine_reg[base]), 0, 0xf));
		fixed_freq = shift_u(fixed_freq, field(range, 4, 7));

		pair32 const substep = add(load_u(&block.substep[base]), mul(splat(75), fixed_freq));
		pair32 const fix_step = shift_u(substep, splat(uint32_t(-12)));
		pair32 const fix_off = eq(field(load_bytes(&block.fix_reg[base]), 5, 1), zero);
		pair32 const fix_mask = {vmvnq_u32(fix_off.lo), vmvnq_u32(fix_off.hi)};

		pair32 const kept = select(fix_mask, bit_and(substep, splat(0xfff)), load_u(&block.substep[base]));
		vst1q_u32(&block.substep[base], kept.lo);
		vst1q_u32(&block.substep[base + 4], kept.hi);

		pair32 const out = select(fix_mask, fix_step, pitched);
		vst1q_u32(&block.step[base], out.lo);
		vst1q_u32(&block.step[base + 4], out.hi);
	}
}

}

#endif
