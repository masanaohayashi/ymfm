// Offline numerical example and scalar throughput measurement (no audio I/O).
// cmake --build build --target ymfm_precision_example
// build/ymfm_precision_example
#include "ymfm_precision.h"
#include <chrono>
#include <cstdio>
#include <vector>

template<class Real> void run()
{
    using namespace ymfm::precision;
    fm_engine<Real,128> synth(model::opz);
    synth.prepare(Real(48000));
    voice_parameters<Real> patch;
    patch.algorithm=4;
    patch.feedback=Real(45.5);
    patch.gain_left=patch.gain_right=Real(1)/Real(128);
    patch.lfos[0].frequency=Real(5.2);
    patch.lfos[0].pitch_cents=Real(12.5);
    patch.lfos[1].frequency=Real(0.7);
    patch.lfos[1].amplitude=Real(8.5);
    for(unsigned op=0;op<4;++op){
        auto& p=patch.operators[op];
        p.waveform=op;p.ratio=Real(op+1);
        p.attack=Real(100.25);p.decay=Real(40.5);p.sustain_level=Real(64);
        p.total_level=Real(12*op);p.am_enabled=true;
    }
    for(unsigned v=0;v<128;++v){
        patch.frequency=Real(110)*std::exp2(Real(v%36)/Real(12));
        if(!synth.set_voice(v,patch))return;
        synth.key_on(v);
    }
    std::vector<Real> left(256),right(256);
    auto begin=std::chrono::steady_clock::now();
    Real checksum=0;
    for(unsigned block=0;block<188;++block){
        synth.render(left.data(),right.data(),left.size());
        for(Real s:left)checksum+=s*s;
    }
    double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    std::printf("%s: 128 voices, 48128 frames, %.4f CPU wall seconds, checksum %.8g\n",
                sizeof(Real)==4?"float32":"float64",elapsed,double(checksum));
}
int main(){run<float>();run<double>();}
