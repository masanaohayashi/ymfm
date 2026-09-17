// NEON output stage: one sample of eight four-operator channels.
//
// The eight channels are the lanes. Operators inside a channel are serially
// dependent, so they stay four sequential steps, but each step now covers the
// whole chip. Both halves of every operation are written out rather than
// looped so the core can issue them down separate SIMD pipes.

#include "ymfm_fmout.h"

#if YMFM_HAVE_VECTOR_OUTPUT

namespace ymfm
{

namespace
{

// "quiet" envelope level, above which an operator contributes nothing. This
// tests the raw attenuation, before the envelope shift is applied, so it is
// not a pure optimisation and cannot be dropped.
constexpr uint32_t EG_QUIET = 0x380;

struct pair32
{
	uint32x4_t lo, hi;
};

struct pairs32
{
	int32x4_t lo, hi;
};

inline pair32 load_u(uint32_t const *p) { return {vld1q_u32(p), vld1q_u32(p + 4)}; }
inline pairs32 load_s(int32_t const *p) { return {vld1q_s32(p), vld1q_s32(p + 4)}; }

// Gather four lanes, each from its own waveform, by moving indices out to
// general registers and the results back in. Going through memory instead
// costs a store-to-load stall on every operator.
inline uint32x4_t gather_wave(uint16_t const *const *wave, uint32_t base, uint32x4_t index)
{
	uint32x4_t result = vdupq_n_u32(wave[base + 0][vgetq_lane_u32(index, 0)]);
	result = vsetq_lane_u32(wave[base + 1][vgetq_lane_u32(index, 1)], result, 1);
	result = vsetq_lane_u32(wave[base + 2][vgetq_lane_u32(index, 2)], result, 2);
	result = vsetq_lane_u32(wave[base + 3][vgetq_lane_u32(index, 3)], result, 3);
	return result;
}

// Same for the shared power table, where only the index varies per lane.
inline uint32x4_t gather_power(uint32x4_t input)
{
	uint32_t const i0 = vgetq_lane_u32(input, 0);
	uint32_t const i1 = vgetq_lane_u32(input, 1);
	uint32_t const i2 = vgetq_lane_u32(input, 2);
	uint32_t const i3 = vgetq_lane_u32(input, 3);
	uint32x4_t result = vdupq_n_u32(g_power_table[i0 & 0xff] >> (i0 >> 8));
	result = vsetq_lane_u32(g_power_table[i1 & 0xff] >> (i1 >> 8), result, 1);
	result = vsetq_lane_u32(g_power_table[i2 & 0xff] >> (i2 >> 8), result, 2);
	result = vsetq_lane_u32(g_power_table[i3 & 0xff] >> (i3 >> 8), result, 3);
	return result;
}

inline pairs32 sel_s(pair32 mask, pairs32 a, pairs32 b)
{
	return {vbslq_s32(mask.lo, a.lo, b.lo), vbslq_s32(mask.hi, a.hi, b.hi)};
}

// Truncate to 16 bits with sign extension: the reference keeps these
// intermediate operator values in an int16 array, and the wrap is audible.
inline pairs32 to_int16(pairs32 v)
{
	return {vshrq_n_s32(vshlq_n_s32(v.lo, 16), 16),
			vshrq_n_s32(vshlq_n_s32(v.hi, 16), 16)};
}

inline pairs32 add16(pairs32 a, pairs32 b)
{
	return to_int16({vaddq_s32(a.lo, b.lo), vaddq_s32(a.hi, b.hi)});
}

// One operator across all eight channels.
pairs32 compute_volume(fm_output_block const &block, uint32_t slot, pairs32 opmod,
	pair32 am_offset)
{
	uint32_t const base = block.slot_base[slot];

	// Operators whose envelope is effectively off contribute nothing. The same
	// mask drives the per-lane result below and the whole-group skip here, so
	// the two cannot disagree: when every lane is quiet the masked result is
	// all zeroes anyway, and skipping avoids both gathers. Real patches leave
	// whole slots silent for long stretches, which is where this pays.
	pair32 const atten = load_u(&block.env_atten[base]);
	pair32 const quiet = {
		vcgtq_u32(atten.lo, vdupq_n_u32(EG_QUIET)),
		vcgtq_u32(atten.hi, vdupq_n_u32(EG_QUIET))};
	if (vminvq_u32(vandq_u32(quiet.lo, quiet.hi)) == 0xffffffffu)
		return {vdupq_n_s32(0), vdupq_n_s32(0)};

	// phase() is the top ten bits of the 10.10 accumulator, plus the
	// modulation, wrapped to the waveform length
	pair32 phase = load_u(&block.phase[base]);
	uint32x4_t const idxL = vandq_u32(
		vaddq_u32(vshrq_n_u32(phase.lo, 10), vreinterpretq_u32_s32(opmod.lo)), vdupq_n_u32(0x3ff));
	uint32x4_t const idxH = vandq_u32(
		vaddq_u32(vshrq_n_u32(phase.hi, 10), vreinterpretq_u32_s32(opmod.hi)), vdupq_n_u32(0x3ff));

	// Each lane reads its own waveform, so this is a manual gather. The
	// tables are a few kilobytes and stay in L1.
	pair32 const sin_atten = {gather_wave(block.waveform, base, idxL),
							  gather_wave(block.waveform, base + 4, idxH)};

	// envelope attenuation: shifted, plus AM where enabled, plus total level,
	// clamped, then scaled to 4.8
	pair32 const shift = load_u(&block.eg_shift[base]);
	pair32 const total = load_u(&block.total_level[base]);
	pair32 const ammask = load_u(&block.am_mask[base]);

	uint32x4_t envL = vshlq_u32(atten.lo, vnegq_s32(vreinterpretq_s32_u32(shift.lo)));
	uint32x4_t envH = vshlq_u32(atten.hi, vnegq_s32(vreinterpretq_s32_u32(shift.hi)));
	envL = vaddq_u32(envL, vandq_u32(am_offset.lo, ammask.lo));
	envH = vaddq_u32(envH, vandq_u32(am_offset.hi, ammask.hi));
	envL = vminq_u32(vaddq_u32(envL, total.lo), vdupq_n_u32(0x3ff));
	envH = vminq_u32(vaddq_u32(envH, total.hi), vdupq_n_u32(0x3ff));

	uint32x4_t const inL = vaddq_u32(vandq_u32(sin_atten.lo, vdupq_n_u32(0x7fff)),
		vshlq_n_u32(envL, 2));
	uint32x4_t const inH = vaddq_u32(vandq_u32(sin_atten.hi, vdupq_n_u32(0x7fff)),
		vshlq_n_u32(envH, 2));

	// the power table is shared by every lane, but the index is not, so this
	// is a gather as well
	pair32 const vol = {gather_power(inL), gather_power(inH)};

	// the top bit of the waveform entry is the sign of the sine
	uint32x4_t const negL = vtstq_u32(sin_atten.lo, vdupq_n_u32(0x8000));
	uint32x4_t const negH = vtstq_u32(sin_atten.hi, vdupq_n_u32(0x8000));
	pairs32 result = {
		vbslq_s32(negL, vnegq_s32(vreinterpretq_s32_u32(vol.lo)), vreinterpretq_s32_u32(vol.lo)),
		vbslq_s32(negH, vnegq_s32(vreinterpretq_s32_u32(vol.hi)), vreinterpretq_s32_u32(vol.hi))};

	return sel_s(quiet, {vdupq_n_s32(0), vdupq_n_s32(0)}, result);
}

// Per-lane pick out of the opout slots. The algorithm table only ever names a
// few of the eight, and which ones is fixed per operator, so each call selects
// over just the reachable set rather than walking all eight.
template<unsigned... Slots>
pairs32 pick(pairs32 const (&opout)[8], pair32 index)
{
	constexpr unsigned slots[] = {Slots...};
	pairs32 result = opout[slots[0]];
	for (unsigned entry = 1; entry < sizeof...(Slots); entry++)
	{
		unsigned const slot = slots[entry];
		pair32 const hit = {
			vceqq_u32(index.lo, vdupq_n_u32(slot)),
			vceqq_u32(index.hi, vdupq_n_u32(slot))};
		result = sel_s(hit, opout[slot], result);
	}
	return result;
}

inline pairs32 shift_right_1(pairs32 v)
{
	return {vshrq_n_s32(v.lo, 1), vshrq_n_s32(v.hi, 1)};
}

inline int32_t horizontal_add(pairs32 v)
{
	return vaddvq_s32(vaddq_s32(v.lo, v.hi));
}

}

void fm_output_4op_x8(fm_output_block const &block, int32_t &out0, int32_t &out1)
{
	pair32 const am_offset = load_u(block.am_offset);
	pair32 const fb_shift = load_u(block.fb_shift);
	pair32 const fb_mask = load_u(block.fb_mask);
	pair32 const algorithm = load_u(block.algorithm);
	pair32 const active = load_u(block.active);
	pair32 const contributes = load_u(block.contributes);
	pairs32 const fb_sum = load_s(block.feedback_sum);

	// operator 1 takes its own previous output back as modulation
	pairs32 const opmod1 = {
		vandq_s32(vshlq_s32(fb_sum.lo, vnegq_s32(vreinterpretq_s32_u32(fb_shift.lo))),
			vreinterpretq_s32_u32(fb_mask.lo)),
		vandq_s32(vshlq_s32(fb_sum.hi, vnegq_s32(vreinterpretq_s32_u32(fb_shift.hi))),
			vreinterpretq_s32_u32(fb_mask.hi))};

	pairs32 const op1 = compute_volume(block, 0, opmod1, am_offset);

	// channels that are not clocked keep their previous operator-1 value
	pairs32 const previous = load_s(block.feedback_io);
	pairs32 const stored = sel_s(active, to_int16(op1), previous);
	vst1q_s32(&block.feedback_io[0], stored.lo);
	vst1q_s32(&block.feedback_io[4], stored.hi);

	pairs32 opout[8];
	int32x4_t const zero = vdupq_n_s32(0);
	opout[0] = {zero, zero};
	opout[1] = to_int16(op1);

	pair32 const bit0 = {vandq_u32(algorithm.lo, vdupq_n_u32(1)),
						 vandq_u32(algorithm.hi, vdupq_n_u32(1))};
	opout[2] = to_int16(compute_volume(block, 1, shift_right_1(pick<0, 1>(opout, bit0)), am_offset));
	opout[5] = add16(opout[1], opout[2]);

	pair32 const bits1 = {vandq_u32(vshrq_n_u32(algorithm.lo, 1), vdupq_n_u32(7)),
						  vandq_u32(vshrq_n_u32(algorithm.hi, 1), vdupq_n_u32(7))};
	opout[3] = to_int16(compute_volume(block, 2, shift_right_1(pick<0, 1, 2, 5>(opout, bits1)), am_offset));
	opout[6] = add16(opout[1], opout[3]);
	opout[7] = add16(opout[2], opout[3]);

	pair32 const bits4 = {vandq_u32(vshrq_n_u32(algorithm.lo, 4), vdupq_n_u32(7)),
						  vandq_u32(vshrq_n_u32(algorithm.hi, 4), vdupq_n_u32(7))};
	pairs32 result = compute_volume(block, 3, shift_right_1(pick<0, 1, 2, 3, 6, 7>(opout, bits4)), am_offset);

	// optionally fold in operators 1, 2 and 3, clamping after each
	int32x4_t const clipmax = vdupq_n_s32(block.clipmax);
	int32x4_t const clipmin = vdupq_n_s32(-block.clipmax - 1);
	for (unsigned index = 1; index <= 3; index++)
	{
		pair32 const use = {
			vtstq_u32(algorithm.lo, vdupq_n_u32(1u << (6 + index))),
			vtstq_u32(algorithm.hi, vdupq_n_u32(1u << (6 + index)))};
		pairs32 const summed = {
			vminq_s32(vmaxq_s32(vaddq_s32(result.lo, opout[index].lo), clipmin), clipmax),
			vminq_s32(vmaxq_s32(vaddq_s32(result.hi, opout[index].hi), clipmin), clipmax)};
		result = sel_s(use, summed, result);
	}

	// route to the two outputs and fold the lanes down once, rather than
	// adding each channel in separately
	result = sel_s(contributes, result, {zero, zero});
	pair32 const out0_mask = load_u(block.out0_mask);
	pair32 const out1_mask = load_u(block.out1_mask);
	out0 += horizontal_add({vandq_s32(result.lo, vreinterpretq_s32_u32(out0_mask.lo)),
							vandq_s32(result.hi, vreinterpretq_s32_u32(out0_mask.hi))});
	out1 += horizontal_add({vandq_s32(result.lo, vreinterpretq_s32_u32(out1_mask.lo)),
							vandq_s32(result.hi, vreinterpretq_s32_u32(out1_mask.hi))});
}

}

#endif
