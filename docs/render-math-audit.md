# Render math audit (2026-10-09)

Scope: the precision engine's entire `render()` call graph, both native SIMD
and scalar, float32 and float64, compiled by AppleClang on arm64 with `-O3`.
The TX81Z host's parameter-update path was also inspected separately.

## Render callback

`project_fast_envelope()` used `ceil(distance / step)` when a decay,
sustain, release or reverb threshold occurred within a 64-frame interval.
This emitted one `fdiv` in each floating-point instantiation. A bounded
binary search now finds the first crossing using multiplication and comparison,
with at most six iterations and no floating-to-integer quotient conversion.
The existing `scripts/check-render-kernel.py` rejects division instructions
and unexpected external symbols across the complete render translation unit.

The remaining source-level `std::abs` calls in `bounded_phase_offset()`,
`modulation()` and `fast_control()` are compiled inline; they do not call libm.
`integer_power()` uses integer-exponent multiplication, not `pow()`.
Oscillator, exponential gain and pitch modulation use preinitialized tables.
The audit permits only memory helpers and stack-protector symbols.

## Parameter updates: still contain math calls

Passing the render audit does **not** establish that a host's entire audio
callback is free of math calls. TX81Z's `YmfmOpzPort::generatePrecisionSamples()`
and `advanceTo()` loop over dirty voices and call `precisionUpdate()`, which
imports register parameters and calls `fm_engine::set_voice()` -> `cache()`.
Key/register processing can also call `precisionUpdate()` before key-on.

| Location | Operation | Frequency |
| --- | --- | --- |
| `ymfm_precision_import.h`, `import_common()` | `exp2` for note frequency | Once per imported voice |
| `ymfm_precision_import.h`, `import_lfo()` | `ldexp` for LFO frequency | Twice per imported OPZ voice |
| `ymfm_precision.h`, `cache()` | `exp2` for feedback | Once per voice update when feedback >= 1 |
| `ymfm_precision.h`, `cache()` operator loop | `exp2` for detune and envelope shift, `expm1` for attack | Per operator, four operators per voice update |
| TX81Z `YmfmOpzPort::setDetuneCents()` | `exp2` for global detune | When detune changes |

Parameter/cache/import code also contains runtime divisions, including
phase-step conversion, sample/noise clocks and the noise-frequency divisor.
They are outside `render()` and therefore outside the existing assembly audit.
The host's scheduling/time conversion code has additional integer divisions.
These update paths require a separate optimization and audit before claiming
that the complete host audio path has no division or mathematical library calls.

## Initialization only

Wave-table construction uses `acos`, `sin` and `cos`; exponential-table
construction uses `exp2`; envelope-rate construction uses `ldexp` and `log1p`.
`prepare()` calls `cache()` for all voices. Engine construction initializes
the tables before the audio callback. These calls must remain outside the
callback when the engine is instantiated or prepared by a host.

## Original chip cores

Source inspection of the original `ymfm` cores found no transcendental
math calls in their generation code. OPQ's detune code uses integer `std::abs`.
This is a source inspection, not a compiled audit of every original chip core.
