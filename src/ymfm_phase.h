// Phase accumulation, split out so each instruction set can implement the same
// function in its own source file.
//
// Every operator on the chip advances its own 10.10 phase by its own step,
// with no interaction between operators, so this is the most uniform work in
// the core. The cost in the scalar version was never the addition itself but
// reaching each operator's step through its own heap object; the step lives in
// a parallel array here so a whole slot loads at once.

#ifndef YMFM_PHASE_H
#define YMFM_PHASE_H

#pragma once

#include "ymfm_condition.h"

#include <cstdint>

namespace ymfm
{

// Advance every operator's phase by one sample. count is a multiple of 8;
// padding entries carry a step of zero and so stay put.
void phase_clock(uint32_t *phase, uint32_t const *step, uint32_t count);

}

#endif // YMFM_PHASE_H
