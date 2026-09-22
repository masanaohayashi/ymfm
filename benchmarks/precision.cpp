#include "ymfm_precision.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

template<class Real> void measure(const char* name, unsigned voices, bool lfo, unsigned repeats, bool released=false)
{
    using namespace ymfm::precision;
    fm_engine<Real,128> synth(model::opz);
    voice_parameters<Real> patch;
    patch.algorithm=4; patch.feedback=Real(45.5);
    patch.gain_left=patch.gain_right=Real(1)/128;
    if(lfo){patch.lfos[0].frequency=Real(5.2);patch.lfos[0].pitch_cents=Real(12.5);
        patch.lfos[1].frequency=Real(0.7);patch.lfos[1].amplitude=Real(8.5);}
    for(unsigned op=0;op<4;++op){auto& p=patch.operators[op];p.waveform=op;p.ratio=Real(op+1);
        p.attack=Real(100.25);p.decay=Real(40.5);p.sustain_level=Real(64);
        p.total_level=Real(12*op);p.am_enabled=true;}
    for(unsigned v=0;v<voices;++v){patch.frequency=Real(110)*std::exp2(Real(v%36)/12);
        if(!synth.set_voice(v,patch))std::abort();synth.key_on(v);}
    Real left[256],right[256];
    if(released){
        patch.noise=true;patch.operators[3].release=Real(127);
        synth.set_voice(voices-1,patch);
        synth.render(left,right,256);
        for(unsigned v=0;v<voices;++v)synth.key_off(v);
        for(unsigned n=0;n<256;++n)synth.render(left,right,256);
        for(unsigned v=0;v<voices;++v)if(synth.active(v))std::abort();
    }
    std::vector<double> times;
    double checksum=0;
    for(unsigned rep=0;rep<repeats;++rep){
        auto start=std::chrono::steady_clock::now();
        for(unsigned block=0;block<64;++block){synth.render(left,right,256);
            for(Real x:left)checksum+=double(x)*double(x);}
        times.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
    }
    std::sort(times.begin(),times.end());
    std::printf("%s_%s %.9f %.12g\n",sizeof(Real)==4?"f32":"f64",name,times[times.size()/2],checksum);
}
int main(int argc,char** argv){
    unsigned repeats=argc>1?unsigned(std::atoi(argv[1])):3;
    if(!repeats)return 1;
    if(argc>2){measure<double>("full_lfo",128,true,repeats);return 0;}
    measure<float>("idle",0,false,repeats);measure<double>("idle",0,false,repeats);
    measure<float>("released",128,true,repeats,true);measure<double>("released",128,true,repeats,true);
    measure<float>("one",1,true,repeats);measure<double>("one",1,true,repeats);
    measure<float>("full_dry",128,false,repeats);measure<double>("full_dry",128,false,repeats);
    measure<float>("full_lfo",128,true,repeats);measure<double>("full_lfo",128,true,repeats);
}
