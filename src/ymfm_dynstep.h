// The per-sample half of the phase step, for operators whose step moves
// because LFO pitch modulation or fixed-frequency mode makes it move.
//
// The scalar version of this is the one place in the core where a whole
// operator's worth of register decoding, a table lookup and two variable
// shifts happen for every operator on every sample. With pitch modulation on
// a four-operator patch that is 32 of them per sample, so it is worth doing a
// slot at a time.
//
// Everything that depends only on the registers is decoded by the caller when
// they change, and reaches this as parallel arrays; what is left per sample is
// the LFO's contribution, the octave overflow it can cause, and the table
// lookup. The per-channel entries are eight long because a slot's eight
// operators are its eight channels in order, so the same eight serve every
// slot.

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

	uint32_t const *eff_base;    // [count] keycode with the octave gaps removed
	uint32_t const *octave;      // [count]
	uint32_t const *delta;       // [count] coarse detune, in 1/64ths
	uint32_t const *detune;      // [count] detune by keycode
	uint32_t const *multiple;    // [count] frequency multiplier, x.4
	uint32_t const *fix_mask;    // [count] all ones in fixed-frequency mode
	uint32_t const *fix_rate;    // [count] 75 * the fixed frequency
	uint32_t *substep;           // [count] read and written, 12-bit fix remainder

	uint32_t const *pm_shift;    // [8] signed, negative shifts right
	uint32_t const *pm_live;     // [8] zero where sensitivity is zero
	uint32_t const *pm2_shift;   // [8]
	uint32_t const *pm2_live;    // [8]

	int32_t lfo_raw_pm;          // both LFOs' PM, packed low byte and high byte
	uint32_t count;              // operators, a multiple of 8
	bool any_fixed;              // whether any operator is in fixed-frequency mode
};

// Recompute every operator's phase step for this sample. Only defined for
// targets with a vector implementation; callers must check
// YMFM_HAVE_VECTOR_DYNSTEP.
void dyn_step_clock(dyn_step_block const &block);

}

#endif // YMFM_DYNSTEP_H
