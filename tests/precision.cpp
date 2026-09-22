#include "ymfm_precision_import.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <new>

static unsigned failures = 0;
static bool watch_allocation = false;
static unsigned allocations = 0;
void* operator new(std::size_t size)
{
    if (watch_allocation) ++allocations;
    if (void* p=std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete[](void* p) noexcept { ::operator delete(p); }

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)
using namespace ymfm::precision;

// Independent analytic oracle. No table, integer attenuation or phase lookup.
long double analytic(unsigned w, long double phase)
{
    long double p = phase - std::floor(phase);
    if (w >= 2 && p >= 0.5L) return 0;
    long double s = std::sin((w < 4 ? 2 : 4) * std::acos(-1.0L) * p);
    if (w >= 6) s = std::abs(s);
    return (w & 1) ? s * std::abs(s) : s;
}

template<class R> void wave_accuracy()
{
    const auto& table = wave_table<R>::instance();
    long double maximum = 0;
    uint64_t rng = 9;
    for (unsigned w = 0; w < 8; ++w) {
        for (unsigned i = 0; i < 150000; ++i) {
            rng = rng * UINT64_C(6364136223846793005) + 1;
            long double phase = std::ldexp(static_cast<long double>(rng), -64);
            long double error = std::abs(static_cast<long double>(table.lookup(w, rng)) - analytic(w, phase));
            maximum = std::max(maximum, error);
        }
        // Explicitly include both sides of each OPZ corner and the wrap point.
        for (uint64_t boundary : {uint64_t(0), uint64_t(1)<<62, uint64_t(1)<<63, uint64_t(3)<<62})
            for (int offset = -3; offset <= 3; ++offset) {
                uint64_t phase = boundary + uint64_t(offset);
                long double x = std::ldexp(static_cast<long double>(phase), -64);
                CHECK(std::abs(static_cast<long double>(table.lookup(w, phase)) - analytic(w, x)) < 2e-7L);
            }
    }
    std::printf("wave %s max absolute error %.12Lg\n", sizeof(R) == 4 ? "f32" : "f64", maximum);
    CHECK(maximum < (sizeof(R) == 4 ? 1.5e-7L : 3e-12L));
}

template<class R> void wave_packets()
{
    const auto& table=wave_table<R>::instance();
    constexpr unsigned width=wave_table<R>::packet_size;
    unsigned waves[width];uint64_t phases[width];R gains[width],out[width];
    uint64_t rng=9;
    for(unsigned trial=0;trial<100000;++trial){
        for(unsigned n=0;n<width;++n){
            rng=rng*UINT64_C(6364136223846793005)+1;
            phases[n]=rng;waves[n]=(trial+n)%8;gains[n]=R(0.123)*(R(n)+R(1));
        }
        table.lookup_packet(waves,phases,gains,out);
        for(unsigned n=0;n<width;++n)CHECK(out[n]==table.lookup(waves[n],phases[n])*gains[n]);
    }
}

template<class R> void exponential_packets()
{
    const auto& table=exponential_table<R>::instance();
    constexpr unsigned width=wave_table<R>::packet_size;
    R x[width],out[width];uint64_t rng=9;
    for(unsigned trial=0;trial<100000;++trial){
        for(unsigned n=0;n<width;++n){
            rng=rng*UINT64_C(6364136223846793005)+1;
            x[n]=trial==0 ? (n%2 ? R(-64) : R(16)) : R(rng>>32)*R(80.0/4294967296.0)-R(64);
        }
        table.lookup_packet(x,out);
        for(unsigned n=0;n<width;++n)CHECK(out[n]==table.lookup(x[n]));
    }
}

template<class R> void phase_boundaries()
{
    CHECK(phase_offset(R(0)) == 0);
    CHECK(phase_offset(R(1)) == 0);
    CHECK(phase_offset(R(-1)) == 0);
    CHECK(phase_offset(R(0.5)) == (uint64_t(1) << 63));
    CHECK(phase_offset(R(-0.25)) == (uint64_t(3) << 62));
    CHECK(phase_offset(std::ldexp(R(1), -64)) == 1);
    CHECK(phase_offset(-std::ldexp(R(1), -64)) == UINT64_MAX);
    CHECK(phase_offset(std::numeric_limits<R>::infinity()) == 0);
    CHECK(phase_offset(std::numeric_limits<R>::quiet_NaN()) == 0);
    CHECK(phase_offset(std::nextafter(R(1), R(0))) != 0);
    CHECK(phase_offset(R(-123.25)) == (uint64_t(3) << 62));
    fm_engine<R,1> e(model::opz);
    voice_parameters<R> p;
    p.operators[3].fixed = true;
    p.operators[3].fixed_hz = R(0.001);
    CHECK(e.set_voice(0,p)); CHECK(e.key_on(0,8));
    uint64_t step = phase_step(R(0.001), R(48000));
    R l[1],r[1];
    for (unsigned i=1; i<=10000; ++i) {
        e.render(l,r,1);
        CHECK(e.phase(0,3) == step * i);
    }
}

// Handwritten 8 FM graphs against a continuous sine oracle, independent of
// the engine's route masks. Covers operator PM depth, TL and output scaling.
template<class R> void algorithms()
{
    constexpr unsigned count = 3072;
    std::vector<R> l(count), r(count);
    long double worst = 0;
    for (unsigned algorithm = 0; algorithm < 8; ++algorithm) {
        fm_engine<R,1> e(model::opz);
        voice_parameters<R> p; p.algorithm = algorithm;
        for (unsigned o=0; o<4; ++o) {
            p.operators[o].ratio = R(o+1);
            p.operators[o].total_level = R(12+o*4);
        }
        CHECK(e.set_voice(0,p)); e.key_on(0); e.render(l.data(),r.data(),count);
        for (unsigned n=0; n<count; ++n) {
            auto op = [&](unsigned o, long double modulation) {
                long double phase = static_cast<long double>(n+1) * 440 * (o+1) / 48000;
                long double amplitude = std::exp2(-static_cast<long double>(12+o*4)/8);
                return analytic(0, phase + 4*modulation) * amplitude;
            };
            long double a=op(0,0), b=0,c=0,expected=0;
            switch (algorithm) {
                case 0: b=op(1,a); c=op(2,b); expected=op(3,c); break;
                case 1: b=op(1,0); c=op(2,a+b); expected=op(3,c); break;
                case 2: b=op(1,0); c=op(2,b); expected=op(3,a+c); break;
                case 3: b=op(1,a); c=op(2,0); expected=op(3,b+c); break;
                case 4: b=op(1,a); c=op(2,0); expected=b+op(3,c); break;
                case 5: expected=op(1,a)+op(2,a)+op(3,a); break;
                case 6: expected=op(1,a)+op(2,0)+op(3,0); break;
                case 7: expected=a+op(1,0)+op(2,0)+op(3,0); break;
            }
            worst = std::max(worst, std::abs(static_cast<long double>(l[n])-expected));
            CHECK(l[n] == r[n]);
        }
    }
    std::printf("algorithm %s oracle max error %.12Lg\n",sizeof(R)==4?"f32":"f64",worst);
    CHECK(worst < (sizeof(R)==4 ? 2e-5L : 2e-10L));
}

template<class R> void feedback_and_tails()
{
    fm_engine<R,1> e;
    voice_parameters<R> p;p.algorithm=7;p.feedback=legacy<R>::feedback3(R(1));
    p.operators[0].total_level=R(24);p.frequency=R(250);
    e.set_voice(0,p);e.key_on(0,1);
    long double old=0,recent=0,max_error=0;
    for(unsigned n=0;n<4096;++n) {
        R l,r;e.render(&l,&r,1);
        long double expected=analytic(0,static_cast<long double>(n+1)*250/48000+(old+recent)/64)*0.125L;
        max_error=std::max(max_error,std::abs(static_cast<long double>(l)-expected));
        old=recent;recent=expected;
    }
    CHECK(max_error<(sizeof(R)==4?1e-7L:1e-12L));
    // Fully released operators stay exactly silent even with nonzero feedback.
    e.key_off(0);
    p.operators[0].release=R(127);e.set_voice(0,p);
    R l,r;for(unsigned n=0;n<10000;++n)e.render(&l,&r,1);
    CHECK(l==R(0));CHECK(!e.active(0));
}

template<class R> void envelopes()
{
    R l[1], r[1];
    fm_engine<R,1> e;
    voice_parameters<R> p;
    p.operators[3].attack = R(0);
    e.set_voice(0,p); e.key_on(0,8);
    for(unsigned i=0;i<100;++i) { e.render(l,r,1); CHECK(l[0]==R(0)); }
    CHECK(e.active(0)); // a zero attack holds and may later be edited
    p.operators[3].attack=R(127);
    e.set_voice(0,p); e.key_on(0,8);
    e.render(l,r,1); CHECK(e.attenuation(0,3)==R(0));

    // Rate 10: six increments per eight events, one event every 512 EG ticks.
    p.operators[3].decay=R(10)*R(127)/R(62);
    p.operators[3].sustain_level=R(127);
    long double decay_expected = 3579545.0L / 192 * 0.75L / 512;
    for(R sr : {R(44100),R(48000),R(96000)}) {
        e.prepare(sr); e.set_voice(0,p); e.key_on(0,8);
        for(unsigned n=0;n<unsigned(sr);++n) e.render(l,r,1);
        CHECK(std::abs(static_cast<long double>(e.attenuation(0,3))-decay_expected) < (sizeof(R)==4 ? 0.0001L : 2e-10L));
    }
    // Rate 8 attack: half the events multiply (attenuation+1) by 15/16.
    e.prepare(R(48000)); p.operators[3].attack=R(8)*R(127)/R(62);
    e.set_voice(0,p); e.key_on(0,8);
    for(unsigned n=0;n<48000;++n) e.render(l,r,1);
    long double attack_expected=1024*std::exp(std::log(15.0L/16)*0.5L/512*3579545.0L/192)-1;
    CHECK(std::abs(static_cast<long double>(e.attenuation(0,3))-attack_expected) < (sizeof(R)==4?0.002L:2e-9L));

    // Sub-ULP decay increments must accumulate even in binary32.
    e.prepare(R(48000)); p.operators[3].attack=R(127);
    p.operators[3].decay=R(127); p.operators[3].sustain_level=R(64);
    p.operators[3].sustain_rate=R(2)*R(127)/R(62);
    e.set_voice(0,p); e.key_on(0,8);
    for(unsigned n=0;n<1000;++n) e.render(l,r,1);
    R before=e.attenuation(0,3);
    for(unsigned n=0;n<10000;++n) e.render(l,r,1);
    CHECK(e.attenuation(0,3)>before+R(0.5));
    p.operators[3].release=R(127); e.set_voice(0,p); e.key_off(0,8);
    for(unsigned n=0;n<2000;++n) e.render(l,r,1);
    CHECK(!e.active(0)); CHECK(l[0]==R(0));

    // An intermediate 7-bit rate must survive the whole DSP path.
    fm_engine<R,1> a,b;
    p.operators[3].attack=R(127); p.operators[3].decay=R(40.25);
    p.operators[3].sustain_level=R(127);
    a.set_voice(0,p); a.key_on(0,8);
    p.operators[3].decay=R(40.5); b.set_voice(0,p); b.key_on(0,8);
    for(unsigned n=0;n<2000;++n){a.render(l,r,1);b.render(l,r,1);}
    CHECK(b.attenuation(0,3)>a.attenuation(0,3));
}

template<class R> void block_and_polyphony()
{
    constexpr unsigned count=2048;
    fm_engine<R,128> big(model::opz), split(model::opz);
    for(unsigned v=0;v<128;++v) {
        voice_parameters<R> p;
        p.algorithm=v%8; p.feedback=R(v); p.frequency=R(80+v*3);
        p.gain_left=R(0.005); p.gain_right=R(0.003);
        p.noise=v==127;
        for(unsigned o=0;o<4;++o) {
            p.operators[o].waveform=(v+o)%8;
            p.operators[o].attack=R(80.25+o);
            p.operators[o].am_enabled=true;
            p.operators[o].total_level=R(o*10);
        }
        p.lfos[0].frequency=R(5.25);p.lfos[0].pitch_cents=R(17.5);p.lfos[0].amplitude=R(12.5);
        p.lfos[1].frequency=R(3.125);p.lfos[1].waveform=lfo_wave::sine;p.lfos[1].pitch_cents=R(-8.5);
        CHECK(big.set_voice(v,p));CHECK(split.set_voice(v,p));big.key_on(v);split.key_on(v);
    }
    std::vector<R> a(count),b(count),c(count),d(count);
    allocations=0; watch_allocation=true;
    big.render(a.data(),b.data(),count);
    watch_allocation=false; CHECK(allocations==0);
    unsigned at=0;
    while(at<count){ unsigned n=std::min(1+at%79,count-at);split.render(c.data()+at,d.data()+at,n);at+=n; }
    CHECK(a==c);CHECK(b==d);
    bool sound=false;
    for(auto x:a){CHECK(std::isfinite(x));sound|=x!=R(0);}
    CHECK(sound);
    CHECK(big.active(127)); CHECK(!big.active(128));
    for(unsigned v=0;v<128;++v) big.key_off(v);

    // Mix does not clip at +/-1, and voice 127 is actually rendered.
    fm_engine<R,128> sum;
    voice_parameters<R> p; p.frequency=R(12000);
    for(unsigned v=0;v<128;++v){sum.set_voice(v,p);sum.key_on(v,8);}
    R l,r;sum.render(&l,&r,1);CHECK(std::abs(l-R(128))<R(0.0001));
}

template<class R> void validation_and_opz()
{
    fm_engine<R,1> e(model::opz); voice_parameters<R> p;
    CHECK(!e.prepare(R(0)));CHECK(!e.prepare(std::numeric_limits<R>::infinity()));
    CHECK(!e.set_voice(1,p));CHECK(!e.key_on(1));CHECK(!e.key_off(0,16));
    p.operators[2].ratio=std::numeric_limits<R>::quiet_NaN();CHECK(!e.set_voice(0,p));
    CHECK(e.get_voice(0)->operators[2].ratio==R(1));
    CHECK(e.render(nullptr,nullptr,0));CHECK(!e.render(nullptr,nullptr,1));
    p=voice_parameters<R>{};p.operators[3].fixed=true;p.operators[3].fixed_hz=R(8);
    e.set_voice(0,p);e.key_on(0,8);R l,r;e.render(&l,&r,1);
    CHECK(e.phase(0,3)==phase_step(R(8),R(48000)));
    p.frequency=R(880);p.lfos[0].pitch_cents=R(1200);p.lfos[0].waveform=lfo_wave::square;
    e.set_voice(0,p);e.render(&l,&r,1);
    CHECK(e.phase(0,3)==2*phase_step(R(8),R(48000)));
    p.operators[3].fixed_pitch_modulation=true;e.set_voice(0,p);e.render(&l,&r,1);
    CHECK(e.phase(0,3)==2*phase_step(R(8),R(48000))+phase_step(R(16),R(48000)));
    p.operators[3].release=R(127);p.operators[3].reverb=R(127);
    e.set_voice(0,p);e.key_off(0,8);
    for(unsigned i=0;i<2000;++i)e.render(&l,&r,1);
    CHECK(e.active(0)); // slowed OPZ reverb tail
}

template<class R> void noise_clock()
{
    fm_engine<R,1> e;e.prepare(R(48000),R(3072000));
    voice_parameters<R> p;p.noise=true;p.noise_frequency=R(48000);
    e.set_voice(0,p);e.key_on(0,8);
    // Reference OPM/OPZ shift register ticks twice per native audio sample.
    uint32_t state=1;
    bool positive=false,negative=false;
    for(unsigned i=0;i<128;++i){
        for(unsigned j=0;j<2;++j){
            state<<=1;
            state|=((state>>17)^(state>>14)^1)&1;
        }
        R expected=((state>>17)&1?R(-1):R(1))*R(1023)/R(4096);
        R l,r;e.render(&l,&r,1);CHECK(l==expected);
        positive|=l>R(0);negative|=l<R(0);
    }
    CHECK(positive&&negative);
}

void double_is_double()
{
    fm_engine<double,1> a,b;voice_parameters<double> p;
    p.frequency=440.00000001;a.set_voice(0,p);a.key_on(0,8);
    p.frequency=440.00000002;b.set_voice(0,p);b.key_on(0,8);
    double l,r,x,y;a.render(&l,&r,1);b.render(&x,&y,1);
    CHECK(a.phase(0,3)!=b.phase(0,3));CHECK(l!=x);
    CHECK(float(440.00000001)==float(440.00000002));
}

void imports()
{
    ymfm::opm_registers opm;opm.reset();
    ymfm::opz_registers opz;opz.reset();
    auto write=[](auto& r,unsigned a,unsigned d){uint32_t c,m;r.write(uint16_t(a),uint8_t(d),c,m);};
    write(opm,0x20,0xc7);write(opm,0x40,2);write(opm,0x80,31);write(opm,0xe0,0xff);
    voice_parameters<double> p;
    CHECK(import_opm(opm,0,3579545.0,p));
    CHECK(p.algorithm==7);CHECK(p.gain_left==1&&p.gain_right==1);
    CHECK(p.operators[0].ratio==2);CHECK(p.operators[0].attack==127);
    CHECK(p.operators[0].sustain_level==127);CHECK(p.operators[0].release==127);
    CHECK(!import_opm(opm,8,3579545.0,p));
    write(opz,0x40+24,0x12);write(opz,0x40+24,0xf3);write(opz,0x80+24,0x3f);
    CHECK(import_opz(opz,0,3579545.0,p));
    CHECK(p.operators[3].fixed);CHECK(p.operators[3].fixed_hz==70);
    CHECK(p.operators[3].waveform==7);
    fm_engine<double,1> e(model::opz);CHECK(e.set_voice(0,p));
}

template<class R> void exponential_accuracy()
{
    const auto& table=exponential_table<R>::instance();
    long double worst=0;
    for(unsigned i=0;i<=300000;++i){
        R x=R(-64)+R(80)*R(i)/R(300000);
        long double reference=std::exp2(static_cast<long double>(x));
        worst=std::max(worst,std::abs(static_cast<long double>(table.lookup(x))/reference-1));
    }
    std::printf("exponential %s max relative error %.12Lg\n",sizeof(R)==4?"f32":"f64",worst);
    CHECK(worst<(sizeof(R)==4?2e-7L:4e-15L));
    for(int k=-4096;k<=1024;++k){
        R x=R(k)/R(64);
        CHECK(std::abs(table.lookup(x)/std::exp2(x)-R(1))<R(4)*std::numeric_limits<R>::epsilon());
    }
}

void sleeping_random()
{
    const auto& jump=random_jump_table::instance();
    for(uint32_t seed:{1u,123u,0xdeadbeefu}){
        uint32_t state=seed;
        for(unsigned n=0;n<10000;++n){
            if(n%113==0)CHECK(jump.advance(seed,n)==state);
            random_jump_table::next(state);
        }
        CHECK(jump.advance(seed,UINT64_C(4294967295))==seed);
        CHECK(jump.advance(seed,UINT64_C(0x123456789abcdef0))==
              jump.advance(jump.advance(seed,UINT64_C(0x1234567800000000)),UINT64_C(0x9abcdef0)));
    }
#if defined(__SIZEOF_INT128__)
    uint64_t a=1,b=9;
    for(unsigned i=0;i<10000;++i){
        a=a*UINT64_C(6364136223846793005)+1;b=b*UINT64_C(1442695040888963407)+1;
        CHECK(multiply_high(a,b)==uint64_t((__uint128_t(a)*b)>>64));
    }
#endif
}

template<class R> void sleeping_voices()
{
    fm_engine<R,128> asleep(model::opz),running(model::opz);
    voice_parameters<R> p;
    p.lfos[0].frequency=R(700);p.lfos[0].pitch_cents=R(12);
    p.lfos[0].waveform=lfo_wave::noise;p.lfos[0].key_sync=false;
    p.lfos[1].frequency=R(317);p.lfos[1].amplitude=R(14);
    p.lfos[1].waveform=lfo_wave::sine;p.lfos[1].key_sync=false;
    p.operators[3].am_enabled=true;
    p.operators[3].release=R(127);
    asleep.set_voice(127,p);running.set_voice(127,p);running.key_on(127,8);
    R a[256],b[256],c[256],d[256];
    for(unsigned n=0;n<200;++n){asleep.render(a,b,256);running.render(c,d,256);}
    asleep.key_on(127,8);running.key_on(127,8);
    asleep.render(a,b,256);running.render(c,d,256);
    for(unsigned i=0;i<256;++i)CHECK(a[i]==c[i]);
    // Disable and later re-enable an LFO, preserving its free-running clock.
    p.lfos[0].pitch_cents=R(0);asleep.set_voice(127,p);running.set_voice(127,p);
    asleep.key_off(127);running.key_off(127);
    for(unsigned n=0;n<50;++n){asleep.render(a,b,256);running.render(c,d,256);}
    CHECK(!asleep.active(127));CHECK(!running.active(127));
    p.lfos[0].pitch_cents=R(12);asleep.set_voice(127,p);running.set_voice(127,p);
    asleep.key_on(127,8);running.key_on(127,8);
    asleep.render(a,b,256);running.render(c,d,256);
    for(unsigned i=0;i<256;++i)CHECK(a[i]==c[i]);

    // Finish a release in a large block vs one sample at a time. A subsequent
    // free-running noise LFO key-on must agree across both retirement paths.
    asleep.key_off(127);running.key_off(127);
    std::vector<R> large(8192),other(8192);
    asleep.render(large.data(),other.data(),8192);
    for(unsigned n=0;n<8192;++n){running.render(c,d,1);CHECK(c[0]==large[n]);}
    CHECK(!asleep.active(127));CHECK(!running.active(127));
    asleep.key_on(127,8);running.key_on(127,8);
    asleep.render(a,b,256);running.render(c,d,256);
    for(unsigned i=0;i<256;++i)CHECK(a[i]==c[i]);

    // All ended, even with noise enabled: silence and unchanged operator phase.
    p.noise=true;asleep.set_voice(127,p);asleep.key_off(127);
    asleep.render(large.data(),other.data(),8192);CHECK(!asleep.active(127));
    uint64_t phase=asleep.phase(127,3);
    asleep.render(large.data(),other.data(),8192);
    CHECK(asleep.phase(127,3)==phase);
    for(auto x:large)CHECK(x==R(0));
}

// Compare the singleton kernel with the general kernel carrying an inaudible
// second voice, including retirement of that voice inside a render block.
template<class R> void singleton_transition()
{
    for (unsigned algorithm=0;algorithm<8;++algorithm) {
        fm_engine<R,128> one(model::opz),two(model::opz);
        voice_parameters<R> patch;
        patch.algorithm=algorithm;patch.feedback=R(80);patch.frequency=R(137.25);
        patch.lfos[0].frequency=R(37);patch.lfos[0].pitch_cents=R(23);
        patch.lfos[1].frequency=R(91);patch.lfos[1].amplitude=R(17);
        for(unsigned o=0;o<4;++o){
            patch.operators[o].waveform=(algorithm+o)%8;
            patch.operators[o].am_enabled=true;
            patch.operators[o].total_level=R(9*o);
        }
        CHECK(one.set_voice(127,patch));CHECK(two.set_voice(127,patch));
        one.key_on(127);two.key_on(127);
        patch.gain_left=patch.gain_right=R(0);
        for(auto& op:patch.operators)op.release=R(127);
        CHECK(two.set_voice(0,patch));two.key_on(0);
        std::vector<R> a(8192),b(8192),c(8192),d(8192);
        auto compare=[&](unsigned frames){
            CHECK(one.render(a.data(),b.data(),frames));
            CHECK(two.render(c.data(),d.data(),frames));
            for(unsigned s=0;s<frames;++s){CHECK(a[s]==c[s]);CHECK(b[s]==d[s]);}
            for(unsigned o=0;o<4;++o){
                CHECK(one.phase(127,o)==two.phase(127,o));
                CHECK(one.attenuation(127,o)==two.attenuation(127,o));
            }
        };
        compare(257);two.key_off(0);compare(8192);
        CHECK(!two.active(0));compare(257);
        // Re-enter the general kernel without disturbing the audible voice.
        two.key_on(0);compare(257);
    }
}

template<class R> void held_envelope_edits()
{
    for (model chip : {model::opm,model::opz}) {
        fm_engine<R,128> engine(chip);
        voice_parameters<R> patch;
        patch.frequency=R(375);patch.algorithm=7;
        patch.operators[3].attack=R(127);
        patch.operators[3].release=R(127);
        CHECK(engine.set_voice(127,patch));CHECK(engine.key_on(127,8));
        R left[256],right[256];
        CHECK(engine.render(left,right,256));
        CHECK(engine.attenuation(127,3)==R(0));CHECK(engine.active(127));
        patch.operators[3].sustain_rate=R(40.5);
        CHECK(engine.set_voice(127,patch));CHECK(engine.render(left,right,256));
        CHECK(engine.attenuation(127,3)>R(0));
        patch.operators[3].sustain_rate=R(0);
        CHECK(engine.set_voice(127,patch));CHECK(engine.render(left,right,256));
        R held=engine.attenuation(127,3);
        CHECK(engine.render(left,right,256));CHECK(engine.attenuation(127,3)==held);
        // Tone edits must remain effective while the EG itself is stationary.
        patch.operators[3].total_level=R(12.5);
        patch.operators[3].envelope_shift=R(1.5);
        patch.lfos[0].amplitude=R(30);patch.lfos[0].frequency=R(11);
        patch.operators[3].am_enabled=true;
        CHECK(engine.set_voice(127,patch));CHECK(engine.render(left,right,256));
        CHECK(engine.attenuation(127,3)==held);
        CHECK(engine.key_off(127,8));CHECK(engine.render(left,right,256));
        CHECK(engine.attenuation(127,3)>held);
        for(unsigned n=0;n<32;++n)CHECK(engine.render(left,right,256));
        CHECK(!engine.active(127));
        CHECK(engine.key_on(127,8));CHECK(engine.render(left,right,256));
        CHECK(engine.active(127));CHECK(engine.attenuation(127,3)==R(0));
    }
}

template<class R> void steady_packets()
{
    // An inaudible extra slot breaks the reference's contiguous voice run,
    // so it always uses the general kernel without changing the audible mix.
    for (unsigned voices : {3u,4u,5u,8u,17u,123u}) {
        fm_engine<R,128> packet(model::opz),reference(model::opz);
        std::array<voice_parameters<R>,128> patches;
        voice_parameters<R> silent;
        silent.gain_left=silent.gain_right=R(0);silent.operators[3].attack=R(0);
        CHECK(reference.set_voice(0,silent));CHECK(reference.key_on(0,8));
        for(unsigned n=0;n<voices;++n){
            unsigned v=n+5;auto& p=patches[v];
            p.algorithm=n%8;p.feedback=R(n%100);p.frequency=R(73.5+3*n);
            p.gain_left=R(0.007);p.gain_right=R(0.009);
            for(unsigned o=0;o<4;++o){
                auto& op=p.operators[o];op.waveform=(n+o)%8;
                op.total_level=R(13.25*o);op.envelope_shift=R(0.25*o);
                // One exact decay increment gives a nonzero held attenuation.
                op.decay=R(127);op.sustain_level=R(0.1);
                op.release=R(127);op.am_enabled=true;
            }
            CHECK(packet.set_voice(v,p));
            CHECK(reference.set_voice(v,p));
            CHECK(packet.key_on(v));CHECK(reference.key_on(v));
        }
        std::vector<R> a(4096),b(4096),c(4096),d(4096);
        auto compare=[&](unsigned frames){
            allocations=0;watch_allocation=true;
            CHECK(packet.render(a.data(),b.data(),frames));
            CHECK(reference.render(c.data(),d.data(),frames));
            watch_allocation=false;CHECK(allocations==0);
            for(unsigned n=0;n<frames;++n){CHECK(a[n]==c[n]);CHECK(b[n]==d[n]);}
            for(unsigned v=5;v<5+voices;++v)for(unsigned o=0;o<4;++o){
                CHECK(packet.phase(v,o)==reference.phase(v,o));
                CHECK(packet.attenuation(v,o)==reference.attenuation(v,o));
            }
        };
        compare(1); // settle envelopes, then enter the packet path next block
        compare(257);compare(4096);
        auto edit=[&](unsigned v){
            CHECK(packet.set_voice(v,patches[v]));
            CHECK(reference.set_voice(v,patches[v]));
        };
        patches[5].operators[3].total_level=R(43.75);edit(5);compare(256);
        patches[6].noise=true;edit(6);compare(256);
        patches[6].noise=false;edit(6);compare(256);
        // A real LFO must invalidate the block's constant-gain eligibility.
        patches[5].lfos[0].frequency=R(7);patches[5].lfos[0].amplitude=R(19);
        edit(5);compare(257);
        patches[5].lfos[0].amplitude=R(0);edit(5);compare(257);
        // Pitch-only LFOs keep amplitude invariant. Mix fixed-frequency
        // operators with and without pitch modulation in the same packet.
        for(unsigned v=5;v<5+voices;++v){
            auto& p=patches[v];p.lfos[0].frequency=R(v+1);p.lfos[0].pitch_cents=R(73);
            p.lfos[0].waveform=static_cast<lfo_wave>(v%5);
            p.operators[1].fixed=v%3==0;p.operators[1].fixed_pitch_modulation=v%2==0;
            edit(v);
        }
        compare(257);compare(4096);
        // Global AM does not affect operators with AM disabled.
        for(unsigned v=5;v<5+voices;++v){
            patches[v].lfos[0].amplitude=R(17);
            for(unsigned o=0;o<4;++o)patches[v].operators[o].am_enabled=o%2==0;
            edit(v);
        }
        compare(257);compare(4096);
        // Release just one operator: other operators initially remain eligible.
        // Retirement of its voice later splits the contiguous run mid-block.
        CHECK(packet.key_off(6,1));CHECK(reference.key_off(6,1));compare(256);
        CHECK(packet.key_off(6));CHECK(reference.key_off(6));compare(4096);
        CHECK(!packet.active(6));CHECK(!reference.active(6));compare(257);
        CHECK(packet.key_on(6));CHECK(reference.key_on(6));compare(1);compare(257);
    }
}

template<class R> void dynamic_packets()
{
    for(model chip:{model::opm,model::opz})for(unsigned voices:{4u,5u,17u})for(bool lfo:{false,true}) {
        fm_engine<R,128> packet(chip),reference(chip);
        voice_parameters<R> silent;silent.gain_left=silent.gain_right=R(0);
        // The silent reference voice forces the general modulated path even
        // when every audible voice has disabled LFOs.
        silent.lfos[0].frequency=R(5);silent.lfos[0].pitch_cents=R(13);
        silent.operators[3].attack=R(0);reference.set_voice(0,silent);reference.key_on(0,8);
        std::array<voice_parameters<R>,128> patches;
        for(unsigned n=0;n<voices;++n){
            unsigned v=5+n;auto& p=patches[v];p.algorithm=n%8;p.feedback=R(83.5);
            p.frequency=R(117.25+n*7);p.gain_left=p.gain_right=R(0.02);
            p.lfos[0].frequency=R(37);p.lfos[0].pitch_cents=lfo?R(31):R(0);p.lfos[0].amplitude=lfo?R(17):R(0);
            for(unsigned o=0;o<4;++o){auto& op=p.operators[o];
                op.waveform=chip==model::opz?(n+o)%8:0;
                op.attack=R(80.25+7*o+n%3);op.decay=R(90.5+n%5);
                op.sustain_level=R(3+n%3);op.sustain_rate=n%2?R(4.1):R(0);
                op.release=R(127);op.reverb=R(100.5);op.total_level=R(12.5*o);op.am_enabled=true;
            }
            packet.set_voice(v,p);reference.set_voice(v,p);packet.key_on(v);reference.key_on(v);
        }
        std::vector<R>a(8192),b(8192),c(8192),d(8192);
        auto compare=[&](unsigned frames){
            allocations=0;watch_allocation=true;
            CHECK(packet.render(a.data(),b.data(),frames));CHECK(reference.render(c.data(),d.data(),frames));
            watch_allocation=false;CHECK(allocations==0);
            CHECK(std::equal(a.begin(),a.begin()+frames,c.begin()));
            CHECK(std::equal(b.begin(),b.begin()+frames,d.begin()));
            bool equal=true;
            for(unsigned v=5;v<5+voices;++v)for(unsigned o=0;o<4;++o)
                equal&=packet.phase(v,o)==reference.phase(v,o)&&packet.attenuation(v,o)==reference.attenuation(v,o);
            CHECK(equal);
        };
        compare(257);compare(8192);
        // Exercise block dispatch changes while envelopes continue running.
        for(bool enable:{!lfo,lfo}) {
            for(unsigned v=5;v<5+voices;++v){
                patches[v].lfos[0].pitch_cents=enable?R(31):R(0);
                patches[v].lfos[0].amplitude=enable?R(17):R(0);
                packet.set_voice(v,patches[v]);reference.set_voice(v,patches[v]);
            }
            compare(257);
        }
        for(unsigned v=5;v<5+voices;++v){packet.key_off(v);reference.key_off(v);}
        compare(257);compare(8192);
        for(unsigned v=5;v<5+voices;++v){
            patches[v].noise=v==5;patches[v].operators[0].attack=R(0);
            packet.set_voice(v,patches[v]);reference.set_voice(v,patches[v]);
            packet.key_on(v);reference.key_on(v);
        }
        compare(257);compare(8192);
    }
}

int main()
{
    wave_accuracy<float>();wave_accuracy<double>();
    wave_packets<float>();wave_packets<double>();
    exponential_packets<float>();exponential_packets<double>();
    phase_boundaries<float>();phase_boundaries<double>();
    algorithms<float>();algorithms<double>();
    envelopes<float>();envelopes<double>();
    feedback_and_tails<float>();feedback_and_tails<double>();
    block_and_polyphony<float>();block_and_polyphony<double>();
    validation_and_opz<float>();validation_and_opz<double>();
    noise_clock<float>();noise_clock<double>();
    double_is_double();imports();
    exponential_accuracy<float>();exponential_accuracy<double>();
    singleton_transition<float>();singleton_transition<double>();
    held_envelope_edits<float>();held_envelope_edits<double>();
    steady_packets<float>();steady_packets<double>();
    dynamic_packets<float>();dynamic_packets<double>();
    sleeping_random();sleeping_voices<float>();sleeping_voices<double>();
    std::printf("%u failures\n",failures);
    return failures?1:0;
}
