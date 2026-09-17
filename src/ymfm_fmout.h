// Output stage for four-operator channels, split per instruction set the same
// way the envelope is. See ymfm_condition.h for how the target is chosen.

#ifndef YMFM_FMOUT_H
#define YMFM_FMOUT_H

#pragma once

#include "ymfm_condition.h"

#include <cstdint>

namespace ymfm
{

// 10-bit mantissas with an implied leading bit, matching the internal format
// of the OPN die. The implicit 0x400 is pre-incorporated and the values are
// left-shifted by 2 so a plain right shift finishes the conversion; the order
// is reversed to save a NOT on the input.
extern uint16_t const g_power_table[256];

// One sample of eight four-operator channels.
//
// Per-operator arrays are indexed by operator number, which puts a slot's
// eight channels in eight consecutive entries; slot_base gives the first
// operator of each slot. Per-channel arrays are indexed by channel.
struct fm_output_block
{
	uint32_t const *phase;             // [ops] 10.10 phase
	uint32_t const *env_atten;         // [ops] envelope attenuation
	uint32_t const *eg_shift;          // [ops] envelope shift
	uint32_t const *total_level;       // [ops] total level, scaled
	uint32_t const *am_mask;           // [ops] all ones where AM applies
	uint16_t const *const *waveform;   // [ops] waveform base per operator
	uint32_t const *slot_base;         // [4]

	uint32_t const *am_offset;         // [8] LFO AM amount per channel
	uint32_t const *fb_shift;          // [8] 10 - feedback
	uint32_t const *fb_mask;           // [8] zero when feedback is off
	int32_t const *fb0;                // [8] operator-1 output two samples back
	int32_t const *fb1;                // [8] and one sample back
	// read and written: channels that are not clocked this sample keep the
	// operator-1 value they had, exactly as skipping them would
	int32_t *fb_in;                    // [8] operator-1 output for this sample
	uint32_t const *active;            // [8] all ones when the channel is clocked
	uint32_t const *algorithm;         // [8] packed wiring word
	uint32_t const *out0_mask;         // [8]
	uint32_t const *out1_mask;         // [8]
	uint32_t const *contributes;       // [8] active and routed somewhere
	int32_t clipmax;
};

// Eight channels' LFO AM offsets at once. The sensitivity fields sit in the
// low bits of eight consecutive register bytes, so the whole set loads without
// any gathering; a sensitivity of zero means no AM rather than a shift of -1,
// so each term is masked instead of branched on.
void lfo_am_offsets_x8(uint8_t const *sens0, uint8_t const *sens1, uint32_t am0, uint32_t am1, uint32_t *out);

// Accumulates the eight channels into the two outputs. Only defined for
// targets with a vector implementation; callers must check
// YMFM_HAVE_VECTOR_OUTPUT.
void fm_output_4op_x8(fm_output_block const &block, int32_t &out0, int32_t &out1);

}

#endif // YMFM_FMOUT_H
