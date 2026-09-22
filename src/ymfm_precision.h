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

#ifndef YMFM_PRECISION_H
#define YMFM_PRECISION_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

// An opt-in OPM/OPZ synthesis path. The original integer cores are untouched.
// All continuous audio state uses Real; phase uses unsigned Q0.64 turns.
namespace ymfm { namespace precision {

enum class model { opm, opz };
enum class stage : uint8_t { off, attack, decay, sustain, release, reverb };
enum class lfo_wave : uint8_t { saw, square, triangle, noise, sine };

template<class Real> struct numeric_type {
    static_assert(std::is_same<Real, float>::value || std::is_same<Real, double>::value,
                  "select float (binary32) or double (binary64)");
    static_assert(std::numeric_limits<Real>::is_iec559, "IEEE floating point required");
    using type = Real;
};

// Avoid converting a value rounded to 2^64 to uint64_t (undefined behaviour).
// Signed offsets are reduced in floating point, converted as a positive
// magnitude, then negated with defined unsigned arithmetic. Do NOT add 1 to
// tiny negative offsets: doing so would discard their low phase bits.
template<class Real> inline uint64_t phase_offset(Real turns)
{
    if (!std::isfinite(turns)) return 0;
    Real fraction = std::fmod(turns, Real(1));
    Real magnitude = std::abs(fraction);
    uint64_t bits = static_cast<uint64_t>(std::ldexp(magnitude, 64));
    return fraction < Real(0) ? uint64_t(0) - bits : bits;
}

template<class Real> inline uint64_t phase_step(Real hz, Real sample_rate)
{
    // Phase conversion is a numeric boundary, not an audio sample operation.
    // Keep the division at least binary64 even for binary32 audio, otherwise
    // the Q0.64 accumulator would inherit a 24-bit frequency increment.
    return phase_offset(static_cast<double>(hz) / static_cast<double>(sample_rate));
}

// Cubic Hermite segments, with one-sided derivatives at the OPZ wave corners.
// Values and polynomial coefficients are Real, not converted chip log-ROMs.
// Coefficients are arranged by field, ready for a future gather/SIMD backend.
template<class Real> class wave_table : public numeric_type<Real> {
public:
    static constexpr unsigned bits = 12;
    static constexpr unsigned size = 1u << bits;
    static const wave_table& instance()
    {
        static const wave_table table;
        return table;
    }
    Real lookup(unsigned wave, uint64_t phase) const
    {
        unsigned index = unsigned(phase >> (64 - bits));
        constexpr uint64_t mask = (uint64_t(1) << (64 - bits)) - 1;
        Real t = std::ldexp(Real(phase & mask), -(64 - int(bits)));
        return ((m_c[3][wave][index] * t + m_c[2][wave][index]) * t
                + m_c[1][wave][index]) * t + m_c[0][wave][index];
    }
private:
    wave_table()
    {
        const long double pi = std::acos(-1.0L);
        for (unsigned w = 0; w < 8; ++w)
            for (unsigned i = 0; i < size; ++i) {
                const long double mid = (i + 0.5L) / size;
                const bool active = w < 2 || mid < 0.5L;
                const long double k = (w < 4 ? 2 : 4) * pi;
                const bool squared = (w & 1) != 0;
                const bool rectified = w >= 6;
                const long double sign = std::sin(k * mid) < 0 ? -1 : 1;
                auto point = [&](unsigned node, long double& y, long double& dy) {
                    long double x = static_cast<long double>(node) / size;
                    long double s = std::sin(k * x), c = std::cos(k * x);
                    // Exact zeros at the known quadrant boundaries.
                    unsigned half_period = w < 4 ? size / 2 : size / 4;
                    if (node % half_period == 0) s = 0;
                    y = squared ? s * s * (rectified ? 1 : sign)
                                : s * (rectified ? sign : 1);
                    dy = squared ? 2 * s * c * k * (rectified ? 1 : sign)
                                 : c * k * (rectified ? sign : 1);
                    if (!active) y = dy = 0;
                    dy /= size;
                };
                long double a, da, b, db;
                point(i, a, da); point(i + 1, b, db);
                m_c[0][w][i] = Real(a);
                m_c[1][w][i] = Real(da);
                m_c[2][w][i] = Real(3 * (b - a) - 2 * da - db);
                m_c[3][w][i] = Real(2 * (a - b) + da + db);
            }
    }
    Real m_c[4][8][size];
};

// Mean rates of the original OPM/OPZ EG step patterns. Integer patterns are
// timing constants, not audio state. Fractional rates interpolate their means.
// Attack uses the mean logarithmic multiplier, preserving exponential shape.
template<class Real> struct envelope_rates : numeric_type<Real> {
    std::array<Real, 64> decay{}, attack{};
    envelope_rates()
    {
        static const uint32_t patterns[64] = {
            0,0,0x10101010,0x10101010,0x10101010,0x10101010,0x11101110,0x11101110,
            0x10101010,0x10111010,0x11101110,0x11111110,
            0x10101010,0x10111010,0x11101110,0x11111110,
            0x10101010,0x10111010,0x11101110,0x11111110,
            0x10101010,0x10111010,0x11101110,0x11111110,
            0x10101010,0x10111010,0x11101110,0x11111110,
            0x10101010,0x10111010,0x11101110,0x11111110,
            0x10101010,0x10111010,0x11101110,0x11111110,
            0x10101010,0x10111010,0x11101110,0x11111110,
            0x10101010,0x10111010,0x11101110,0x11111110,
            0x10101010,0x10111010,0x11101110,0x11111110,
            0x11111111,0x21112111,0x21212121,0x22212221,
            0x22222222,0x42224222,0x42424242,0x44424442,
            0x44444444,0x84448444,0x84848484,0x88848884,
            0x88888888,0x88888888,0x88888888,0x88888888
        };
        for (unsigned r = 0; r < 64; ++r) {
            Real events = std::ldexp(Real(1), std::min(int(r / 4) - 11, 0));
            for (unsigned n = 0; n < 8; ++n) {
                Real inc = Real((patterns[r] >> (4 * n)) & 15);
                decay[r] += inc * events / Real(8);
                attack[r] += std::log1p(-inc / Real(16)) * events / Real(8);
            }
        }
    }
    Real at(const std::array<Real, 64>& table, Real rate) const
    {
        unsigned i = unsigned(rate);
        return i >= 63 ? table[63] : table[i] + (table[i + 1] - table[i]) * (rate - Real(i));
    }
};

template<class Real> struct operator_parameters : numeric_type<Real> {
    Real ratio = Real(1);
    Real detune_cents = Real(0);
    Real detune_hz = Real(0);
    Real fixed_hz = Real(440);
    bool fixed = false;
    bool fixed_pitch_modulation = false;
    unsigned waveform = 0;
    Real total_level = Real(0);       // 0..127, 0.75 nominal dB/unit
    Real attack = Real(127);         // 0..127 (including fractions)
    Real decay = Real(0);
    Real sustain_rate = Real(0);
    Real release = Real(80);
    Real sustain_level = Real(0);    // 0..127 maps to 0..992 attenuation units
    Real rate_scaling = Real(0);     // continuous additive effective EG rate, 0..31
    Real envelope_shift = Real(0);   // OPZ 0..3, attenuation / 2^shift
    Real reverb = Real(0);           // OPZ 0=disabled, otherwise 0..127 rate
    bool am_enabled = false;
};

template<class Real> struct lfo_parameters : numeric_type<Real> {
    Real frequency = Real(0);        // Hz, 0 freezes the LFO
    Real pitch_cents = Real(0);      // signed peak depth
    Real amplitude = Real(0);        // peak attenuation, 0..1023 units
    lfo_wave waveform = lfo_wave::triangle;
    bool key_sync = true;
};

template<class Real> struct voice_parameters : numeric_type<Real> {
    std::array<operator_parameters<Real>, 4> operators{};
    std::array<lfo_parameters<Real>, 2> lfos{};
    Real frequency = Real(440);
    Real feedback = Real(0);         // 0..127, legacy 0..7 mapped across range
    Real gain_left = Real(1);
    Real gain_right = Real(1);
    unsigned algorithm = 0;
    bool noise = false;             // replaces operator 4, like OPM/OPZ channel 7
    Real noise_frequency = Real(10000); // Hz, clocked at host rate
};

// Explicit physical/legacy conversions, with no truncation back to old bits.
template<class Real> struct legacy : numeric_type<Real> {
    static Real rate5(Real x) { return x * Real(127) / Real(31); }
    static Real release4(Real x) { return x * Real(127) / Real(15); }
    static Real feedback3(Real x) { return x * Real(127) / Real(7); }
    static Real sustain4(unsigned x)
    { return Real(x == 15 ? 31 : x) * Real(127) / Real(31); }
};

// All hot fields are structure-of-arrays with the voice index contiguous.
// Scalar kernels are operator-major / voice-minor. No virtual dispatch,
// per-voice allocation or locks. Future SIMD may use unaligned loads, or an
// aligned allocator; do not impose over-aligned C++14 heap requirements.
template<class Real, std::size_t Voices = 8> class fm_engine : public numeric_type<Real> {
    static_assert(Voices >= 1 && Voices <= 128, "1..128 voices");
    template<class T> using lanes = std::array<T, Voices>;
    struct operator_bank {
        lanes<uint64_t> phase{}, step{};
        lanes<Real> attenuation{}, error{}, output{}, frequency{}, level{}, shift{};
        lanes<Real> attack_delta{}, decay_step{}, sustain_step{}, release_step{}, reverb_step{};
        lanes<Real> sustain_level{};
        lanes<stage> state{};
        lanes<unsigned> waveform{};
        lanes<bool> am{}, pitch_modulated{};
    };
    struct lfo_bank {
        lanes<uint64_t> phase{}, step{};
        lanes<Real> pitch{}, amplitude{}, held{};
        lanes<uint32_t> random{};
        lanes<lfo_wave> waveform{};
        lanes<bool> sync{};
    };
public:
    using real_type = Real;
    using parameters = voice_parameters<Real>;
    static constexpr std::size_t voice_count = Voices;

    explicit fm_engine(model chip = model::opm) : m_model(chip), m_wave(wave_table<Real>::instance())
    {
        prepare(Real(48000)); // initialize tables/cache before entering the audio callback
    }
    bool prepare(Real sample_rate, Real reference_clock = Real(3579545))
    {
        if (!range(sample_rate, Real(8000), Real(768000)) ||
            !range(reference_clock, Real(100000), Real(100000000))) return false;
        m_rate = sample_rate; m_reference_clock = reference_clock;
        m_eg_ticks = reference_clock / Real(64 * 3) / sample_rate;
        reset();
        for (std::size_t v = 0; v < Voices; ++v) cache(v);
        return true;
    }
    void reset()
    {
        for (auto& op : m_op) {
            op.phase.fill(0); op.attenuation.fill(Real(1023)); op.error.fill(Real(0));
            op.output.fill(Real(0)); op.state.fill(stage::off);
        }
        for (auto& lfo : m_lfo) {
            lfo.phase.fill(0); lfo.held.fill(Real(0));
            for (std::size_t v = 0; v < Voices; ++v) lfo.random[v] = uint32_t(v + 1);
        }
        m_feedback0.fill(Real(0)); m_feedback1.fill(Real(0));
        m_noise_phase.fill(0); m_noise_latch_phase.fill(0); m_noise_rng.fill(1); m_noise_value.fill(Real(1));
        m_pm.fill(Real(0)); m_am.fill(Real(0));
    }
    bool set_voice(std::size_t voice, const parameters& p)
    {
        if (voice >= Voices || !valid(p)) return false;
        m_parameters[voice] = p;
        cache(voice);
        return true;
    }
    const parameters* get_voice(std::size_t voice) const
    { return voice < Voices ? &m_parameters[voice] : nullptr; }
    bool key_on(std::size_t v, unsigned mask = 15)
    {
        if (v >= Voices || mask > 15) return false;
        if (mask == 0) return true;
        if (mask & 1) m_feedback0[v] = m_feedback1[v] = Real(0);
        for (unsigned o = 0; o < 4; ++o) if (mask & (1u << o)) {
            auto& op = m_op[o];
            op.phase[v] = 0; op.error[v] = Real(0);
            op.state[v] = stage::attack;
            // Retrigger from current attenuation; phase resets as on OPM.
            if (attack_rate(m_parameters[v].operators[o]) >= Real(62)) {
                op.attenuation[v] = Real(0); op.state[v] = stage::decay;
            }
        }
        for (auto& l : m_lfo) if (l.sync[v]) l.phase[v] = 0;
        return true;
    }
    bool key_off(std::size_t v, unsigned mask = 15)
    {
        if (v >= Voices || mask > 15) return false;
        for (unsigned o = 0; o < 4; ++o)
            if ((mask & (1u << o)) && m_op[o].state[v] != stage::off)
                m_op[o].state[v] = stage::release;
        return true;
    }
    bool active(std::size_t v) const
    {
        if (v >= Voices) return false;
        for (const auto& op : m_op) if (op.state[v] != stage::off) return true;
        return false;
    }
    // No allocation. Arrays must be non-overlapping and have frames elements.
    // Controls are applied on the render thread between calls; no hidden locks.
    bool render(Real* left, Real* right, std::size_t frames)
    {
        if (frames && (!left || !right || left == right)) return false;
        for (std::size_t s = 0; s < frames; ++s) {
            modulation();
            for (unsigned o = 0; o < 4; ++o) operator_kernel(o);
            Real l = Real(0), r = Real(0);
            for (std::size_t v = 0; v < Voices; ++v) {
                Real sum = m_op[3].output[v];
                for (unsigned o = 0; o < 3; ++o)
                    if (m_carriers[v] & (1u << o)) sum += m_op[o].output[v];
                m_feedback0[v] = m_feedback1[v];
                m_feedback1[v] = m_op[0].output[v];
                l += sum * m_left[v]; r += sum * m_right[v];
            }
            left[s] = l; right[s] = r;
        }
        return true;
    }
    // Read-only diagnostic access; useful for numerical validation.
    Real attenuation(std::size_t v, unsigned o) const
    { return v < Voices && o < 4 ? m_op[o].attenuation[v] : Real(0); }
    uint64_t phase(std::size_t v, unsigned o) const
    { return v < Voices && o < 4 ? m_op[o].phase[v] : 0; }
private:
    static bool range(Real x, Real lo, Real hi)
    { return std::isfinite(x) && x >= lo && x <= hi; }
    bool valid(const parameters& p) const
    {
        if (!range(p.frequency, Real(0), Real(1000000)) || p.algorithm > 7 ||
            !range(p.feedback, Real(0), Real(127)) ||
            !range(p.gain_left, Real(0), Real(16)) || !range(p.gain_right, Real(0), Real(16)) ||
            !range(p.noise_frequency, Real(0), Real(1000000))) return false;
        for (const auto& o : p.operators) {
            if (!range(o.ratio, Real(0), Real(128)) ||
                !range(o.detune_cents, Real(-9600), Real(9600)) ||
                !range(o.detune_hz, Real(-100000), Real(100000)) ||
                !range(o.fixed_hz, Real(0), Real(1000000)) || o.waveform > (m_model == model::opz ? 7u : 0u) ||
                !range(o.total_level, Real(0), Real(127)) ||
                !range(o.attack, Real(0), Real(127)) || !range(o.decay, Real(0), Real(127)) ||
                !range(o.sustain_rate, Real(0), Real(127)) || !range(o.release, Real(0), Real(127)) ||
                !range(o.sustain_level, Real(0), Real(127)) ||
                !range(o.rate_scaling, Real(0), Real(31)) ||
                !range(o.envelope_shift, Real(0), Real(3)) || !range(o.reverb, Real(0), Real(127))) return false;
        }
        for (const auto& l : p.lfos)
            if (!range(l.frequency, Real(0), Real(2000)) ||
                !range(l.pitch_cents, Real(-9600), Real(9600)) ||
                !range(l.amplitude, Real(0), Real(1023)) || unsigned(l.waveform) > unsigned(lfo_wave::sine)) return false;
        return true;
    }
    static Real effective(Real raw, Real scaling)
    { return raw == Real(0) ? Real(0) : std::min(Real(63), raw + scaling); }
    static Real attack_rate(const operator_parameters<Real>& o)
    { return effective(o.attack * Real(62) / Real(127), o.rate_scaling); }
    void cache(std::size_t v)
    {
        const auto& p = m_parameters[v];
        // Operator numbering follows the FM graph, not register slot order.
        // Inputs are masks over the outputs already calculated this sample.
        static const unsigned routes[8][3] = {
            {1,2,4}, {0,3,4}, {0,2,5}, {1,0,6},
            {1,0,4}, {1,1,1}, {1,0,0}, {0,0,0}
        };
        static const unsigned carriers[8] = {0,0,0,0,2,6,6,7};
        for (unsigned o = 0; o < 3; ++o) m_routes[o][v] = routes[p.algorithm][o];
        m_carriers[v] = carriers[p.algorithm];
        m_left[v] = p.gain_left; m_right[v] = p.gain_right;
        // Full-scale operator signal is normalized to 1 (legacy approx 8192).
        // Legacy feedback phase = (a+b)*2^fb/128 turns; taper 0..1 smoothly.
        Real fb = p.feedback * Real(7) / Real(127);
        m_feedback_scale[v] = fb < Real(1) ? fb / Real(64) : std::exp2(fb) / Real(128);
        m_noise[v] = p.noise;
        // As on OPM/OPZ, clock the LFSR continuously and sample its state
        // at the requested noise rate. Changing the latch rate must not
        // change the polynomial's clock. Above the reference clock's range,
        // the extended API raises the generator rate to the requested rate.
        Real noise_clock = std::max(m_reference_clock / Real(32), p.noise_frequency);
        m_noise_step[v] = phase_step(noise_clock, m_rate);
        m_noise_whole[v] = uint32_t(noise_clock / m_rate);
        m_noise_latch_step[v] = phase_step(p.noise_frequency, noise_clock);
        m_noise_latch_every[v] = p.noise_frequency >= noise_clock;
        for (unsigned o = 0; o < 4; ++o) {
            const auto& a = p.operators[o]; auto& b = m_op[o];
            Real hz = a.fixed ? a.fixed_hz :
                (p.frequency * std::exp2(a.detune_cents / Real(1200)) + a.detune_hz) * a.ratio;
            b.frequency[v] = std::max(Real(0), hz);
            b.step[v] = phase_step(b.frequency[v], m_rate);
            b.pitch_modulated[v] = !a.fixed || a.fixed_pitch_modulation;
            b.level[v] = a.total_level * Real(8);
            b.shift[v] = m_model == model::opz ? std::exp2(-a.envelope_shift) : Real(1);
            b.waveform[v] = a.waveform; b.am[v] = a.am_enabled;
            b.sustain_level[v] = a.sustain_level * Real(992) / Real(127);
            b.attack_delta[v] = std::expm1(m_rates.at(m_rates.attack, attack_rate(a)) * m_eg_ticks);
            b.decay_step[v] = m_rates.at(m_rates.decay, effective(a.decay * Real(62) / Real(127), a.rate_scaling)) * m_eg_ticks;
            b.sustain_step[v] = m_rates.at(m_rates.decay, effective(a.sustain_rate * Real(62) / Real(127), a.rate_scaling)) * m_eg_ticks;
            b.release_step[v] = m_rates.at(m_rates.decay, effective(Real(2) + a.release * Real(60) / Real(127), a.rate_scaling)) * m_eg_ticks;
            b.reverb_step[v] = a.reverb == Real(0) ? b.release_step[v] :
                std::min(b.release_step[v], m_rates.at(m_rates.decay,
                    effective(Real(2) + a.reverb * Real(28) / Real(127), a.rate_scaling)) * m_eg_ticks);
        }
        for (unsigned i = 0; i < 2; ++i) {
            const auto& a = p.lfos[i]; auto& b = m_lfo[i];
            b.step[v] = phase_step(a.frequency, m_rate);
            b.pitch[v] = a.pitch_cents; b.amplitude[v] = a.amplitude;
            b.waveform[v] = a.waveform; b.sync[v] = a.key_sync;
        }
    }
    static uint32_t random_next(uint32_t& state)
    { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
    void modulation()
    {
        m_pm.fill(Real(0)); m_am.fill(Real(0));
        for (auto& b : m_lfo) for (std::size_t v = 0; v < Voices; ++v) {
            uint64_t before = b.phase[v]; b.phase[v] += b.step[v];
            if (b.phase[v] < before)
                b.held[v] = Real(random_next(b.random[v])) / Real(2147483648.0) - Real(1);
            Real p = std::ldexp(Real(b.phase[v] >> 11), -53);
            Real pm = Real(0), am = Real(0);
            switch (b.waveform[v]) {
                case lfo_wave::saw:
                    pm = p < Real(0.5) ? Real(2)*p : Real(2)*p-Real(2);
                    am = Real(1)-p; break;
                case lfo_wave::square:
                    pm = p < Real(0.5) ? Real(1) : Real(-1);
                    am = p < Real(0.5) ? Real(1) : Real(0); break;
                case lfo_wave::triangle:
                    pm = p < Real(0.25) ? Real(4)*p :
                         (p < Real(0.75) ? Real(2)-Real(4)*p : Real(4)*p-Real(4));
                    am = std::abs(Real(2)*p-Real(1)); break;
                case lfo_wave::noise: pm = b.held[v]; am = (pm+Real(1))/Real(2); break;
                case lfo_wave::sine: pm = m_wave.lookup(0, b.phase[v]); am = (pm+Real(1))/Real(2); break;
            }
            m_pm[v] += pm * b.pitch[v]; m_am[v] += am * b.amplitude[v];
        }
        for (std::size_t v = 0; v < Voices; ++v) if (m_noise[v]) {
            uint64_t before = m_noise_phase[v]; m_noise_phase[v] += m_noise_step[v];
            unsigned ticks = m_noise_whole[v] + unsigned(m_noise_phase[v] < before);
            while (ticks--) {
                // The OPM/OPZ 17-bit polynomial, with low history bits.
                uint32_t& state = m_noise_rng[v];
                state = (state << 1) | (((state >> 16) ^ (state >> 13) ^ 1) & 1);
                uint64_t previous = m_noise_latch_phase[v];
                m_noise_latch_phase[v] += m_noise_latch_step[v];
                if (m_noise_latch_every[v] || m_noise_latch_phase[v] < previous)
                    m_noise_value[v] = (state >> 17) & 1 ? Real(-1) : Real(1);
            }
        }
    }
    void operator_kernel(unsigned o)
    {
        auto& b = m_op[o];
        for (std::size_t v = 0; v < Voices; ++v) {
            Real& e = b.attenuation[v]; stage& st = b.state[v];
            // Carry sub-ULP increments rather than letting slow float32 EGs stall.
            auto add_envelope = [&](Real increment) {
                Real corrected = increment - b.error[v];
                Real next = e + corrected;
                b.error[v] = (next - e) - corrected;
                e = next;
            };
            if (st == stage::decay && e >= b.sustain_level[v]) st = stage::sustain;
            switch (st) {
                case stage::off: b.output[v] = Real(0); continue;
                case stage::attack:
                    add_envelope((e + Real(1)) * b.attack_delta[v]);
                    if (e <= Real(0)) { e = Real(0); b.error[v] = Real(0); st = stage::decay; }
                    break;
                case stage::decay:
                    add_envelope(b.decay_step[v]);
                    if (e >= b.sustain_level[v]) st = stage::sustain;
                    break;
                case stage::sustain: add_envelope(b.sustain_step[v]); break;
                case stage::release:
                    add_envelope(b.release_step[v]);
                    if (m_model == model::opz && e >= Real(192)) st = stage::reverb;
                    break;
                case stage::reverb: add_envelope(b.reverb_step[v]); break;
            }
            if (e >= Real(1023)) {
                e = Real(1023);
                // Zero-rate attack may remain silent indefinitely; do not
                // retire it, as a later rate edit can start the attack.
                if (st != stage::attack) st = stage::off;
            }
            uint64_t step = b.step[v];
            if (b.pitch_modulated[v] && m_pm[v] != Real(0))
                step = phase_step(b.frequency[v] * std::exp2(m_pm[v] / Real(1200)), m_rate);
            b.phase[v] += step;
            Real attenuation = e * b.shift[v] + b.level[v] + (b.am[v] ? m_am[v] : Real(0));
            if (st == stage::off || e >= Real(1023)) { b.output[v] = Real(0); continue; }
            if (o == 3 && m_noise[v]) {
                b.output[v] = m_noise_value[v] * std::max(Real(0), Real(1023) - attenuation) / Real(4096);
                continue;
            }
            Real offset = Real(0);
            if (o == 0) offset = (m_feedback0[v] + m_feedback1[v]) * m_feedback_scale[v];
            else for (unsigned j = 0; j < o; ++j)
                if (m_routes[o - 1][v] & (1u << j)) offset += m_op[j].output[v] * Real(4);
            // Modulation changes lookup phase, never the oscillator accumulator.
            uint64_t lookup = b.phase[v] + phase_offset(offset);
            b.output[v] = m_wave.lookup(b.waveform[v], lookup) * std::exp2(-attenuation / Real(64));
        }
    }
    model m_model;
    const wave_table<Real>& m_wave;
    envelope_rates<Real> m_rates;
    Real m_rate = Real(48000), m_reference_clock = Real(3579545), m_eg_ticks = Real(0);
    std::array<parameters, Voices> m_parameters{}; // cold control data
    std::array<operator_bank, 4> m_op{};
    std::array<lfo_bank, 2> m_lfo{};
    std::array<lanes<unsigned>, 3> m_routes{};
    lanes<unsigned> m_carriers{};
    lanes<Real> m_left{}, m_right{}, m_feedback0{}, m_feedback1{}, m_feedback_scale{}, m_pm{}, m_am{};
    lanes<uint64_t> m_noise_phase{}, m_noise_step{}, m_noise_latch_phase{}, m_noise_latch_step{};
    lanes<uint32_t> m_noise_rng{}, m_noise_whole{};
    lanes<Real> m_noise_value{};
    lanes<bool> m_noise{}, m_noise_latch_every{};
};

template<std::size_t Voices = 8> using fm_engine_f32 = fm_engine<float, Voices>;
template<std::size_t Voices = 8> using fm_engine_f64 = fm_engine<double, Voices>;

} } // namespace ymfm::precision
#endif
