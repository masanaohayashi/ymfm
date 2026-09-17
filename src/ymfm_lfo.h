// The LFO half of the noise-and-LFO clock, with one implementation per
// instruction set selected by ymfm_condition.h.
//
// A chip with two LFOs runs the same seven steps on each of them, and the
// depth scaling doubles that into four independent multiplies, so the whole
// thing is one short vector's worth of work. The waveform fetch in the middle
// is a table index and has to be scalar; everything on either side of it is
// not.
//
// Unlike the envelope, phase and output stages this lives in a header rather
// than a source file per instruction set. Those are big enough that the call
// to reach them is free; this one is about twenty instructions, and measured
// out of line it cost more than it saved. Nothing here needs an instruction
// set beyond what every target of the vector paths already has, so there is no
// per-file compiler flag to lose by inlining it.

#ifndef YMFM_LFO_H
#define YMFM_LFO_H

#pragma once

#include "ymfm_condition.h"

#include <cstdint>

namespace ymfm
{

// Two LFOs' worth of state, as parallel pairs rather than a struct each.
struct lfo_block
{
	uint32_t *counter;          // [2] phase counter, position is bits 22-29
	uint32_t const *rate;       // [2] raw rate register, a 4.4 step with implied 1
	int16_t const *const *wave; // [2] selected waveform, AM low 8 bits, PM upper 8
	int16_t *noise_wave;        // the noise waveform, written one position ahead
	uint32_t noise_value;       // what to write there
	uint32_t const *am_depth;   // [2]
	uint32_t const *pm_depth;   // [2]
	uint8_t *am_out;            // [2] AM amount for the channels to scale
};

#if YMFM_USE_NEON_INTRINSIC

// Advance both LFOs by one sample, returning the two PM values packed into the
// low and high bytes the way the phase step expects them.
inline int32_t lfo_clock(lfo_block const &block)
{
	// Both counters step together. The rate is a 4.4 value with an implied
	// leading 1, so the step is a per-lane variable shift.
	uint32x2_t const rate = vld1_u32(block.rate);
	uint32x2_t const step = vshl_u32(
		vorr_u32(vand_u32(rate, vdup_n_u32(0x0f)), vdup_n_u32(0x10)),
		vreinterpret_s32_u32(vshr_n_u32(rate, 4)));
	uint32x2_t const counter = vadd_u32(vld1_u32(block.counter), step);
	vst1_u32(block.counter, counter);

	// the position is the top eight bits of the 30-bit counter
	uint32x2_t const position = vshr_n_u32(vshl_n_u32(counter, 2), 24);
	uint32_t const pos0 = vget_lane_u32(position, 0);
	uint32_t const pos1 = vget_lane_u32(position, 1);

	// fill in the noise entry one ahead of the current position; this keeps
	// the current value stable for a full LFO clock and effectively latches
	// the running value when the LFO advances
	block.noise_wave[(pos0 + 1) & 0xff] = int16_t(block.noise_value);
	block.noise_wave[(pos1 + 1) & 0xff] = int16_t(block.noise_value);

	// The waveform fetch is a table index, so it is the one part that leaves
	// the vector registers. AM sits in the low eight bits and PM, signed, in
	// the upper eight; both LFOs' halves make four independent multiplies, so
	// they go as one quad rather than two pairs.
	int32x2_t const ampm = vset_lane_s32(block.wave[1][pos1],
		vset_lane_s32(block.wave[0][pos0], vdup_n_s32(0), 0), 1);
	int32x4_t const parts = vcombine_s32(
		vand_s32(ampm, vdup_n_s32(0xff)), vshr_n_s32(ampm, 8));
	int32x4_t const depth = vcombine_s32(
		vreinterpret_s32_u32(vld1_u32(block.am_depth)),
		vreinterpret_s32_u32(vld1_u32(block.pm_depth)));
	int32x4_t const scaled = vshrq_n_s32(vmulq_s32(parts, depth), 7);

	block.am_out[0] = uint8_t(vgetq_lane_s32(scaled, 0));
	block.am_out[1] = uint8_t(vgetq_lane_s32(scaled, 1));
	return (vgetq_lane_s32(scaled, 2) & 0xff) | (vgetq_lane_s32(scaled, 3) << 8);
}

#elif YMFM_USE_SSE_INTRINSIC

inline int32_t lfo_clock(lfo_block const &block)
{
	// SSE2 has no per-lane variable shift, so the two steps are built from
	// the scalar rates; everything after the waveform fetch is still one
	// four-wide multiply.
	uint32_t position[2];
	for (uint32_t which = 0; which < 2; which++)
	{
		uint32_t const rate = block.rate[which];
		block.counter[which] += (0x10 | (rate & 0x0f)) << ((rate >> 4) & 0x0f);
		position[which] = (block.counter[which] >> 22) & 0xff;
	}

	block.noise_wave[(position[0] + 1) & 0xff] = int16_t(block.noise_value);
	block.noise_wave[(position[1] + 1) & 0xff] = int16_t(block.noise_value);

	int32_t const ampm0 = block.wave[0][position[0]];
	int32_t const ampm1 = block.wave[1][position[1]];
	__m128i const parts = _mm_set_epi32(ampm1 >> 8, ampm0 >> 8, ampm1 & 0xff, ampm0 & 0xff);
	__m128i const depth = _mm_set_epi32(int32_t(block.pm_depth[1]), int32_t(block.pm_depth[0]),
		int32_t(block.am_depth[1]), int32_t(block.am_depth[0]));

	// SSE2 multiplies 32-bit lanes only as two 64-bit products, so the odd
	// lanes are shuffled down, multiplied, and interleaved back
	__m128i const even = _mm_mul_epu32(parts, depth);
	__m128i const odd = _mm_mul_epu32(_mm_srli_si128(parts, 4), _mm_srli_si128(depth, 4));
	__m128i const product = _mm_unpacklo_epi64(
		_mm_unpacklo_epi32(even, odd), _mm_unpackhi_epi32(even, odd));
	__m128i const scaled = _mm_srai_epi32(product, 7);

	int32_t out[4];
	_mm_storeu_si128(reinterpret_cast<__m128i *>(out), scaled);
	block.am_out[0] = uint8_t(out[0]);
	block.am_out[1] = uint8_t(out[1]);
	return (out[2] & 0xff) | (out[3] << 8);
}

#else

inline int32_t lfo_clock(lfo_block const &block)
{
	uint32_t position[2];
	for (uint32_t which = 0; which < 2; which++)
	{
		uint32_t const rate = block.rate[which];
		block.counter[which] += (0x10 | (rate & 0x0f)) << ((rate >> 4) & 0x0f);
		position[which] = (block.counter[which] >> 22) & 0xff;
	}

	block.noise_wave[(position[0] + 1) & 0xff] = int16_t(block.noise_value);
	block.noise_wave[(position[1] + 1) & 0xff] = int16_t(block.noise_value);

	int32_t pm[2];
	for (uint32_t which = 0; which < 2; which++)
	{
		int32_t const ampm = block.wave[which][position[which]];
		block.am_out[which] = uint8_t(((ampm & 0xff) * int32_t(block.am_depth[which])) >> 7);
		pm[which] = ((ampm >> 8) * int32_t(block.pm_depth[which])) >> 7;
	}
	return (pm[0] & 0xff) | (pm[1] << 8);
}

#endif

}

#endif // YMFM_LFO_H
