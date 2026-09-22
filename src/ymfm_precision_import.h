// BSD 3-Clause License
//
// Copyright (c) 2021, Aaron Giles
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef YMFM_PRECISION_IMPORT_H
#define YMFM_PRECISION_IMPORT_H

#include "ymfm_precision.h"
#include "ymfm_opm.h"
#include "ymfm_opz.h"

namespace ymfm { namespace precision {
namespace detail {
// Detune timing constants from ymfm's detune_adjustment (fm.ipp).
inline int detune(unsigned setting, unsigned keycode)
{
    static const unsigned table[32][4] = {
        {0,0,1,2},{0,0,1,2},{0,0,1,2},{0,0,1,2},
        {0,1,2,2},{0,1,2,3},{0,1,2,3},{0,1,2,3},
        {0,1,2,4},{0,1,3,4},{0,1,3,4},{0,1,3,5},
        {0,2,4,5},{0,2,4,6},{0,2,4,6},{0,2,5,7},
        {0,2,5,8},{0,3,6,8},{0,3,6,9},{0,3,7,10},
        {0,4,8,11},{0,4,8,12},{0,4,9,13},{0,5,10,14},
        {0,5,11,16},{0,6,12,17},{0,6,13,19},{0,7,14,20},
        {0,8,16,22},{0,8,16,22},{0,8,16,22},{0,8,16,22}
    };
    int value = int(table[keycode][setting & 3]);
    return (setting & 4) ? -value : value;
}

template<class Real> inline lfo_parameters<Real> import_lfo(unsigned rate, unsigned wave,
    unsigned pm_depth, unsigned pm_sens, unsigned am_depth, unsigned am_sens, Real clock)
{
    lfo_parameters<Real> l;
    l.frequency = std::ldexp(Real(16 + (rate & 15)), int(rate >> 4) - 30) * clock / Real(64);
    l.waveform = static_cast<lfo_wave>(wave);
    const Real sensitivity[8] = {Real(0),Real(1)/32,Real(1)/16,Real(1)/8,
                                Real(1)/4,Real(1)/2,Real(2),Real(4)};
    l.pitch_cents = Real(pm_depth) * Real(100) / Real(64) * sensitivity[pm_sens];
    l.amplitude = am_sens ? Real(am_depth) * Real(255) / Real(128) * Real(1u << (am_sens - 1)) : Real(0);
    return l;
}

template<class Real, class Registers> bool import_common(const Registers& r,
    unsigned ch, Real clock, voice_parameters<Real>& result)
{
    if (ch >= 8 || !std::isfinite(clock) || clock < Real(100000) || clock > Real(100000000)) return false;
    voice_parameters<Real> p;
    unsigned bf = r.ch_block_freq(ch), keycode = (bf >> 8) & 31;
    unsigned code = (bf >> 6) & 15;
    // Equal temperament replaces the chip's quantized frequency ROM.
    // At the nominal 3.579545 MHz clock, block=0/code=0 is C#0.
    Real note = Real(13 + 12 * ((bf >> 10) & 7) + code - code / 4) + Real(bf & 63) / Real(64);
    p.frequency = Real(440) * std::exp2((note - Real(69)) / Real(12)) * clock / Real(3579545);
    p.algorithm = r.ch_algorithm(ch);
    p.feedback = legacy<Real>::feedback3(Real(r.ch_feedback(ch)));
    p.gain_left = Real(r.ch_output_0(ch)); p.gain_right = Real(r.ch_output_1(ch));
    p.noise = ch == 7 && r.noise_enable();
    p.noise_frequency = clock / Real(32) / Real(r.noise_frequency() + 1);
    p.lfos[0] = import_lfo<Real>(r.lfo_rate(), r.lfo_waveform(), r.lfo_pm_depth(),
        r.ch_lfo_pm_sens(ch), r.lfo_am_depth(), r.ch_lfo_am_sens(ch), clock);
    const unsigned offsets[4] = {0,16,8,24};
    const Real dt2[4] = {Real(0),Real(600),Real(781),Real(950)};
    for (unsigned i = 0; i < 4; ++i) {
        unsigned op = ch + offsets[i]; auto& a = p.operators[i];
        unsigned multiple = r.op_multiple(op);
        a.ratio = multiple ? Real(multiple) : Real(0.5);
        a.detune_cents = dt2[r.op_detune2(op)];
        a.detune_hz = Real(detune(r.op_detune(op), keycode)) * clock / Real(67108864);
        a.total_level = Real(r.op_total_level(op));
        a.attack = legacy<Real>::rate5(Real(r.op_attack_rate(op)));
        a.decay = legacy<Real>::rate5(Real(r.op_decay_rate(op)));
        a.sustain_rate = legacy<Real>::rate5(Real(r.op_sustain_rate(op)));
        a.release = legacy<Real>::release4(Real(r.op_release_rate(op)));
        a.sustain_level = legacy<Real>::sustain4(r.op_sustain_level(op));
        a.rate_scaling = Real(keycode >> (r.op_ksr(op) ^ 3));
        a.am_enabled = r.op_lfo_am_enable(op) != 0;
    }
    result = p;
    return true;
}
} // detail

// Snapshot conversion, not a register emulator. All subsequent edits use
// set_voice(). No timers, register writes, or integer audio run in this path.
// This imports patch parameters, not phase/envelope/save-state continuity.
template<class Real> bool import_opm(const opm_registers& r, unsigned channel,
    Real clock, voice_parameters<Real>& result)
{
    return detail::import_common(r, channel, clock, result);
}

template<class Real> bool import_opz(const opz_registers& r, unsigned channel,
    Real clock, voice_parameters<Real>& result)
{
    voice_parameters<Real> p;
    if (!detail::import_common(r, channel, clock, p)) return false;
    p.lfos[0].key_sync = r.lfo_sync() != 0;
    p.lfos[1] = detail::import_lfo<Real>(r.lfo2_rate(), r.lfo2_waveform(), r.lfo2_pm_depth(),
        r.ch_lfo2_pm_sens(channel), r.lfo2_am_depth(), r.ch_lfo2_am_sens(channel), clock);
    p.lfos[1].key_sync = r.lfo2_sync() != 0;
    const unsigned offsets[4] = {0,16,8,24};
    for (unsigned i = 0; i < 4; ++i) {
        unsigned op = channel + offsets[i]; auto& a = p.operators[i];
        a.waveform = r.op_waveform(op);
        unsigned multiple = r.op_multiple(op) << 4;
        a.ratio = Real((multiple ? multiple : 8) | r.op_fine(op)) / Real(16);
        a.fixed = r.op_fix_mode(op) != 0;
        unsigned freq = r.op_fix_frequency(op) << 4;
        a.fixed_hz = Real(((freq ? freq : 8) | r.op_fine(op)) << r.op_fix_range(op));
        a.envelope_shift = i == 0 ? Real(0) : Real(r.op_eg_shift(op));
        a.reverb = Real(r.op_reverb_rate(op)) * Real(127) / Real(7);
    }
    // Upstream does not implement the undocumented OPZ channel-volume byte.
    // Keep unity gain here; set_voice provides explicit gains in either model.
    result = p;
    return true;
}

} }
#endif
