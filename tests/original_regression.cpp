#include "ymfm_opm.h"
#include "ymfm_opz.h"
#include <cstdio>
#include <vector>
#include <cstdlib>

// Golden hashes are generated against upstream 81aec25, before modifying any
// original source. Samples include algorithm changes, feedback, EG, LFO,
// OPZ alternate waveform/fixed mode, keyoff and register edits during release.
template<class Chip> uint64_t exercise(bool opz)
{
    ymfm::ymfm_interface interface;
    Chip chip(interface);chip.reset();
    auto write=[&](unsigned a,unsigned d){chip.write_address(uint8_t(a));chip.write_data(uint8_t(d));};
    auto program=[&](unsigned alg){
        for(unsigned ch=0;ch<8;++ch){
            write(0x20+ch,0xc0|((ch&7)<<3)|alg);write(0x28+ch,0x30+ch);write(0x30+ch,0x41);
            write(0x38+ch,0x73);
            for(unsigned slot=0;slot<4;++slot){
                unsigned o=ch+slot*8;
                write(0x40+o,0x11+slot);write(0x60+o,slot*9);write(0x80+o,0x1f);
                write(0xa0+o,0x86);write(0xc0+o,0x42);write(0xe0+o,0x48);
                if(opz){write(0x40+o,0x80|((slot+alg)%8<<4)|3);if(slot==2)write(0x80+o,0x3f);}
            }
            if(opz)write(0x20+ch,0x80|((ch&7)<<3)|alg);else write(0x08,0x78|ch);
        }
        write(0x18,0xe4);write(0x19,0x25);write(0x19,0xc0);write(0x1b,2);
    };
    uint64_t hash=UINT64_C(14695981039346656037);
    auto mix=[&](const typename Chip::output_data& out){
        for(unsigned c=0;c<Chip::OUTPUTS;++c){uint32_t v=uint32_t(out.data[c]);
            for(unsigned b=0;b<4;++b){hash^=(v>>(b*8))&255;hash*=UINT64_C(1099511628211);}}
    };
    for(unsigned alg=0;alg<8;++alg){
        program(alg);
        for(unsigned n=0;n<4096;++n){
            if(n==2048)for(unsigned ch=0;ch<8;++ch){if(opz)write(0x20+ch,0xc0|alg);else write(8,ch);}
            if(n==3000)write(0x60,55);
            typename Chip::output_data out;chip.generate(&out);mix(out);
        }
    }
    // Saving/restoring a running voice must reproduce the next 257 samples.
    program(3);
    std::vector<uint8_t> state;
    ymfm::ymfm_saved_state saver(state,true);chip.save_restore(saver);
    typename Chip::output_data first[257],second[257];chip.generate(first,257);
    ymfm::ymfm_saved_state restore(state,false);chip.save_restore(restore);chip.generate(second,257);
    for(unsigned n=0;n<257;++n)for(unsigned c=0;c<Chip::OUTPUTS;++c)
        if(first[n].data[c]!=second[n].data[c]){std::fprintf(stderr,"save restore mismatch\n");std::exit(1);}
    return hash;
}
int main(){
    uint64_t opm=exercise<ymfm::ym2151>(false),opz=exercise<ymfm::ym2414>(true);
    std::printf("OPM %016llx OPZ %016llx\n",(unsigned long long)opm,(unsigned long long)opz);
    if(opm!=UINT64_C(0x9652c829404dbc75) || opz!=UINT64_C(0x8e86f0e4e6264d91)) {
        std::fprintf(stderr,"original output differs from upstream 81aec25\n");return 1;
    }
    return 0;
}
