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
#include <cstring>
#include <limits>
#include <type_traits>

// An opt-in OPM/OPZ synthesis path. The original integer cores are untouched.
// All continuous audio state uses Real; phase uses unsigned Q0.64 turns.
namespace ymfm { namespace precision {

enum class model { opm, opz };
enum class stage : uint8_t { off, attack, decay, sustain, release, reverb, sustain_hold };
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
    Real magnitude = std::abs(turns);
    // Above this limit every representable value is already an integer.
    constexpr Real integral_limit = std::is_same<Real, float>::value
        ? Real(8388608.0) : Real(4503599627370496.0);
    if (magnitude >= integral_limit) return 0;
    Real fraction = magnitude - Real(static_cast<uint64_t>(magnitude));
    uint64_t bits = static_cast<uint64_t>(fraction * Real(18446744073709551616.0));
    return turns < Real(0) ? uint64_t(0) - bits : bits;
}

// Render-only conversions. Parameter validation bounds PM offsets to +/-16
// turns and modulated oscillator increments below 2^52 turns/sample.
// Public phase_offset() retains its finite/large-input checks.
template<class Real> inline uint64_t bounded_phase_offset(Real turns)
{
    Real fraction = turns - Real(static_cast<int64_t>(turns));
    uint64_t bits = static_cast<uint64_t>(std::abs(fraction) * Real(18446744073709551616.0));
    return fraction < Real(0) ? uint64_t(0) - bits : bits;
}
inline uint64_t positive_phase_offset(double turns)
{
    double fraction = turns - static_cast<double>(static_cast<uint64_t>(turns));
    return static_cast<uint64_t>(fraction * 18446744073709551616.0);
}

template<class Real> inline uint64_t phase_step(Real hz, Real sample_rate)
{
    // Phase conversion is a numeric boundary, not an audio sample operation.
    // Keep the division at least binary64 even for binary32 audio, otherwise
    // the Q0.64 accumulator would inherit a 24-bit frequency increment.
    return phase_offset(static_cast<double>(hz) * (1.0 / static_cast<double>(sample_rate)));
}

// Native 128-bit packets on AArch64/Clang; keep a scalar build for cross-checking.
// This does not change the precision of any lane.
template<class Real> struct simd_packet;
#if defined(__aarch64__) && defined(__clang__) && !defined(YMFM_PRECISION_DISABLE_SIMD)
template<> struct simd_packet<float> {
    using type = float __attribute__((vector_size(16)));
    static type load(const float* p) { type v; std::memcpy(&v,p,sizeof(v)); return v; }
    static void gather(const float* const* c, type& c0, type& c1, type& c2, type& c3) {
        type a=load(c[0]), b=load(c[1]), d=load(c[2]), e=load(c[3]);
        type ab0=__builtin_shufflevector(a,b,0,4,1,5);
        type ab1=__builtin_shufflevector(a,b,2,6,3,7);
        type de0=__builtin_shufflevector(d,e,0,4,1,5);
        type de1=__builtin_shufflevector(d,e,2,6,3,7);
        c0=__builtin_shufflevector(ab0,de0,0,1,4,5);
        c1=__builtin_shufflevector(ab0,de0,2,3,6,7);
        c2=__builtin_shufflevector(ab1,de1,0,1,4,5);
        c3=__builtin_shufflevector(ab1,de1,2,3,6,7);
    }
};
template<> struct simd_packet<double> {
    using type = double __attribute__((vector_size(16)));
    static type load(const double* p) { type v; std::memcpy(&v,p,sizeof(v)); return v; }
    static void gather(const double* const* c, type& c0, type& c1, type& c2, type& c3) {
        type a=load(c[0]), b=load(c[1]), d=load(c[0]+2), e=load(c[1]+2);
        c0=__builtin_shufflevector(a,b,0,2); c1=__builtin_shufflevector(a,b,1,3);
        c2=__builtin_shufflevector(d,e,0,2); c3=__builtin_shufflevector(d,e,1,3);
    }
};
#endif

// Cubic Hermite segments, with one-sided derivatives at the OPZ wave corners.
// Values and polynomial coefficients are Real, not converted chip log-ROMs.
// Keep one segment's four coefficients together to fetch a single cache line.
// Voice state remains SoA; a SIMD backend can gather these segment records.
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
        Real t = Real(phase & mask) * Real(1.0 / 4503599627370496.0);
        return ((m_c[wave][index][3] * t + m_c[wave][index][2]) * t
                + m_c[wave][index][1]) * t + m_c[wave][index][0];
    }
    static constexpr unsigned packet_size = 16 / sizeof(Real);
    void lookup_packet(const unsigned* waves, const uint64_t* phases,
                       const Real* gains, Real* output) const
    {
#if defined(__aarch64__) && defined(__clang__) && !defined(YMFM_PRECISION_DISABLE_SIMD)
        using vector = typename simd_packet<Real>::type;
        vector t{}, c0, c1, c2, c3, gain;
        const Real* coefficients[packet_size];
        for (unsigned n = 0; n < packet_size; ++n) {
            unsigned index = unsigned(phases[n] >> (64 - bits));
            constexpr uint64_t mask = (uint64_t(1) << (64 - bits)) - 1;
            t[n] = Real(phases[n] & mask) * Real(1.0 / 4503599627370496.0);
            coefficients[n] = m_c[waves[n]][index];
        }
        simd_packet<Real>::gather(coefficients, c0, c1, c2, c3);
        std::memcpy(&gain, gains, sizeof(gain));
        vector result = (((c3 * t + c2) * t + c1) * t + c0) * gain;
        std::memcpy(output, &result, sizeof(result));
#else
        for (unsigned n = 0; n < packet_size; ++n)
            output[n] = lookup(waves[n], phases[n]) * gains[n];
#endif
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
                m_c[w][i][0] = Real(a);
                m_c[w][i][1] = Real(da);
                m_c[w][i][2] = Real(3 * (b - a) - 2 * da - db);
                m_c[w][i][3] = Real(2 * (a - b) + da + db);
            }
    }
    Real m_c[8][size][4];
};

// Bounded base-2 exponential for the audio kernel: -64 <= x <= 16.
// A 1/64-octave table plus a local Taylor polynomial; no libm or division.
// Fractional input is retained (including in double mode), not quantized to
// the table index. The table is constructed before rendering begins.
template<class Real> class exponential_table : public numeric_type<Real> {
public:
    static const exponential_table& instance()
    {
        static const exponential_table table;
        return table;
    }
    Real lookup(Real x) const
    {
        int index = int(x * Real(64));
        Real r = x - Real(index) * Real(0.015625);
        Real p;
        if (std::is_same<Real, float>::value)
            p = Real(1) + r * (Real(0.6931471805599453094) + r *
                (Real(0.2402265069591007123) + r * Real(0.05550410866482157995)));
        else
            p = Real(1) + r * (Real(0.6931471805599453094) + r *
                (Real(0.2402265069591007123) + r * (Real(0.05550410866482157995) + r *
                (Real(0.00961812910762847716) + r * Real(0.00133335581464284434)))));
        return m_value[unsigned(index + 4096)] * p;
    }
    void lookup_packet(const Real* x, Real* output) const
    {
        constexpr unsigned width = wave_table<Real>::packet_size;
#if defined(__aarch64__) && defined(__clang__) && !defined(YMFM_PRECISION_DISABLE_SIMD)
        using vector = typename simd_packet<Real>::type;
        vector r{}, scale{};
        for (unsigned n = 0; n < width; ++n) {
            int index = int(x[n] * Real(64));
            r[n] = x[n] - Real(index) * Real(0.015625);
            scale[n] = m_value[unsigned(index + 4096)];
        }
        vector polynomial;
        if (std::is_same<Real, float>::value)
            polynomial = Real(1) + r * (Real(0.6931471805599453094) + r *
                (Real(0.2402265069591007123) + r * Real(0.05550410866482157995)));
        else
            polynomial = Real(1) + r * (Real(0.6931471805599453094) + r *
                (Real(0.2402265069591007123) + r * (Real(0.05550410866482157995) + r *
                (Real(0.00961812910762847716) + r * Real(0.00133335581464284434)))));
        vector result = scale * polynomial;
        std::memcpy(output, &result, sizeof(result));
#else
        for (unsigned n = 0; n < width; ++n) output[n] = lookup(x[n]);
#endif
    }
private:
    exponential_table()
    {
        for (int i = -4096; i <= 1024; ++i)
            m_value[unsigned(i + 4096)] = Real(std::exp2(static_cast<long double>(i) / 64));
    }
    std::array<Real, 5121> m_value;
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

// Jump a sleeping LFO's xorshift generator without replaying audio samples.
// Powers of its linear GF(2) transition are initialized on the control path.
class random_jump_table {
public:
    static uint32_t next(uint32_t& state)
    { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
    static const random_jump_table& instance()
    { static const random_jump_table table; return table; }
    uint32_t advance(uint32_t state, uint64_t count) const
    {
        unsigned bit = 0;
        while (count) {
            if (count & 1) state = apply(m_power[bit], state);
            count >>= 1; ++bit;
        }
        return state;
    }
private:
    using matrix = std::array<uint32_t, 32>;
    static uint32_t apply(const matrix& m, uint32_t state)
    {
        uint32_t result = 0;
        for (unsigned bit = 0; state; ++bit, state >>= 1)
            if (state & 1) result ^= m[bit];
        return result;
    }
    random_jump_table()
    {
        for (unsigned bit = 0; bit < 32; ++bit) {
            uint32_t basis = uint32_t(1) << bit;
            m_power[0][bit] = next(basis);
        }
        for (unsigned power = 1; power < 64; ++power)
            for (unsigned bit = 0; bit < 32; ++bit)
                m_power[power][bit] = apply(m_power[power-1], m_power[power-1][bit]);
    }
    std::array<matrix, 64> m_power;
};

inline uint64_t multiply_high(uint64_t a, uint64_t b)
{
    uint64_t al = uint32_t(a), bl = uint32_t(b), ah = a >> 32, bh = b >> 32;
    uint64_t low = al * bl;
    uint64_t mid = ah * bl + (low >> 32);
    uint64_t carry = mid >> 32;
    mid = al * bh + uint32_t(mid);
    return ah * bh + carry + (mid >> 32);
}

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
        lanes<uint64_t> phase{}, step{}, time{};
        lanes<Real> pitch{}, amplitude{}, held{};
        lanes<uint32_t> random{};
        lanes<lfo_wave> waveform{};
        lanes<bool> sync{}, enabled{};
    };
public:
    using real_type = Real;
    using parameters = voice_parameters<Real>;
    static constexpr std::size_t voice_count = Voices;

    explicit fm_engine(model chip = model::opm) : m_model(chip), m_wave(wave_table<Real>::instance()), m_exp(exponential_table<Real>::instance()), m_random_jump(random_jump_table::instance())
    {
        prepare(Real(48000)); // initialize tables/cache before entering the audio callback
    }
    bool prepare(Real sample_rate, Real reference_clock = Real(3579545))
    {
        if (!range(sample_rate, Real(8000), Real(768000)) ||
            !range(reference_clock, Real(100000), Real(100000000))) return false;
        m_rate = sample_rate; m_reference_clock = reference_clock;
        m_inverse_rate = 1.0 / static_cast<double>(sample_rate);
        m_eg_ticks = reference_clock / Real(64 * 3) / sample_rate;
        reset();
        for (std::size_t v = 0; v < Voices; ++v) cache(v);
        return true;
    }
    void reset()
    {
        m_time = 0; m_awake_count = 0; m_awake.fill(false); m_retire = false;
        for (auto& op : m_op) {
            op.phase.fill(0); op.attenuation.fill(Real(1023)); op.error.fill(Real(0));
            op.output.fill(Real(0)); op.state.fill(stage::off);
        }
        for (auto& lfo : m_lfo) {
            lfo.phase.fill(0); lfo.time.fill(0); lfo.held.fill(Real(0));
            for (std::size_t v = 0; v < Voices; ++v) lfo.random[v] = uint32_t(v + 1);
        }
        m_feedback0.fill(Real(0)); m_feedback1.fill(Real(0));
        m_noise_phase.fill(0); m_noise_latch_phase.fill(0); m_noise_rng.fill(1); m_noise_value.fill(Real(1));
        m_pm.fill(Real(0)); m_am.fill(Real(0));
    }
    bool set_voice(std::size_t voice, const parameters& p)
    {
        if (voice >= Voices || !valid(p)) return false;
        sync_lfos(voice);
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
        sync_lfos(v);
        if (!m_awake[v]) { m_awake[v] = true; ++m_awake_count; }
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
        if (!frames) return true;
        if (!m_awake_count) {
            std::fill_n(left, frames, Real(0)); std::fill_n(right, frames, Real(0));
            m_time += frames;
            return true;
        }
        // Build sparse work lists once per block; no unused-slot work per sample.
        m_live_count = m_noise_count = 0;
        m_lfo_count.fill(0);
        for (std::size_t v = 0; v < Voices; ++v) {
            if (!m_awake[v]) continue;
            if (m_noise[v]) m_noise_live[m_noise_count++] = v;
            m_live[m_live_count++] = v;
            m_pm[v] = m_am[v] = Real(0); m_pitch_factor[v] = Real(1);
            for (unsigned i = 0; i < 2; ++i) if (m_lfo[i].enabled[v]) {
                sync_lfo(m_lfo[i], v);
                m_lfo_live[i][m_lfo_count[i]++] = v;
            }
        }
        if (!m_live_count && !m_noise_count) {
            std::fill_n(left, frames, Real(0)); std::fill_n(right, frames, Real(0));
            m_time += frames;
            return true;
        }
        prepare_steady_kernels();
        if (m_steady_ready[0] || m_steady_ready[1] || m_steady_ready[2] || m_steady_ready[3])
            render_dispatch<true>(left, right, frames);
        else
            render_dispatch<false>(left, right, frames);
        m_time += frames;
        for (unsigned i = 0; i < 2; ++i)
            for (std::size_t n = 0; n < m_lfo_count[i]; ++n)
                m_lfo[i].time[m_lfo_live[i][n]] = m_time;
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
        // While the voice is awake, clock the LFSR and sample its state
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
            if (b.state[v] == stage::sustain_hold && b.sustain_step[v] != Real(0))
                b.state[v] = stage::sustain;
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
            b.enabled[v] = a.pitch_cents != Real(0) || a.amplitude != Real(0);
        }
    }
    static uint32_t random_next(uint32_t& state)
    { return random_jump_table::next(state); }
    void sync_lfo(lfo_bank& b, std::size_t v)
    {
        uint64_t elapsed = m_time - b.time[v];
        if (!elapsed) return;
        uint64_t delta = b.step[v] * elapsed, previous = b.phase[v];
        b.phase[v] += delta;
        uint64_t wraps = multiply_high(b.step[v], elapsed) + uint64_t(b.phase[v] < previous);
        if (wraps) {
            b.random[v] = m_random_jump.advance(b.random[v], wraps);
            b.held[v] = Real(b.random[v]) * Real(1.0 / 2147483648.0) - Real(1);
        }
        b.time[v] = m_time;
    }
    void sync_lfos(std::size_t v)
    { for (auto& b : m_lfo) sync_lfo(b, v); }
    void modulation()
    {
        for (std::size_t n = 0; n < m_live_count; ++n) {
            std::size_t v = m_live[n]; m_pm[v] = m_am[v] = Real(0);
        }
        for (unsigned i = 0; i < 2; ++i) {
          auto& b = m_lfo[i];
          for (std::size_t n = 0; n < m_lfo_count[i]; ++n) {
            std::size_t v = m_lfo_live[i][n];
            uint64_t before = b.phase[v]; b.phase[v] += b.step[v];
            if (b.phase[v] < before)
                b.held[v] = Real(random_next(b.random[v])) * Real(1.0 / 2147483648.0) - Real(1);
            Real p = Real(b.phase[v] >> 11) * Real(1.0 / 9007199254740992.0);
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
                case lfo_wave::noise: pm = b.held[v]; am = (pm+Real(1))*Real(0.5); break;
                case lfo_wave::sine: pm = m_wave.lookup(0, b.phase[v]); am = (pm+Real(1))*Real(0.5); break;
            }
            m_pm[v] += pm * b.pitch[v]; m_am[v] += am * b.amplitude[v];
        }
        }
        for (std::size_t n = 0; n < m_live_count; ++n) {
            std::size_t v = m_live[n];
            m_pitch_factor[v] = m_pm[v] == Real(0) ? Real(1) : m_exp.lookup(m_pm[v] * Real(1.0 / 1200.0));
        }
    }
    void noise_kernel()
    {
        for (std::size_t n = 0; n < m_noise_count; ++n) {
            std::size_t v = m_noise_live[n];
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
    void retire_finished(uint64_t now)
    {
        m_retire = false;
        m_steady_ready.fill(false); // retirement can split a contiguous run
        std::size_t count = 0;
        for (std::size_t n = 0; n < m_live_count; ++n) {
            std::size_t v = m_live[n];
            if (active(v)) m_live[count++] = v;
            else {
                m_awake[v] = false; --m_awake_count;
                for (auto& lfo : m_lfo) if (lfo.enabled[v]) lfo.time[v] = now;
            }
        }
        m_live_count = count;
        for (unsigned i = 0; i < 2; ++i) {
            count = 0;
            for (std::size_t n = 0; n < m_lfo_count[i]; ++n) {
                std::size_t v = m_lfo_live[i][n];
                if (m_awake[v]) m_lfo_live[i][count++] = v;
            }
            m_lfo_count[i] = count;
        }
        count = 0;
        for (std::size_t n = 0; n < m_noise_count; ++n)
            if (m_awake[m_noise_live[n]]) m_noise_live[count++] = m_noise_live[n];
        m_noise_count = count;
    }
    template<bool Steady> void render_dispatch(Real* left, Real* right, std::size_t frames)
    {
        // Controls cannot change within a block. Without LFOs the general EG
        // kernel can omit per-operator PM/AM loads and phase-step conversion.
        // Keep the existing singleton kernel: specialization benefits polyphony.
        if (m_live_count == 1 || m_lfo_count[0] || m_lfo_count[1]) render_samples<Steady, true>(left, right, frames);
        else render_samples<Steady, false>(left, right, frames);
    }
    template<bool Steady, bool Modulated> void render_samples(Real* left, Real* right, std::size_t frames)
    {
        for (std::size_t s = 0; s < frames; ++s) {
            if (Modulated && (m_lfo_count[0] || m_lfo_count[1])) modulation();
            if (m_noise_count) noise_kernel();
            // A singleton specialization removes four loop prologues/backedges.
            // Recheck after each sample: retirement may change the live count.
            if (m_live_count == 1) {
                operator_kernel<0, true, Steady, Modulated>(); operator_kernel<1, true, Steady, Modulated>();
                operator_kernel<2, true, Steady, Modulated>(); operator_kernel<3, true, Steady, Modulated>();
            } else {
                operator_kernel<0, false, Steady, Modulated>(); operator_kernel<1, false, Steady, Modulated>();
                operator_kernel<2, false, Steady, Modulated>(); operator_kernel<3, false, Steady, Modulated>();
            }
            Real l = Real(0), r = Real(0);
            for (std::size_t n = 0; n < m_live_count; ++n) {
                std::size_t v = m_live[n];
                Real sum = m_op[3].output[v];
                for (unsigned o = 0; o < 3; ++o)
                    if (m_carriers[v] & (1u << o)) sum += m_op[o].output[v];
                m_feedback0[v] = m_feedback1[v];
                m_feedback1[v] = m_op[0].output[v];
                l += sum * m_left[v]; r += sum * m_right[v];
            }
            left[s] = l; right[s] = r;
            if (m_retire) {
                retire_finished(m_time + s + 1);
                if (!m_live_count) {
                    std::fill_n(left + s + 1, frames - s - 1, Real(0));
                    std::fill_n(right + s + 1, frames - s - 1, Real(0));
                    break;
                }
            }
        }
    }
    void prepare_steady_kernels()
    {
        m_steady_ready.fill(false);
        // Held EGs have a constant base attenuation. LFO modulation remains
        // sample-accurate; controls are applied between render calls.
        if (m_live_count < wave_table<Real>::packet_size) return;
        m_steady_first = m_live[0];
        if (m_live[m_live_count - 1] != m_steady_first + m_live_count - 1) return;
        for (unsigned o = 0; o < 4; ++o) {
            if (o == 3 && m_noise_count) continue;
            auto& b = m_op[o];
            bool held = true, pitch = false, amplitude = false;
            for (std::size_t n = 0; n < m_live_count; ++n) {
                std::size_t v = m_steady_first + n;
                if (b.state[v] != stage::sustain_hold) { held = false; break; }
                bool amplitude_lfo = m_lfo[0].amplitude[v] != Real(0) || m_lfo[1].amplitude[v] != Real(0);
                amplitude |= b.am[v] && amplitude_lfo;
                pitch |= b.pitch_modulated[v] &&
                    (m_lfo[0].pitch[v] != Real(0) || m_lfo[1].pitch[v] != Real(0));
            }
            if (!held) continue;
            m_steady_ready[o] = true; m_steady_pitch[o] = pitch; m_steady_am[o] = amplitude;
            for (std::size_t n = 0; n < m_live_count; ++n) {
                std::size_t v = m_steady_first + n;
                m_steady_attenuation[o][v] = b.attenuation[v] * b.shift[v] + b.level[v];
                if (!amplitude) m_steady_gain[o][v] = m_exp.lookup(-m_steady_attenuation[o][v] * Real(0.015625));
            }
        }
    }
    template<unsigned o, bool Pitch> uint64_t steady_phase(std::size_t v)
    {
        auto& b = m_op[o];
        uint64_t step = b.step[v];
        if (Pitch && b.pitch_modulated[v] && m_pm[v] != Real(0))
            step = positive_phase_offset(static_cast<double>(b.frequency[v] * m_pitch_factor[v]) * m_inverse_rate);
        b.phase[v] += step;
        Real offset = Real(0);
        if (o == 0) offset = (m_feedback0[v] + m_feedback1[v]) * m_feedback_scale[v];
        else for (unsigned j = 0; j < o; ++j)
            if (m_routes[o - 1][v] & (1u << j)) offset += m_op[j].output[v] * Real(4);
        return b.phase[v] + bounded_phase_offset(offset);
    }
    template<unsigned o, bool Pitch, bool Amplitude> void steady_kernel()
    {
        constexpr unsigned width = wave_table<Real>::packet_size;
        auto& b = m_op[o];
        std::size_t v = m_steady_first, end = v + m_live_count;
        for (; v + width <= end; v += width) {
            uint64_t phases[width];
            for (unsigned n = 0; n < width; ++n) phases[n] = steady_phase<o, Pitch>(v + n);
            if (Amplitude) {
                Real attenuation[width], gains[width];
                for (unsigned n = 0; n < width; ++n)
                    attenuation[n] = -(m_steady_attenuation[o][v+n] + (b.am[v+n] ? m_am[v+n] : Real(0))) * Real(0.015625);
                m_exp.lookup_packet(attenuation, gains);
                m_wave.lookup_packet(&b.waveform[v], phases, gains, &b.output[v]);
            } else
                m_wave.lookup_packet(&b.waveform[v], phases, &m_steady_gain[o][v], &b.output[v]);
        }
        for (; v < end; ++v) {
            Real gain = Amplitude ? m_exp.lookup(-(m_steady_attenuation[o][v] + (b.am[v] ? m_am[v] : Real(0))) * Real(0.015625))
                                  : m_steady_gain[o][v];
            b.output[v] = m_wave.lookup(b.waveform[v], steady_phase<o, Pitch>(v)) * gain;
        }
    }
    template<unsigned o, bool Single, bool Steady, bool Modulated> void operator_kernel()
    {
        if (Steady && !Single && m_steady_ready[o]) {
            if (m_steady_pitch[o]) {
                if (m_steady_am[o]) steady_kernel<o, true, true>();
                else steady_kernel<o, true, false>();
            } else {
                if (m_steady_am[o]) steady_kernel<o, false, true>();
                else steady_kernel<o, false, false>();
            }
            return;
        }
        auto& b = m_op[o];
        for (std::size_t n = 0; n < (Single ? 1 : m_live_count); ++n) {
            std::size_t v = m_live[n];
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
                case stage::sustain:
                    add_envelope(b.sustain_step[v]);
                    // Only freeze after the compensated sum has settled. A
                    // nonzero residual must still be applied on later samples.
                    if (b.sustain_step[v] == Real(0) && b.error[v] == Real(0)) st = stage::sustain_hold;
                    break;
                case stage::sustain_hold: break;
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
                if (st != stage::attack) { st = stage::off; m_retire = true; }
            }
            uint64_t step = b.step[v];
            if (Modulated && b.pitch_modulated[v] && m_pm[v] != Real(0))
                step = positive_phase_offset(static_cast<double>(b.frequency[v] * m_pitch_factor[v]) * m_inverse_rate);
            b.phase[v] += step;
            Real attenuation = e * b.shift[v] + b.level[v] + (Modulated && b.am[v] ? m_am[v] : Real(0));
            if (st == stage::off || e >= Real(1023)) { b.output[v] = Real(0); continue; }
            if (o == 3 && m_noise[v]) {
                b.output[v] = m_noise_value[v] * std::max(Real(0), Real(1023) - attenuation) * Real(0.000244140625);
                continue;
            }
            Real offset = Real(0);
            if (o == 0) offset = (m_feedback0[v] + m_feedback1[v]) * m_feedback_scale[v];
            else for (unsigned j = 0; j < o; ++j)
                if (m_routes[o - 1][v] & (1u << j)) offset += m_op[j].output[v] * Real(4);
            // Modulation changes lookup phase, never the oscillator accumulator.
            uint64_t lookup = b.phase[v] + bounded_phase_offset(offset);
            b.output[v] = m_wave.lookup(b.waveform[v], lookup) * m_exp.lookup(-attenuation * Real(0.015625));
        }
    }
    std::array<bool, 4> m_steady_ready{}, m_steady_pitch{}, m_steady_am{};
    std::array<lanes<Real>, 4> m_steady_gain{}, m_steady_attenuation{};
    std::size_t m_steady_first = 0;
    model m_model;
    const wave_table<Real>& m_wave;
    const exponential_table<Real>& m_exp;
    const random_jump_table& m_random_jump;
    uint64_t m_time = 0;
    lanes<bool> m_awake{};
    std::size_t m_awake_count = 0;
    bool m_retire = false;
    std::size_t m_live_count = 0, m_noise_count = 0;
    lanes<std::size_t> m_live{}, m_noise_live{};
    std::array<lanes<std::size_t>, 2> m_lfo_live{};
    std::array<std::size_t, 2> m_lfo_count{};
    double m_inverse_rate = 1.0 / 48000.0;
    envelope_rates<Real> m_rates;
    Real m_rate = Real(48000), m_reference_clock = Real(3579545), m_eg_ticks = Real(0);
    std::array<parameters, Voices> m_parameters{}; // cold control data
    std::array<operator_bank, 4> m_op{};
    std::array<lfo_bank, 2> m_lfo{};
    std::array<lanes<unsigned>, 3> m_routes{};
    lanes<unsigned> m_carriers{};
    lanes<Real> m_left{}, m_right{}, m_feedback0{}, m_feedback1{}, m_feedback_scale{}, m_pm{}, m_am{}, m_pitch_factor{};
    lanes<uint64_t> m_noise_phase{}, m_noise_step{}, m_noise_latch_phase{}, m_noise_latch_step{};
    lanes<uint32_t> m_noise_rng{}, m_noise_whole{};
    lanes<Real> m_noise_value{};
    lanes<bool> m_noise{}, m_noise_latch_every{};
};

template<std::size_t Voices = 8> using fm_engine_f32 = fm_engine<float, Voices>;
template<std::size_t Voices = 8> using fm_engine_f64 = fm_engine<double, Voices>;

} } // namespace ymfm::precision
#endif
