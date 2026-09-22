#include "ymfm_precision.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <ctime>

static int selected_count = 0;
static char** selected_cases = nullptr;

template<class Real> void measure(const char* name, unsigned voices, bool lfo, unsigned repeats, bool released=false, bool steady=false, unsigned block_size=256, bool pitch_only=false, bool staggered=false, unsigned envelope_case=0)
{
    char case_name[80];
    std::snprintf(case_name,sizeof(case_name),"%s_%s",sizeof(Real)==4?"f32":"f64",name);
    if(selected_count){
        bool selected=false;
        for(int i=0;i<selected_count;++i)selected|=std::strcmp(case_name,selected_cases[i])==0;
        if(!selected)return;
    }
    using namespace ymfm::precision;
    fm_engine<Real,128> synth(model::opz);
    voice_parameters<Real> patch;
    patch.algorithm=4; patch.feedback=Real(45.5);
    patch.gain_left=patch.gain_right=Real(1)/128;
    if(lfo){patch.lfos[0].frequency=Real(5.2);patch.lfos[0].pitch_cents=Real(12.5);
        patch.lfos[1].frequency=Real(0.7);patch.lfos[1].amplitude=pitch_only?Real(0):Real(8.5);}
    for(unsigned op=0;op<4;++op){auto& p=patch.operators[op];p.waveform=op;p.ratio=Real(op+1);
        p.attack=Real(100.25);p.decay=Real(40.5);p.sustain_level=Real(64);
        p.total_level=Real(12*op);p.am_enabled=true;
        if(envelope_case==1)p.attack=Real(40);
        if(envelope_case==2){p.attack=Real(127);p.decay=Real(0);p.sustain_level=Real(0);p.release=Real(40);}
        if(steady){p.attack=Real(127);p.decay=Real(0);p.sustain_level=Real(0);}}
    for(unsigned v=0;v<voices;++v){patch.frequency=Real(110)*std::exp2(Real(v%36)/12);
        if(staggered)for(auto& op:patch.operators){
            op.attack=v%4==0?Real(80.25):Real(127);
            op.decay=v%4==2?Real(0):Real(40.5);
            op.sustain_level=v%4==2?Real(0):Real(64);
        }
        if(!synth.set_voice(v,patch))std::abort();synth.key_on(v);}
    Real left[1024],right[1024];
    if(!block_size || block_size>1024 || 16384%block_size)std::abort();
    if(staggered){
        synth.render(left,right,256);
        for(unsigned v=3;v<voices;v+=4)synth.key_off(v);
    }
    if(released){
        patch.noise=true;patch.operators[3].release=Real(127);
        synth.set_voice(voices-1,patch);
        synth.render(left,right,256);
        for(unsigned v=0;v<voices;++v)synth.key_off(v);
        for(unsigned n=0;n<256;++n)synth.render(left,right,256);
        for(unsigned v=0;v<voices;++v)if(synth.active(v))std::abort();
    }
    const bool cpu_time=std::getenv("YMFM_BENCH_CPU_TIME")!=nullptr;
    std::vector<double> times;
    double checksum=0;
    for(unsigned rep=0;rep<repeats;++rep){
        if(envelope_case==2)for(unsigned v=0;v<voices;++v){synth.key_on(v);synth.key_off(v);}
        auto start=std::chrono::steady_clock::now();
        std::clock_t cpu_start=cpu_time?std::clock():0;
        for(unsigned block=0;block<16384/block_size;++block){synth.render(left,right,block_size);
            for(unsigned s=0;s<block_size;++s)checksum+=double(left[s])*double(left[s]);}
        double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        if(cpu_time){
            std::clock_t cpu_end=std::clock();
            if(cpu_start==std::clock_t(-1) || cpu_end==std::clock_t(-1))std::abort();
            elapsed=double(cpu_end-cpu_start)/CLOCKS_PER_SEC;
        }
        times.push_back(elapsed);
        if(envelope_case)for(unsigned v=0;v<voices;++v)for(unsigned o=0;o<4;++o){
            Real a=synth.attenuation(v,o);
            if(!synth.active(v) || (envelope_case==1 && a<=Real(0)) ||
               (envelope_case==2 && (a<=Real(0) || a>=Real(192)))){
                std::fprintf(stderr,"%s left target EG stage: rep=%u attenuation=%g\n",case_name,rep,double(a));
                std::abort();
            }
        }
    }
    std::sort(times.begin(),times.end());
    std::printf("%s_%s %.9f %.12g\n",sizeof(Real)==4?"f32":"f64",name,times[times.size()/2],checksum);
}
int main(int argc,char** argv){
    unsigned repeats=argc>1?unsigned(std::atoi(argv[1])):3;
    if(!repeats)return 1;
    if(argc>2 && std::strcmp(argv[2],"--cases")==0){
        if(argc<4)return 1;
        selected_count=argc-3;selected_cases=argv+3;
    }
    if(argc>2 && std::strcmp(argv[2],"mixed")==0){
        measure<float>("mixed_envelopes",128,true,repeats,false,false,256,false,true);
        measure<double>("mixed_envelopes",128,true,repeats,false,false,256,false,true);
        return 0;
    }
    if(argc>2 && !selected_count){measure<double>("full_lfo",128,true,repeats);return 0;}
    measure<float>("idle",0,false,repeats);measure<double>("idle",0,false,repeats);
    measure<float>("released",128,true,repeats,true);measure<double>("released",128,true,repeats,true);
    measure<float>("one",1,true,repeats);measure<double>("one",1,true,repeats);
    measure<float>("eight",8,true,repeats);measure<double>("eight",8,true,repeats);
    measure<float>("thirtytwo",32,true,repeats);measure<double>("thirtytwo",32,true,repeats);
    measure<float>("one_dry",1,false,repeats);measure<double>("one_dry",1,false,repeats);
    measure<float>("eight_dry",8,false,repeats);measure<double>("eight_dry",8,false,repeats);
    measure<float>("thirtytwo_dry",32,false,repeats);measure<double>("thirtytwo_dry",32,false,repeats);
    measure<float>("attack_lfo",128,true,repeats,false,false,256,false,false,1);
    measure<double>("attack_lfo",128,true,repeats,false,false,256,false,false,1);
    measure<float>("release_lfo",128,true,repeats,false,false,256,false,false,2);
    measure<double>("release_lfo",128,true,repeats,false,false,256,false,false,2);
    measure<float>("full_dry",128,false,repeats);measure<double>("full_dry",128,false,repeats);
    measure<float>("full_lfo",128,true,repeats);measure<double>("full_lfo",128,true,repeats);
    measure<float>("steady_dry",128,false,repeats,false,true);measure<double>("steady_dry",128,false,repeats,false,true);
    measure<float>("steady_lfo",128,true,repeats,false,true);measure<double>("steady_lfo",128,true,repeats,false,true);
    measure<float>("steady_pitch",128,true,repeats,false,true,256,true);measure<double>("steady_pitch",128,true,repeats,false,true,256,true);
    measure<float>("mixed_envelopes",128,true,repeats,false,false,256,false,true);
    measure<double>("mixed_envelopes",128,true,repeats,false,false,256,false,true);
    measure<float>("small_block",128,true,repeats,false,false,16);measure<double>("small_block",128,true,repeats,false,false,16);
    measure<float>("large_block",128,true,repeats,false,false,1024);measure<double>("large_block",128,true,repeats,false,false,1024);
}
