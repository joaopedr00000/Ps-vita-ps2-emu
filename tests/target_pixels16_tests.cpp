#include <cstdio>
#include <random>
#include <algorithm>
#include "TargetPixels16.h"
int main(){
    uint16_t layout[128],block[128],out[128]; unsigned fail=0;
    const unsigned tab[2][16]={{0,2,8,10,16,18,24,26,1,3,9,11,17,19,25,27},{4,6,12,14,20,22,28,30,5,7,13,15,21,23,29,31}};
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x)layout[(y/2)*32+tab[y%2][x]]=x|(y<<8);
    if(!TargetPixels16::Matches(layout))return 1;
    for(unsigned base=0;base<65536;base+=128){
        uint32_t p[8*23];std::fill_n(p,8*23,0xdeadbeefu);
        for(unsigned i=0;i<128;++i)block[i]=base+i;
        TargetPixels16::Read(block,p,23);
        for(unsigned i=0;i<128;++i){unsigned q=layout[i],c=block[i];uint32_t expected=((c&31)<<3)|((c&0x3e0)<<6)|((c&0x7c00)<<9)|((c&0x8000)?0xff000000u:0);fail+=p[(q>>8)*23+(q&255)]!=expected;}
        for(unsigned y=0;y<8;++y)for(unsigned x=16;x<23;++x)fail+=p[y*23+x]!=0xdeadbeefu;
        TargetPixels16::Write(p,23,out);for(unsigned i=0;i<128;++i)fail+=out[i]!=block[i];
    }
    std::mt19937 rng(1234);
    for(unsigned n=0;n<2000;++n){uint32_t p[8*29];for(auto& x:p)x=rng();TargetPixels16::Write(p,29,out);
        for(unsigned i=0;i<128;++i){unsigned q=layout[i];uint32_t c=p[(q>>8)*29+(q&255)],gs=(c&0xffffffu)|((((c>>24)+1)/2)<<24);fail+=out[i]!=(((gs>>3)&31)|((gs>>6)&0x3e0)|((gs>>9)&0x7c00)|((gs>>16)&0x8000));}}
    std::printf("GS16: %u failures\n",fail);return fail?1:0;
}
