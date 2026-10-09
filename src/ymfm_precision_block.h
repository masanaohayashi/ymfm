// BSD-3-Clause; same license as ymfm_precision.h.
// Included inside fm_engine. Fixed scratch storage; no callback allocations.
// Dexed/msfa's operator-major blocks and gain ramps inspired the scheduling.
// This implementation retains OPZ's eight waveforms and two LFOs.
static constexpr unsigned control_frames = 64;
render_mode m_render_mode = render_mode::faithful;
lanes<unsigned> m_fast_remaining{};
lanes<bool> m_fast_constant{};
std::array<lanes<Real>, 4> m_fast_gain{}, m_fast_dgain{}, m_fast_env{}, m_fast_denv{}, m_fast_end_env{};
std::array<lanes<stage>, 4> m_fast_end_stage{};
std::array<lanes<uint64_t>, 4> m_fast_step{};
std::array<std::array<lanes<Real>,3>,3> m_fast_routes{};
std::array<std::array<Real, control_frames>, 4> m_block_output{};
std::array<Real, control_frames> m_block_noise{};

static uint64_t fast_phase_offset(Real turns)
{
    Real fraction=turns-Real(static_cast<int32_t>(turns));
    return uint64_t(int64_t(static_cast<int32_t>(fraction*Real(1073741824.0))))<<34;
}

static Real integer_power(Real base, unsigned exponent)
{
    Real result = Real(1);
    while (exponent) {
        if (exponent & 1) result *= base;
        base *= base; exponent >>= 1;
    }
    return result;
}

// Block-rate envelope projection. Cross stage boundaries within the interval;
// do not turn a long decay, slow attack or reverb into a 64-frame staircase.
void project_fast_envelope(unsigned o, std::size_t v)
{
    auto& b = m_op[o]; Real e = b.attenuation[v]; stage st = b.state[v];
    unsigned remaining = control_frames;
    while (remaining && st != stage::off && st != stage::sustain_hold) {
        if (st == stage::decay && e >= b.sustain_level[v]) st = stage::sustain;
        if (st == stage::attack) {
            Real base = Real(1) + b.attack_delta[v];
            Real end = (e + Real(1)) * integer_power(base, remaining) - Real(1);
            if (end > Real(0) || base == Real(1)) { e = std::min(Real(1023), end); break; }
            unsigned low = 1, high = remaining;
            while (low < high) {
                unsigned mid = (low + high) / 2;
                if ((e + Real(1)) * integer_power(base, mid) <= Real(1)) high = mid;
                else low = mid + 1;
            }
            e = Real(0); st = stage::decay; remaining -= low; continue;
        }
        Real step = st == stage::decay ? b.decay_step[v]
            : st == stage::sustain ? b.sustain_step[v]
            : st == stage::release ? b.release_step[v] : b.reverb_step[v];
        Real threshold = st == stage::decay ? b.sustain_level[v]
            : st == stage::release && m_model == model::opz ? Real(192) : Real(1023);
        if (step == Real(0)) {
            if (st == stage::sustain) st = stage::sustain_hold;
            break;
        }
        // Bound before converting: tiny rates may need billions of samples.
        Real distance = std::max(Real(0), threshold - e);
        unsigned n = remaining;
        if (distance < step * Real(remaining))
            n = std::max(1u, unsigned(std::ceil(distance / step)));
        e += step * Real(n); remaining -= n;
        if (e >= Real(1023)) { e = Real(1023); st = stage::off; break; }
        if (e >= threshold) st = st == stage::decay ? stage::sustain : stage::reverb;
    }
    m_fast_end_env[o][v] = e;
    m_fast_end_stage[o][v] = st;
    m_fast_env[o][v] = b.attenuation[v];
    m_fast_denv[o][v] = (e - b.attenuation[v]) / Real(control_frames);
}

void fast_control(std::size_t v)
{
    sync_lfos(v); m_pm[v] = m_am[v] = Real(0);
    for (auto& lfo : m_lfo) if (lfo.enabled[v]) {
        Real p = Real(lfo.phase[v] >> 11) * Real(1.0 / 9007199254740992.0);
        Real pm = Real(0), am = Real(0);
        switch (lfo.waveform[v]) {
            case lfo_wave::saw: pm = p < Real(.5) ? Real(2)*p : Real(2)*p-Real(2); am=Real(1)-p; break;
            case lfo_wave::square: pm = p < Real(.5) ? Real(1) : Real(-1); am=p<Real(.5)?Real(1):Real(0); break;
            case lfo_wave::triangle: pm=p<Real(.25)?Real(4)*p:(p<Real(.75)?Real(2)-Real(4)*p:Real(4)*p-Real(4)); am=std::abs(Real(2)*p-Real(1)); break;
            case lfo_wave::noise: pm=lfo.held[v]; am=(pm+Real(1))*Real(.5); break;
            case lfo_wave::sine: pm=m_wave.lookup(0,lfo.phase[v]); am=(pm+Real(1))*Real(.5); break;
        }
        m_pm[v] += pm*lfo.pitch[v]; m_am[v] += am*lfo.amplitude[v];
    }
    Real pitch = m_pm[v] == Real(0) ? Real(1) : m_exp.lookup(m_pm[v] * Real(1.0/1200.0));
    for (unsigned o=0;o<4;++o) {
        auto& b=m_op[o]; project_fast_envelope(o,v);
        Real am=b.am[v]?m_am[v]:Real(0);
        Real start=b.attenuation[v]*b.shift[v]+b.level[v]+am;
        Real end=m_fast_end_env[o][v]*b.shift[v]+b.level[v]+am;
        bool noise=o==3 && m_noise[v];
        auto gain=[&](Real attenuation, stage state, Real envelope) {
            if (state==stage::off || envelope>=Real(1023)) return Real(0);
            return noise ? std::max(Real(0),Real(1023)-attenuation)*Real(.000244140625)
                         : m_exp.lookup(-attenuation*Real(.015625));
        };
        m_fast_gain[o][v]=gain(start,b.state[v],b.attenuation[v]);
        m_fast_dgain[o][v]=(gain(end,m_fast_end_stage[o][v],m_fast_end_env[o][v])-m_fast_gain[o][v])/Real(control_frames);
        m_fast_step[o][v]=b.pitch_modulated[v] && m_pm[v]!=Real(0)
            ? positive_phase_offset(static_cast<double>(b.frequency[v]*pitch)*m_inverse_rate) : b.step[v];
    }
    for(unsigned o=0;o<3;++o)for(unsigned j=0;j<3;++j)
        m_fast_routes[o][j][v]=(m_routes[o][v]&(1u<<j))?Real(4):Real(0);
    for(unsigned o=0;o<4;++o)if(m_op[o].state[v]==stage::off)m_fast_step[o][v]=0;
    // A held envelope without LFOs has no control work until a key/parameter
    // edit invalidates m_fast_remaining. Keep its gain/phase path running,
    // including feedback and noise, but avoid projecting it every 64 samples.
    bool constant=!m_lfo[0].enabled[v] && !m_lfo[1].enabled[v];
    for(unsigned o=0;o<4;++o)
        constant &= m_op[o].state[v]==stage::off || m_op[o].state[v]==stage::sustain_hold;
    m_fast_constant[v]=constant;
    m_fast_remaining[v]=control_frames;
}

Real clock_block_noise(std::size_t v)
{
    uint64_t before=m_noise_phase[v]; m_noise_phase[v]+=m_noise_step[v];
    unsigned ticks=m_noise_whole[v]+unsigned(m_noise_phase[v]<before);
    while(ticks--) {
        uint32_t& state=m_noise_rng[v];
        state=(state<<1)|(((state>>16)^(state>>13)^1)&1);
        uint64_t previous=m_noise_latch_phase[v]; m_noise_latch_phase[v]+=m_noise_latch_step[v];
        if(m_noise_latch_every[v] || m_noise_latch_phase[v]<previous)
            m_noise_value[v]=(state>>17)&1?Real(-1):Real(1);
    }
    return m_noise_value[v];
}

// Linear interpolation across the same preinitialized OPZ table; SIMD spans
// consecutive time samples, including sparse/non-contiguous voice allocation.
void fast_wave_packet(unsigned wave, const uint64_t* phases, const Real* gains, Real* output)
{
    constexpr unsigned width=wave_table<Real>::packet_size;
#if (defined(__aarch64__) || defined(__SSE2__)) && defined(__clang__) && !defined(YMFM_PRECISION_DISABLE_SIMD)
    using vector=typename simd_packet<Real>::type;
    vector a{}, delta{}, fraction{}, gain{};
    const Real* pairs[width];
    for(unsigned n=0;n<width;++n) {
        unsigned index=unsigned(phases[n]>>54);
        pairs[n]=m_wave.fast_pair(wave,index);
        fraction[n]=Real(uint32_t(phases[n]>>30)&0xffffff)*Real(1.0/16777216.0);
        gain[n]=gains[n];
    }
    simd_packet<Real>::gather_pair(pairs,a,delta);
    vector result=(a+delta*fraction)*gain;
    std::memcpy(output,&result,sizeof(result));
#else
    for(unsigned n=0;n<width;++n) {
        unsigned index=unsigned(phases[n]>>54);
        const Real* pair=m_wave.fast_pair(wave,index);Real a=pair[0],d=pair[1];
        Real fraction=Real(uint32_t(phases[n]>>30)&0xffffff)*Real(1.0/16777216.0);
        output[n]=(a+d*fraction)*gains[n];
    }
#endif
}

template<unsigned o, unsigned Route> void fast_operator(std::size_t v, unsigned count)
{
    auto& b=m_op[o]; auto& out=m_block_output[o];
    unsigned wave=b.waveform[v];
    uint64_t phase=b.phase[v], step=m_fast_step[o][v];
    Real gain=m_fast_gain[o][v], delta=m_fast_dgain[o][v];
    if(b.state[v]==stage::off) {
        std::fill_n(out.data(),count,Real(0));b.output[v]=Real(0);
        if(o==0){m_feedback0[v]=count==1?m_feedback1[v]:Real(0);m_feedback1[v]=Real(0);}
        return;
    }
    if(o==3 && m_noise[v]) {
        for(unsigned i=0;i<count;++i) {phase+=step;gain+=delta;out[i]=m_block_noise[i]*gain;}
    } else if(o==0 && m_feedback_scale[v]!=Real(0)) {
        Real fb0=m_feedback0[v],fb1=m_feedback1[v],scale=m_feedback_scale[v];
        for(unsigned i=0;i<count;++i) {
            phase+=step;gain+=delta;
            Real offset=(fb0+fb1)*scale;
            uint64_t lookup=phase+(fast_phase_offset(offset));
            unsigned index=unsigned(lookup>>54);
            const Real* pair=m_wave.fast_pair(wave,index);Real a=pair[0],d=pair[1];
            Real fraction=Real(uint32_t(lookup>>30)&0xffffff)*Real(1.0/16777216.0);
            Real value=(a+d*fraction)*gain;
            out[i]=value;fb0=fb1;fb1=value;
        }
        m_feedback0[v]=fb0;m_feedback1[v]=fb1;
    } else {
        constexpr unsigned width=wave_table<Real>::packet_size;
        unsigned i=0;
        for(;i+width<=count;i+=width) {
            uint64_t phases[width];Real gains[width];
            for(unsigned n=0;n<width;++n) {
                phase+=step;gain+=delta;Real offset=Real(0);
                if(Route&1)offset+=m_block_output[0][i+n]*Real(4);
                if(Route&2)offset+=m_block_output[1][i+n]*Real(4);
                if(Route&4)offset+=m_block_output[2][i+n]*Real(4);
                phases[n]=phase+(fast_phase_offset(offset));
                gains[n]=gain;
            }
            fast_wave_packet(wave,phases,gains,out.data()+i);
        }
        for(;i<count;++i) {
            phase+=step;gain+=delta;Real offset=Real(0);
            if(Route&1)offset+=m_block_output[0][i]*Real(4);
            if(Route&2)offset+=m_block_output[1][i]*Real(4);
            if(Route&4)offset+=m_block_output[2][i]*Real(4);
            uint64_t lookup=phase+(fast_phase_offset(offset));
            unsigned index=unsigned(lookup>>54);
            const Real* pair=m_wave.fast_pair(wave,index);Real a=pair[0],d=pair[1];
            Real fraction=Real(uint32_t(lookup>>30)&0xffffff)*Real(1.0/16777216.0);
            out[i]=(a+d*fraction)*gain;
        }
        if(o==0) {m_feedback0[v]=count==1?m_feedback1[v]:out[count-2];m_feedback1[v]=out[count-1];}
    }
    b.phase[v]=phase;m_fast_gain[o][v]=gain;b.output[v]=out[count-1];
}

template<unsigned o> void dispatch_fast_operator(std::size_t v,unsigned count)
{
    if(o==0) {fast_operator<o,0>(v,count);return;}
    switch(m_routes[o-1][v]) {
        case 0: fast_operator<o,0>(v,count);break;
        case 1: fast_operator<o,1>(v,count);break;
        case 2: fast_operator<o,2>(v,count);break;
        case 3: fast_operator<o,3>(v,count);break;
        case 4: fast_operator<o,4>(v,count);break;
        case 5: fast_operator<o,5>(v,count);break;
        case 6: fast_operator<o,6>(v,count);break;
        default: fast_operator<o,7>(v,count);break;
    }
}

void mix_block(std::size_t v, Real* left, Real* right, unsigned count)
{
    unsigned carriers=m_carriers[v];
    for(unsigned i=0;i<count;++i) {
        Real sum=m_block_output[3][i];
        for(unsigned o=0;o<3;++o) if(carriers&(1u<<o)) sum+=m_block_output[o][i];
        left[i]+=sum*m_left[v];right[i]+=sum*m_right[v];
    }
}

// SIMD across voices also parallelizes feedback (which is serial in time).
// Time packets remain useful for a singleton; polyphonic feedback uses this
// path so high feedback does not throw away the SIMD speed-up.
template<unsigned o> void fast_voice_operator(std::size_t first=0)
{
    constexpr unsigned width=wave_table<Real>::packet_size;
    auto& b=m_op[o];
    for(std::size_t at=first;at<m_live_count;at+=width) {
        unsigned count=unsigned(std::min<std::size_t>(width,m_live_count-at));
        Real values[width]{};
        unsigned waves[width]{};
#if (defined(__aarch64__) || defined(__SSE2__)) && defined(__clang__) && !defined(YMFM_PRECISION_DISABLE_SIMD)
        using vector=typename simd_packet<Real>::type;
        vector gain{},delta{},offset{},scale{},input0{},input1{},input2{};
        for(unsigned n=0;n<count;++n) {
            auto v=m_live[at+n];gain[n]=m_fast_gain[o][v];delta[n]=m_fast_dgain[o][v];
            if(o==0){input0[n]=m_feedback0[v];input1[n]=m_feedback1[v];scale[n]=m_feedback_scale[v];}
            else {unsigned route=m_routes[o-1][v];
                input0[n]=route&1?m_op[0].output[v]:Real(0);
                if(o>1)input1[n]=route&2?m_op[1].output[v]:Real(0);
                if(o>2)input2[n]=route&4?m_op[2].output[v]:Real(0);
            }
        }
        gain+=delta;
        if(o==0)offset=(input0+input1)*scale;
        else {offset=input0*Real(4);if(o>1)offset+=input1*Real(4);if(o>2)offset+=input2*Real(4);}
        using integers=int32_t __attribute__((vector_size(width*sizeof(int32_t))));
        using unsigneds=uint32_t __attribute__((vector_size(width*sizeof(uint32_t))));
        vector turns=offset-__builtin_convertvector(__builtin_convertvector(offset,integers),vector);
        integers modulation=__builtin_convertvector(turns*Real(1073741824.0),integers);
        unsigneds high{},low{};
        for(unsigned n=0;n<count;++n) {
            auto v=m_live[at+n];m_fast_gain[o][v]=gain[n];
            if(b.state[v]!=stage::off)b.phase[v]+=m_fast_step[o][v];
            high[n]=uint32_t(b.phase[v]>>32);
            low[n]=uint32_t(b.phase[v]>>30)&3u;
            waves[n]=b.waveform[v];
        }
        // The same exact high-word phase arithmetic as the contiguous path.
        high+=__builtin_convertvector(modulation,unsigneds)<<2;
        unsigneds indices=high>>22;
        unsigneds fractions=((high&0x3fffffu)<<2)|low;
        vector fraction=__builtin_convertvector(fractions,vector)*Real(1.0/16777216.0);
        vector a{},d{};
        const Real* pairs[width];
        for(unsigned n=0;n<width;++n)pairs[n]=m_wave.fast_pair(waves[n],indices[n]);
        simd_packet<Real>::gather_pair(pairs,a,d);
        vector result=(a+d*fraction)*gain;
        std::memcpy(values,&result,sizeof(result));
#else
        for(unsigned n=0;n<count;++n) {
            auto v=m_live[at+n];Real gain=m_fast_gain[o][v]+m_fast_dgain[o][v];m_fast_gain[o][v]=gain;
            if(b.state[v]!=stage::off)b.phase[v]+=m_fast_step[o][v];
            Real offset=Real(0);
            if(o==0)offset=(m_feedback0[v]+m_feedback1[v])*m_feedback_scale[v];
            else for(unsigned j=0;j<o;++j)if(m_routes[o-1][v]&(1u<<j))offset+=m_op[j].output[v]*Real(4);
            uint64_t lookup=b.phase[v]+(fast_phase_offset(offset));
            unsigned index=unsigned(lookup>>54);
            const Real* pair=m_wave.fast_pair(b.waveform[v],index);Real a=pair[0],d=pair[1];
            Real fraction=Real(uint32_t(lookup>>30)&0xffffff)*Real(1.0/16777216.0);
            values[n]=(a+d*fraction)*gain;
        }
#endif
        for(unsigned n=0;n<count;++n) {
            auto v=m_live[at+n];Real value=b.state[v]==stage::off || m_fast_gain[o][v]==Real(0)?Real(0):values[n];
            if(o==3 && m_noise[v])value=clock_block_noise(v)*m_fast_gain[o][v];
            b.output[v]=value;
            if(o==0){m_feedback0[v]=m_feedback1[v];m_feedback1[v]=value;}
        }
    }
}

#if (defined(__aarch64__) || defined(__SSE2__)) && defined(__clang__) && !defined(YMFM_PRECISION_DISABLE_SIMD)
template<unsigned o, bool Constant = false, unsigned common_routes=8> void fast_contiguous_operator(std::size_t packet_count)
{
    constexpr unsigned width=wave_table<Real>::packet_size;
    using vector=typename simd_packet<Real>::type;
    using integers=int32_t __attribute__((vector_size(width*sizeof(int32_t))));
    using phases2=uint64_t __attribute__((vector_size(16)));
    auto& b=m_op[o];
    for(std::size_t v=m_live[0],end=v+packet_count;v<end;v+=width) {
        vector gain=simd_packet<Real>::load(&m_fast_gain[o][v]);
        if constexpr(!Constant) {
            gain+=simd_packet<Real>::load(&m_fast_dgain[o][v]);
            std::memcpy(&m_fast_gain[o][v],&gain,sizeof(gain));
        }
        vector offset{};
        if(o==0)offset=(simd_packet<Real>::load(&m_feedback0[v])+simd_packet<Real>::load(&m_feedback1[v]))*simd_packet<Real>::load(&m_feedback_scale[v]);
        else if constexpr(common_routes<8) {
            if(common_routes&1)offset+=simd_packet<Real>::load(&m_op[0].output[v])*Real(4);
            if(o>1 && (common_routes&2))offset+=simd_packet<Real>::load(&m_op[1].output[v])*Real(4);
            if(o>2 && (common_routes&4))offset+=simd_packet<Real>::load(&m_op[2].output[v])*Real(4);
        } else {
            offset=simd_packet<Real>::load(&m_op[0].output[v])*simd_packet<Real>::load(&m_fast_routes[o-1][0][v]);
            if(o>1)offset+=simd_packet<Real>::load(&m_op[1].output[v])*simd_packet<Real>::load(&m_fast_routes[o-1][1][v]);
            if(o>2)offset+=simd_packet<Real>::load(&m_op[2].output[v])*simd_packet<Real>::load(&m_fast_routes[o-1][2][v]);
        }
        vector fraction=offset-__builtin_convertvector(__builtin_convertvector(offset,integers),vector);
        integers modulation=__builtin_convertvector(fraction*Real(1073741824.0),integers);
        uint64_t phases[width];
        for(unsigned n=0;n<width;n+=2) {
            phases2 phase,step;std::memcpy(&phase,&b.phase[v+n],sizeof(phase));std::memcpy(&step,&m_fast_step[o][v+n],sizeof(step));
            phase+=step;std::memcpy(&b.phase[v+n],&phase,sizeof(phase));std::memcpy(phases+n,&phase,sizeof(phase));
        }
        vector a{},d{},t{};
        using unsigneds=uint32_t __attribute__((vector_size(width*sizeof(uint32_t))));
        unsigneds high{},low{};
        for(unsigned n=0;n<width;++n) {
            high[n]=uint32_t(phases[n]>>32);
            low[n]=uint32_t(phases[n]>>30)&3u;
        }
        // Modulation is a multiple of 2^34, so it cannot affect the low
        // 32 phase bits. Unsigned lane arithmetic preserves exact wrapping.
        high += __builtin_convertvector(modulation,unsigneds)<<2;
        unsigneds indices=high>>22;
        unsigneds fractions=((high&0x3fffffu)<<2)|low;
        t=__builtin_convertvector(fractions,vector)*Real(1.0/16777216.0);
        const Real* pairs[width];
        for(unsigned n=0;n<width;++n)
            pairs[n]=m_wave.fast_pair(b.waveform[v+n],indices[n]);
        simd_packet<Real>::gather_pair(pairs,a,d);
        vector result=(a+d*t)*gain;
        // Gain zero always emits canonical silence, even on a negative wave.
        result= gain==Real(0) ? vector{} : result;
        if(o==3 && m_noise_count)
            for(unsigned n=0;n<width;++n)
                if(m_noise[v+n])result[n]=clock_block_noise(v+n)*gain[n];
        std::memcpy(&b.output[v],&result,sizeof(result));
        if(o==0) {
            vector previous=simd_packet<Real>::load(&m_feedback1[v]);
            std::memcpy(&m_feedback0[v],&previous,sizeof(previous));std::memcpy(&m_feedback1[v],&result,sizeof(result));
        }
    }
}
template<unsigned o, bool Constant = false>
void dispatch_contiguous_operator(unsigned route,std::size_t packet_count)
{
    switch(route) {
        case 0: fast_contiguous_operator<o,Constant,0>(packet_count);break;
        case 1: fast_contiguous_operator<o,Constant,1>(packet_count);break;
        case 2: fast_contiguous_operator<o,Constant,2>(packet_count);break;
        case 3: fast_contiguous_operator<o,Constant,3>(packet_count);break;
        case 4: fast_contiguous_operator<o,Constant,4>(packet_count);break;
        case 5: fast_contiguous_operator<o,Constant,5>(packet_count);break;
        case 6: fast_contiguous_operator<o,Constant,6>(packet_count);break;
        case 7: fast_contiguous_operator<o,Constant,7>(packet_count);break;
        default: fast_contiguous_operator<o,Constant>(packet_count);break;
    }
}

#endif

void fast_voice_samples(Real* left,Real* right,unsigned count)
{
#if (defined(__aarch64__) || defined(__SSE2__)) && defined(__clang__) && !defined(YMFM_PRECISION_DISABLE_SIMD)
    constexpr unsigned width=wave_table<Real>::packet_size;
    // Keep full contiguous packets fast even with a partial last packet.
    // Live voices are ordered; stop at the first hole without touching it.
    std::size_t prefix=1;
    while(prefix<m_live_count && m_live[prefix]==m_live[0]+prefix)++prefix;
    const std::size_t packet_count=prefix-prefix%width;
    const bool contiguous=packet_count!=0;
    bool constant=contiguous;
    unsigned common_routes[3]{8,8,8},common_carriers=8;
    if(contiguous) {
        for(unsigned o=0;o<3;++o)common_routes[o]=m_routes[o][m_live[0]];
        common_carriers=m_carriers[m_live[0]];
        for(std::size_t v=m_live[0],end=v+packet_count;v<end;++v) {
            constant &= m_fast_constant[v];
            for(unsigned o=0;o<3;++o)if(m_routes[o][v]!=common_routes[o])common_routes[o]=8;
            if(m_carriers[v]!=common_carriers)common_carriers=8;
        }
    }
#endif
    for(unsigned i=0;i<count;++i) {
#if (defined(__aarch64__) || defined(__SSE2__)) && defined(__clang__) && !defined(YMFM_PRECISION_DISABLE_SIMD)
        if(contiguous) {
            if(constant) {
                fast_contiguous_operator<0,true>(packet_count);dispatch_contiguous_operator<1,true>(common_routes[0],packet_count);
                dispatch_contiguous_operator<2,true>(common_routes[1],packet_count);dispatch_contiguous_operator<3,true>(common_routes[2],packet_count);
            } else {
                fast_contiguous_operator<0>(packet_count);dispatch_contiguous_operator<1>(common_routes[0],packet_count);dispatch_contiguous_operator<2>(common_routes[1],packet_count);dispatch_contiguous_operator<3>(common_routes[2],packet_count);
            }
        }
        const std::size_t remaining=packet_count;
#else
        const std::size_t remaining=0;
#endif
        if(remaining<m_live_count) {
            fast_voice_operator<0>(remaining);fast_voice_operator<1>(remaining);
            fast_voice_operator<2>(remaining);fast_voice_operator<3>(remaining);
        }
        Real l=Real(0),r=Real(0);
#if (defined(__aarch64__) || defined(__SSE2__)) && defined(__clang__) && !defined(YMFM_PRECISION_DISABLE_SIMD)
        if(contiguous) {
            using vector=typename simd_packet<Real>::type;
            using mask_element=typename std::conditional<sizeof(Real)==4,int32_t,int64_t>::type;
            using masks=mask_element __attribute__((vector_size(16)));
            for(std::size_t v=m_live[0],end=v+packet_count;v<end;v+=width) {
                vector sum=simd_packet<Real>::load(&m_op[3].output[v]);
                if(common_carriers<8) {
                    for(unsigned o=0;o<3;++o)if(common_carriers&(1u<<o))sum+=simd_packet<Real>::load(&m_op[o].output[v]);
                } else {
                    masks carriers{};
                    for(unsigned lane=0;lane<width;++lane)carriers[lane]=m_carriers[v+lane];
                    for(unsigned o=0;o<3;++o)
                        sum=(carriers&mask_element(1u<<o))!=0?sum+simd_packet<Real>::load(&m_op[o].output[v]):sum;
                }
                vector vl=sum*simd_packet<Real>::load(&m_left[v]);
                vector vr=sum*simd_packet<Real>::load(&m_right[v]);
                // Keep the original sequential voice sum, including its
                // floating-point rounding, after vectorizing each voice.
                for(unsigned lane=0;lane<width;++lane){l+=vl[lane];r+=vr[lane];}
            }
        }
#endif
        for(std::size_t n=remaining;n<m_live_count;++n) {
            auto v=m_live[n];Real sum=m_op[3].output[v];
            for(unsigned o=0;o<3;++o)if(m_carriers[v]&(1u<<o))sum+=m_op[o].output[v];
            l+=sum*m_left[v];r+=sum*m_right[v];
        }
        left[i]=l;right[i]=r;
    }
}

void render_fast(Real* left, Real* right, std::size_t frames)
{
    std::fill_n(left,frames,Real(0));std::fill_n(right,frames,Real(0));
    std::size_t at=0;
    while(at<frames && m_live_count) {
        unsigned count=unsigned(std::min<std::size_t>(control_frames,frames-at));
        for(std::size_t n=0;n<m_live_count;++n) {
            std::size_t v=m_live[n];
            if(!m_fast_remaining[v]) fast_control(v);
            if(!m_fast_constant[v])count=std::min(count,m_fast_remaining[v]);
        }
        bool voice_packets=m_live_count>=wave_table<Real>::packet_size;
        // Short runs without feedback are faster as time packets. Feedback
        // uses voice packets so its serial dependency does not defeat SIMD.
        if(m_live_count<=8) {
            bool feedback=false;
            for(std::size_t n=0;n<m_live_count;++n)feedback|=m_feedback_scale[m_live[n]]!=Real(0);
            if(!feedback)voice_packets=false;
        }
        if(voice_packets)fast_voice_samples(left+at,right+at,count);
        for(std::size_t n=0;n<m_live_count;++n) {
            std::size_t v=m_live[n];
            if(!voice_packets) {
                if(m_noise[v]) for(unsigned i=0;i<count;++i) m_block_noise[i]=clock_block_noise(v);
                dispatch_fast_operator<0>(v,count);dispatch_fast_operator<1>(v,count);
                dispatch_fast_operator<2>(v,count);dispatch_fast_operator<3>(v,count);
                mix_block(v,left+at,right+at,count);
            }
            if(m_fast_constant[v]) {if(!active(v))m_retire=true;continue;}
            m_fast_remaining[v]-=count;

            for(unsigned o=0;o<4;++o) {
                auto& b=m_op[o];
                // Compute from interval position, so caller block partitions
                // cannot change the next control endpoint.
                b.attenuation[v]=m_fast_env[o][v]+m_fast_denv[o][v]*Real(control_frames-m_fast_remaining[v]);b.error[v]=Real(0);
                if(!m_fast_remaining[v]) {
                    b.attenuation[v]=m_fast_end_env[o][v];b.error[v]=Real(0);
                    b.state[v]=m_fast_end_stage[o][v];
                }

            }
            if(!active(v))m_retire=true;
        }
        at+=count;m_time+=count;
        if(m_retire)retire_finished(m_time);
    }
    m_time+=frames-at;
    // LFOs are analytically synchronized on the next control/key/edit boundary.
}

// SIMD across independent voices for uniform decay, including active LFOs.
// Every lane keeps the original compensated EG sum and phase arithmetic.
#if (defined(__aarch64__) || defined(__SSE2__)) && defined(__clang__) && !defined(YMFM_PRECISION_DISABLE_SIMD)
template<unsigned o, bool Modulated> void decay_packet_kernel()
{
    constexpr unsigned width=wave_table<Real>::packet_size;
    using vector=typename simd_packet<Real>::type;
    auto& b=m_op[o];
    // Channel 7 noise requires envelope-domain output, handled by the oracle.
    if(o==3 && m_noise_count) {operator_kernel<o,false,false,Modulated,true>();return;}
    std::size_t n=0;
    for(;n+width<=m_live_count;n+=width) {
        vector envelope{},error{},step{},shift{},level{},am{};
        for(unsigned lane=0;lane<width;++lane) {
            auto v=m_live[n+lane];envelope[lane]=b.attenuation[v];error[lane]=b.error[v];
            step[lane]=b.decay_step[v];shift[lane]=b.shift[v];level[lane]=b.level[v];
            am[lane]=Modulated && b.am[v]?m_am[v]:Real(0);
        }
        vector corrected=step-error,next=envelope+corrected;
        vector residual=(next-envelope)-corrected;
        vector exponent=-(next*shift+level+am)*Real(.015625);
        Real exponents[width],gains[width],outputs[width];
        std::memcpy(exponents,&exponent,sizeof(exponent));
        uint64_t phases[width];unsigned waves[width];
        bool silent[width]{};
        for(unsigned lane=0;lane<width;++lane) {
            auto v=m_live[n+lane];Real e=next[lane];
            b.attenuation[v]=e;b.error[v]=residual[lane];
            if(e>=b.sustain_level[v]) {b.state[v]=stage::sustain;m_decay_ready[o]=false;}
            if(e>=Real(1023)) {b.attenuation[v]=Real(1023);b.state[v]=stage::off;m_retire=true;m_decay_ready[o]=false;silent[lane]=true;exponents[lane]=Real(0);}
            uint64_t increment=b.step[v];
            if(Modulated && b.pitch_modulated[v] && m_pm[v]!=Real(0))
                increment=positive_phase_offset(static_cast<double>(b.frequency[v]*m_pitch_factor[v])*m_inverse_rate);
            b.phase[v]+=increment;Real offset=Real(0);
            if(o==0)offset=(m_feedback0[v]+m_feedback1[v])*m_feedback_scale[v];
            else for(unsigned j=0;j<o;++j)if(m_routes[o-1][v]&(1u<<j))offset+=m_op[j].output[v]*Real(4);
            phases[lane]=b.phase[v]+bounded_phase_offset(offset);waves[lane]=b.waveform[v];
        }
        m_exp.lookup_packet(exponents,gains);m_wave.lookup_packet(waves,phases,gains,outputs);
        for(unsigned lane=0;lane<width;++lane)b.output[m_live[n+lane]]=silent[lane]?Real(0):outputs[lane];
    }
    // Tail lanes use the same kernel with a temporary, bounded sparse list.
    if(n<m_live_count) {
        auto count=m_live_count;std::array<std::size_t,width> saved{};
        unsigned tail=unsigned(count-n);
        for(unsigned lane=0;lane<tail;++lane){saved[lane]=m_live[lane];m_live[lane]=m_live[n+lane];}
        m_live_count=tail;operator_kernel<o,false,false,Modulated,true>();m_live_count=count;
        for(unsigned lane=0;lane<tail;++lane)m_live[lane]=saved[lane];
    }
}
#endif
