// Processor selection for the hand-written SIMD paths.
//
// Everything here comes from compiler predefined macros, so no build system
// has to pass flags and every target picks its own path. Each ISA gets a whole
// source file implementing the same functions; the ones that do not match the
// target compile to nothing, so all of them can sit in the build unconditionally.

#ifndef YMFM_CONDITION_H
#define YMFM_CONDITION_H

#pragma once

// MSVC defines WIN32 on every Windows target including ARM64, so ARM is tested
// first. Windows ARM64 needs its own file because MSVC's NEON types differ.
#if defined(_M_ARM64) || defined(_M_ARM64EC) \
    || defined(__aarch64__) || defined(__arm64__) \
    || defined(__ARM_NEON) || defined(__ARM_NEON__)
    #define YMFM_USE_SSE_INTRINSIC   0
    #define YMFM_USE_NEON_INTRINSIC  1
    #define YMFM_USE_NO_INTRINSIC    0
#elif defined(__i386__) || defined(__x86_64__) \
    || defined(_M_IX86) || defined(_M_X64)
    #define YMFM_USE_SSE_INTRINSIC   1
    #define YMFM_USE_NEON_INTRINSIC  0
    #define YMFM_USE_NO_INTRINSIC    0
#else
    #define YMFM_USE_SSE_INTRINSIC   0
    #define YMFM_USE_NEON_INTRINSIC  0
    #define YMFM_USE_NO_INTRINSIC    1
#endif

// Set this to build the scalar reference on a machine that has NEON or SSE, so
// the two can be run against the same golden hash.
#if defined(YMFM_FORCE_NO_INTRINSIC)
    #undef YMFM_USE_SSE_INTRINSIC
    #undef YMFM_USE_NEON_INTRINSIC
    #undef YMFM_USE_NO_INTRINSIC
    #define YMFM_USE_SSE_INTRINSIC   0
    #define YMFM_USE_NEON_INTRINSIC  0
    #define YMFM_USE_NO_INTRINSIC    1
#endif

// Which stages have a hand-written implementation for this target. The scalar
// versions compile exactly where these are 0, so a target with no vector
// implementation still links; adding an instruction set means adding its file
// and flipping its flag here, not touching the call sites.
#if YMFM_USE_NEON_INTRINSIC
    #define YMFM_HAVE_VECTOR_EG  1
#else
    #define YMFM_HAVE_VECTOR_EG  0
#endif

// Set where a vector output stage exists, so the engine knows to use it.
#if defined(YMFM_HAVE_VECTOR_OUTPUT_OVERRIDE)
    #define YMFM_HAVE_VECTOR_OUTPUT  YMFM_HAVE_VECTOR_OUTPUT_OVERRIDE
#elif YMFM_USE_NEON_INTRINSIC
    #define YMFM_HAVE_VECTOR_OUTPUT  1
#else
    #define YMFM_HAVE_VECTOR_OUTPUT  0
#endif

#if YMFM_USE_SSE_INTRINSIC
    #include <emmintrin.h>
    #include <xmmintrin.h>
#elif YMFM_USE_NEON_INTRINSIC
    #include <arm_neon.h>
#endif

#endif // YMFM_CONDITION_H
