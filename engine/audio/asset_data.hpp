#pragma once
#include "audio.hpp"
#include <cmath>
#include <cstring>
#include <string_view>

namespace engine::audio {
// Shared by the atomic scene loader and file loader. No device or mixer linkage.
inline bool decodeNgsndData(std::string_view bytes, Sound& out, std::string& error) {
    const auto fail=[&](const char* reason){error=reason;return false;};
    if(bytes.size()<44||bytes.size()>64u*1024*1024||bytes.substr(0,4)!="NGS1")return fail("Invalid or oversized ngsnd");
    const auto u16=[&](size_t p){return uint32_t(uint8_t(bytes[p]))|(uint32_t(uint8_t(bytes[p+1]))<<8);};
    const auto u32=[&](size_t p){return u16(p)|(u16(p+2)<<16);};
    const auto f32=[&](size_t p){const auto bits=u32(p);float v;std::memcpy(&v,&bits,4);return v;};
    const auto channels=u16(6),frames=u32(12),flags=u16(18),offset=u32(36),size=u32(40);
    if(u16(4)!=1||channels<1||channels>2||u32(8)!=kSampleRate||!frames||u16(16)!=2||flags>1||offset<44||offset%4||
       uint64_t(frames)*channels*4!=size||uint64_t(offset)+size!=bytes.size()||u32(20)!=0||u32(24)!=(flags?frames:0))return fail("Unsupported or truncated ngsnd");
    if(!std::isfinite(f32(28))||!std::isfinite(f32(32)))return fail("Invalid ngsnd loudness");
    Sound next;next.channels=channels;next.sampleRate=kSampleRate;next.loop=flags!=0;next.loudnessLufs=f32(28);next.peakDbtp=f32(32);
    next.samples.resize(size/4);
    for(size_t i=0;i<next.samples.size();++i){const float sample=f32(offset+i*4);if(!std::isfinite(sample)||std::abs(sample)>16)return fail("Invalid ngsnd sample");next.samples[i]=sample;}
    out=std::move(next);error.clear();return true;
}
}
