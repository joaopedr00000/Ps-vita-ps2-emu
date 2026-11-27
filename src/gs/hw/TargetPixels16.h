#pragma once
#include <stdint.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif
namespace TargetPixels16 {
inline unsigned Place(unsigned u) {
    unsigned k=u%8, g=(u%32)/8;
    return (2*g+((k&1)*8)+((k>>1)&1)) | ((2*(u/32)+(k>>2))<<8);
}
inline bool Matches(const uint16_t* place) {
    for(unsigned u=0;u<128;++u) if(place[u]!=Place(u)) return false;
    return true;
}
inline uint32_t Expand(uint16_t c) {
    return ((c&31u)<<3)|((c&0x3e0u)<<6)|((c&0x7c00u)<<9)|((c&0x8000u)?0xff000000u:0);
}
inline uint16_t Compress(uint32_t c) {
    return static_cast<uint16_t>(((c>>3)&31u)|((c>>6)&0x3e0u)|((c>>9)&0x7c00u)|((c>>24)==255?0x8000u:0));
}
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
inline uint32x4_t Expand4(uint16x4_t c) {
    uint32x4_t v=vmovl_u16(c);
    auto rgb=vorrq_u32(vshlq_n_u32(vandq_u32(v,vdupq_n_u32(31)),3),vshlq_n_u32(vandq_u32(v,vdupq_n_u32(0x3e0)),6));
    rgb=vorrq_u32(rgb,vshlq_n_u32(vandq_u32(v,vdupq_n_u32(0x7c00)),9));
    return vorrq_u32(rgb,vandq_u32(vcgeq_u32(v,vdupq_n_u32(0x8000)),vdupq_n_u32(0xff000000)));
}
inline uint16x4_t Compress4(uint32x4_t v) {
    auto rgb=vorrq_u32(vandq_u32(vshrq_n_u32(v,3),vdupq_n_u32(31)),vandq_u32(vshrq_n_u32(v,6),vdupq_n_u32(0x3e0)));
    rgb=vorrq_u32(rgb,vandq_u32(vshrq_n_u32(v,9),vdupq_n_u32(0x7c00)));
    return vmovn_u32(vorrq_u32(rgb,vandq_u32(vceqq_u32(vshrq_n_u32(v,24),vdupq_n_u32(255)),vdupq_n_u32(0x8000))));
}
inline uint16x8_t Interleave(uint16x8_t v) {
    auto z=vzip_u16(vget_low_u16(v),vget_high_u16(v)); return vcombine_u16(z.val[0],z.val[1]);
}
inline void Expand8(uint16x8_t v,uint32_t* p) {
    vst1q_u32(p,Expand4(vget_low_u16(v))); vst1q_u32(p+4,Expand4(vget_high_u16(v)));
}
inline uint16x8_t Compress8(const uint32_t* p) {
    return vcombine_u16(Compress4(vld1q_u32(p)),Compress4(vld1q_u32(p+4)));
}
inline void Read(const uint16_t* block,uint32_t* p,unsigned stride) {
    for(unsigned col=0;col<4;++col) {
        auto v=vld4q_u16(block+col*32);
        auto lo=vuzpq_u16(v.val[0],v.val[2]), hi=vuzpq_u16(v.val[1],v.val[3]);
        uint32_t* r0=p+2*col*stride; uint32_t* r1=r0+stride;
        Expand8(Interleave(lo.val[0]),r0); Expand8(Interleave(hi.val[0]),r0+8);
        Expand8(Interleave(lo.val[1]),r1); Expand8(Interleave(hi.val[1]),r1+8);
    }
}
inline void Write(const uint32_t* p,unsigned stride,uint16_t* block) {
    for(unsigned col=0;col<4;++col) {
        const uint32_t* r0=p+2*col*stride; const uint32_t* r1=r0+stride;
        auto lo=vuzpq_u16(Compress8(r0),Compress8(r1)), hi=vuzpq_u16(Compress8(r0+8),Compress8(r1+8));
        auto a=vzip_u16(vget_low_u16(lo.val[0]),vget_high_u16(lo.val[0]));
        auto b=vzip_u16(vget_low_u16(hi.val[0]),vget_high_u16(hi.val[0]));
        auto c=vzip_u16(vget_low_u16(lo.val[1]),vget_high_u16(lo.val[1]));
        auto d=vzip_u16(vget_low_u16(hi.val[1]),vget_high_u16(hi.val[1]));
        uint16x8x4_t v; v.val[0]=vcombine_u16(a.val[0],a.val[1]); v.val[1]=vcombine_u16(b.val[0],b.val[1]);
        v.val[2]=vcombine_u16(c.val[0],c.val[1]); v.val[3]=vcombine_u16(d.val[0],d.val[1]); vst4q_u16(block+col*32,v);
    }
}
#else
inline void Read(const uint16_t* block,uint32_t* p,unsigned stride) {
    for(unsigned u=0;u<128;++u){unsigned q=Place(u);p[(q>>8)*stride+(q&255)]=Expand(block[u]);}
}
inline void Write(const uint32_t* p,unsigned stride,uint16_t* block) {
    for(unsigned u=0;u<128;++u){unsigned q=Place(u);block[u]=Compress(p[(q>>8)*stride+(q&255)]);}
}
#endif
}
