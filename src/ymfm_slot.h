// The two pieces of per-channel bookkeeping the sample loop does around the
// kernels, with one implementation per instruction set.
//
// Both are a single vector wide, so they live in a header the way the LFO does
// rather than in a source file per instruction set: at this size the call to
// reach an out-of-line kernel costs more than the work. The compiler does not
// vectorize either of them on its own, which is why they are written out.

#ifndef YMFM_SLOT_H
#define YMFM_SLOT_H

#pragma once

#include "ymfm_condition.h"

#include <cstdint>

namespace ymfm
{

#if YMFM_USE_NEON_INTRINSIC

// Advance eight channels' operator-1 feedback shift register by one sample.
inline void feedback_clock_x8(int32_t *fb0, int32_t *fb1, int32_t const *fb_in)
{
	int32x4_t const lo = vld1q_s32(&fb1[0]);
	int32x4_t const hi = vld1q_s32(&fb1[4]);
	vst1q_s32(&fb0[0], lo);
	vst1q_s32(&fb0[4], hi);
	vst1q_s32(&fb1[0], vld1q_s32(&fb_in[0]));
	vst1q_s32(&fb1[4], vld1q_s32(&fb_in[4]));
}

// Spread a channel mask into one lane mask per channel, and combine it with
// the channels that are routed somewhere.
inline void channel_masks_x8(uint32_t chanmask, uint32_t const *out_any, uint32_t *active, uint32_t *contributes)
{
	int32_t const lanes_lo[4] = {0, -1, -2, -3};
	int32_t const lanes_hi[4] = {-4, -5, -6, -7};
	uint32x4_t const one = vdupq_n_u32(1);
	uint32x4_t const bits = vdupq_n_u32(chanmask);
	uint32x4_t const lo = vtstq_u32(vshlq_u32(bits, vld1q_s32(lanes_lo)), one);
	uint32x4_t const hi = vtstq_u32(vshlq_u32(bits, vld1q_s32(lanes_hi)), one);
	vst1q_u32(&active[0], lo);
	vst1q_u32(&active[4], hi);
	vst1q_u32(&contributes[0], vandq_u32(lo, vld1q_u32(&out_any[0])));
	vst1q_u32(&contributes[4], vandq_u32(hi, vld1q_u32(&out_any[4])));
}

#elif YMFM_USE_SSE_INTRINSIC

inline void feedback_clock_x8(int32_t *fb0, int32_t *fb1, int32_t const *fb_in)
{
	__m128i const lo = _mm_loadu_si128(reinterpret_cast<__m128i const *>(&fb1[0]));
	__m128i const hi = _mm_loadu_si128(reinterpret_cast<__m128i const *>(&fb1[4]));
	_mm_storeu_si128(reinterpret_cast<__m128i *>(&fb0[0]), lo);
	_mm_storeu_si128(reinterpret_cast<__m128i *>(&fb0[4]), hi);
	_mm_storeu_si128(reinterpret_cast<__m128i *>(&fb1[0]),
		_mm_loadu_si128(reinterpret_cast<__m128i const *>(&fb_in[0])));
	_mm_storeu_si128(reinterpret_cast<__m128i *>(&fb1[4]),
		_mm_loadu_si128(reinterpret_cast<__m128i const *>(&fb_in[4])));
}

inline void channel_masks_x8(uint32_t chanmask, uint32_t const *out_any, uint32_t *active, uint32_t *contributes)
{
	// SSE2 has no per-lane variable shift, so the bits are selected by anding
	// against one bit per lane and comparing that back
	__m128i const bits = _mm_set1_epi32(int32_t(chanmask));
	__m128i const sel_lo = _mm_set_epi32(8, 4, 2, 1);
	__m128i const sel_hi = _mm_set_epi32(128, 64, 32, 16);
	__m128i const lo = _mm_cmpeq_epi32(_mm_and_si128(bits, sel_lo), sel_lo);
	__m128i const hi = _mm_cmpeq_epi32(_mm_and_si128(bits, sel_hi), sel_hi);
	_mm_storeu_si128(reinterpret_cast<__m128i *>(&active[0]), lo);
	_mm_storeu_si128(reinterpret_cast<__m128i *>(&active[4]), hi);
	_mm_storeu_si128(reinterpret_cast<__m128i *>(&contributes[0]),
		_mm_and_si128(lo, _mm_loadu_si128(reinterpret_cast<__m128i const *>(&out_any[0]))));
	_mm_storeu_si128(reinterpret_cast<__m128i *>(&contributes[4]),
		_mm_and_si128(hi, _mm_loadu_si128(reinterpret_cast<__m128i const *>(&out_any[4]))));
}

#else

inline void feedback_clock_x8(int32_t *fb0, int32_t *fb1, int32_t const *fb_in)
{
	for (uint32_t chnum = 0; chnum < 8; chnum++)
	{
		fb0[chnum] = fb1[chnum];
		fb1[chnum] = fb_in[chnum];
	}
}

inline void channel_masks_x8(uint32_t chanmask, uint32_t const *out_any, uint32_t *active, uint32_t *contributes)
{
	for (uint32_t chnum = 0; chnum < 8; chnum++)
	{
		uint32_t const clocked = ((chanmask >> chnum) & 1) ? 0xffffffffu : 0u;
		active[chnum] = clocked;
		contributes[chnum] = clocked & out_any[chnum];
	}
}

#endif

}

#endif // YMFM_SLOT_H
