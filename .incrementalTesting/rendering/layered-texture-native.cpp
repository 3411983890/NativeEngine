#include "../../host/texture_asset.hpp"
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int wmain(int argc,wchar_t** argv) {
    if(argc!=3)return 2;
    std::vector<host::TextureAsset> textures;std::string error;
    const bool loaded=host::loadTextureDirectory(std::filesystem::path(argv[1]),textures,error);
    if(std::wstring(argv[2])==L"reject") {
        if(loaded){std::cerr<<"invalid sRGB data texture was accepted";return 1;}
        return 0;
    }
    if(!loaded){std::cerr<<error;return 1;}
    if(textures.size()!=3){std::cerr<<"expected three linear maps";return 1;}
    for(const auto& texture:textures) {
        const unsigned expected=texture.name=="map.height"?9:texture.name=="map.roughness"?10:texture.name=="map.metallic"?11:0;
        if(!expected||texture.semantic!=expected||texture.srgb||texture.width!=2||texture.height!=2||texture.mips!=2||texture.pixels.size()!=20){std::cerr<<"invalid data texture: "<<texture.name;return 1;}
    }
    return 0;
}
