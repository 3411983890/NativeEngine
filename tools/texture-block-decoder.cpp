#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "../third_party/bcdec/bcdec.h"

int main(int argc,char** argv){try{
    if(argc!=4)throw std::runtime_error("Expected BC format, width and height");
    const int format=std::stoi(argv[1]),width=std::stoi(argv[2]),height=std::stoi(argv[3]);
    const int base=format>=10?format-10:format;const bool signedFormat=format>=10;
    if(base<1||base>7||width<=0||height<=0||width>16384||height>16384||uint64_t(width)*height>64u*1024*1024)throw std::runtime_error("Invalid texture block dimensions/format");
    if(signedFormat&&base!=4&&base!=5&&base!=6)throw std::runtime_error("Invalid signed block format");
    const size_t blocksX=(width+3)/4,blocksY=(height+3)/4,blockBytes=(base==1||base==4)?8:16;
    const bool hdr=base==6;const size_t pixelBytes=hdr?16:4,outputBytes=size_t(width)*height*pixelBytes;
    if(outputBytes>256u*1024*1024)throw std::runtime_error("Decoded texture exceeds 256 MiB");
#ifdef _WIN32
    _setmode(_fileno(stdin),_O_BINARY);_setmode(_fileno(stdout),_O_BINARY);
#endif
    std::vector<uint8_t> input(blocksX*blocksY*blockBytes),output(outputBytes);std::cin.read(reinterpret_cast<char*>(input.data()),input.size());
    if(size_t(std::cin.gcount())!=input.size()||std::cin.peek()!=EOF)throw std::runtime_error("Texture block payload size differs");
    for(size_t by=0;by<blocksY;by++)for(size_t bx=0;bx<blocksX;bx++){
        const auto* source=input.data()+(by*blocksX+bx)*blockBytes;alignas(16) uint8_t rgba[64]{};float rg[32]{},rgb[48]{};
        if(base==1)bcdec_bc1(source,rgba,16);else if(base==2)bcdec_bc2(source,rgba,16);else if(base==3)bcdec_bc3(source,rgba,16);
        else if(base==4)bcdec_bc4_float(source,rg,4,signedFormat);else if(base==5)bcdec_bc5_float(source,rg,8,signedFormat);
        else if(base==6)bcdec_bc6h_float(source,rgb,12,signedFormat);else bcdec_bc7(source,rgba,16);
        for(size_t y=0;y<4&&by*4+y<size_t(height);y++)for(size_t x=0;x<4&&bx*4+x<size_t(width);x++){
            const size_t p=y*4+x,destination=((by*4+y)*width+bx*4+x)*pixelBytes;
            if(hdr){float value[4]={rgb[p*3],rgb[p*3+1],rgb[p*3+2],1};for(float f:value)if(!std::isfinite(f))throw std::runtime_error("Nonfinite HDR block");std::copy_n(reinterpret_cast<uint8_t*>(value),16,output.data()+destination);}
            else if(base==4||base==5){const int channels=base==4?1:2;for(int c=0;c<channels;c++){const float f=rg[p*channels+c];output[destination+c]=static_cast<uint8_t>(std::clamp(std::lround((signedFormat?f*.5f+.5f:f)*255.f),0l,255l));}output[destination+3]=255;}
            else std::copy_n(rgba+p*4,4,output.data()+destination);
        }
    }
    std::cout.write(reinterpret_cast<const char*>(output.data()),output.size());if(!std::cout)throw std::runtime_error("Decoded output failed");return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
