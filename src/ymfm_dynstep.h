// The per-sample half of the phase step, for operators whose step moves
// because LFO pitch modulation or fixed-frequency mode makes it move.
//
// The scalar version of this is the one place in the core where a whole
// operator's worth of register decoding, a table lookup and two variable
// shifts happen for every operator on every sample. With pitch modulation on
// a four-operator patch that is 32 of them per sample, so it is worth doing a
// slot at a time.
//
// The block carries raw pointers into the register file rather than decoded
// values, because the layout it is built for puts a slot's eight channels in
// eight consecutive bytes of every register block; decoding is eight lanes of
// shift-and-mask and there is nothing to cache. The caller is what knows the
// register offsets.

#ifndef YMFM_DYNSTEP_H
#define YMFM_DYNSTEP_H

#pragma once

#include "ymfm_condition.h"

#include <cstdint>

namespace ymfm
{

struct dyn_step_block
{
	uint32_t *step;              // [count] out: this sample's phase step
	uint32_t const *block_freq;  // [count] cached block, keycode and fraction
	uint32_t const *detune;      // [count] cached detune adjustment
	uint32_t const *multiple;    // [count] cached frequency multiplier, x.4

	uint8_t const *detune2_reg;  // [count] coarse detune in bits 6-7
	uint8_t const *fix_reg;      // [count] fixed-frequency mode in bit 5
	uint8_t const *range_reg;    // [count] fix range in bits 4-6, frequency in 0-3
	uint8_t const *fine_reg;     // [count] fix fine in bits 0-3
	uint8_t const *pm_sens_reg;  // [8] LFO 1 pitch sensitivity in bits 4-6
	uint8_t const *pm2_sens_reg; // [8] LFO 2 pitch sensitivity in bits 4-6

	uint32_t *substep;           // [count] read and written, 12-bit fix remainder
	int32_t lfo_raw_pm;          // both LFOs' PM, packed low byte and high byte
	uint32_t count;              // operators, a multiple of 8
};

// Recompute every operator's phase step for this sample. Only defined for
// targets with a vector implementation; callers must check
// YMFM_HAVE_VECTOR_DYNSTEP.
void dyn_step_clock(dyn_step_block const &block);

}

#endif // YMFM_DYNSTEP_H
