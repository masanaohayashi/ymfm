#include "ymfm_precision.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

template<class Real> void measure(const char* name, unsigned voices, bool lfo, unsigned repeats, bool released=false, bool steady=false, unsigned block_size=256)
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
        p.total_level=Real(12*op);p.am_enabled=true;
        if(steady){p.attack=Real(127);p.decay=Real(0);p.sustain_level=Real(0);}}
    for(unsigned v=0;v<voices;++v){patch.frequency=Real(110)*std::exp2(Real(v%36)/12);
        if(!synth.set_voice(v,patch))std::abort();synth.key_on(v);}
    Real left[1024],right[1024];
    if(!block_size || block_size>1024 || 16384%block_size)std::abort();
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
        for(unsigned block=0;block<16384/block_size;++block){synth.render(left,right,block_size);
            for(unsigned s=0;s<block_size;++s)checksum+=double(left[s])*double(left[s]);}
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
    measure<float>("eight",8,true,repeats);measure<double>("eight",8,true,repeats);
    measure<float>("thirtytwo",32,true,repeats);measure<double>("thirtytwo",32,true,repeats);
    measure<float>("full_dry",128,false,repeats);measure<double>("full_dry",128,false,repeats);
    measure<float>("full_lfo",128,true,repeats);measure<double>("full_lfo",128,true,repeats);
    measure<float>("steady_dry",128,false,repeats,false,true);measure<double>("steady_dry",128,false,repeats,false,true);
    measure<float>("steady_lfo",128,true,repeats,false,true);measure<double>("steady_lfo",128,true,repeats,false,true);
    measure<float>("small_block",128,true,repeats,false,false,16);measure<double>("small_block",128,true,repeats,false,false,16);
    measure<float>("large_block",128,true,repeats,false,false,1024);measure<double>("large_block",128,true,repeats,false,false,1024);
}
