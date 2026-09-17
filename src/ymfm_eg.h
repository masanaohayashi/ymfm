// Envelope update, split out so each instruction set can implement the same
// function in its own source file.
//
// This is the hottest loop in the core and the one place where every operator
// on the chip does identical, independent work, so it is where processing a
// whole slot of channels at once actually applies. It is deliberately not a
// template: the per-ISA files have to define it for one concrete shape.

#ifndef YMFM_EG_H
#define YMFM_EG_H

#pragma once

#include "ymfm_condition.h"

#include <cstdint>

namespace ymfm
{

// One chip's worth of envelope state, as parallel arrays rather than a struct
// per operator. Operator numbering puts a slot's eight channels in eight
// consecutive entries, so a slot loads without any shuffling.
struct eg_block
{
	uint32_t *atten;          // [count] current attenuation, 4.6 format
	uint32_t *state;          // [count] current envelope_state
	uint32_t const *sustain;  // [count] sustain level, shifted
	uint32_t *cur_rate;       // [count] rate for the current state
	uint32_t *cur_inc;        // [count] packed increments for the current state
	uint32_t const *rate_of;  // [EG_STATES][count]
	uint32_t const *inc_of;   // [EG_STATES][count]
	uint32_t count;           // operators, a multiple of 8
	bool has_reverb;          // whether release decays on into a reverb state
};

// Advance every operator's envelope by one envelope tick.
void eg_clock(eg_block const &block, uint32_t env_counter);

}

#endif // YMFM_EG_H
