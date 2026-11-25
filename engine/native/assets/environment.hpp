#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace engine::assets {
struct EnvironmentMap {
    unsigned width=0,height=0;
    std::vector<float> pixels; // Linear Rec.709 RGBA32F, north row first; A=1.
    std::array<std::array<float,3>,9> diffuseSH{}; // Cosine-convolved irradiance.
    std::string contentHash;
};
inline std::shared_ptr<const EnvironmentMap> decodeEnvironmentData(std::string_view bytes,const std::string& hash){
    const auto require=[](bool value,const char* message){if(!value)throw std::runtime_error(std::string("NG-E-ASSET-ENVIRONMENT: ")+message);};
    require(bytes.size()>=160&&bytes.substr(0,4)=="NGE1","invalid container");
    const auto u32=[&](std::size_t at){std::uint32_t n=0;for(unsigned i=0;i<4;i++)n|=std::uint32_t(static_cast<unsigned char>(bytes[at+i]))<<(i*8);return n;};
    const auto f32=[&](std::size_t at){const auto n=u32(at);float result;std::memcpy(&result,&n,4);return result;};
    const unsigned width=u32(8),height=u32(12);const auto count=std::uint64_t(width)*height;
    require(u32(4)==1&&width>=2&&width<=16384&&height>0&&std::uint64_t(width)==std::uint64_t(height)*2&&count*16<=256ull*1024*1024,"invalid panorama dimensions");
    require(u32(16)==4&&u32(20)==160&&u32(24)==count*16&&u32(28)==0&&bytes.size()==160+count*16,"invalid layout");for(unsigned at=140;at<160;at++)require(bytes[at]==0,"invalid reserved bytes");
    auto map=std::make_shared<EnvironmentMap>();map->width=width;map->height=height;map->contentHash=hash;
    for(unsigned i=0;i<27;i++){const auto value=f32(32+i*4);require(std::isfinite(value)&&std::abs(value)<=1e14f,"invalid diffuse SH");map->diffuseSH[i/3][i%3]=value;}
    map->pixels.resize(static_cast<std::size_t>(count)*4);
    for(std::size_t i=0;i<map->pixels.size();i++){const auto value=f32(160+i*4);require(i%4==3?value==1:std::isfinite(value)&&value>=0&&value<=1e12f,"invalid radiance/alpha");map->pixels[i]=value;}
    return map;
}
}
