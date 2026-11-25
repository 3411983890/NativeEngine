#include "../../engine/native/material_layer_asset.hpp"
#include <fstream>
#include <iostream>
int main(int argc,char** argv){
    try {
        if(argc!=2)throw std::invalid_argument("Expected material file");
        std::ifstream file(argv[1]);nlohmann::json data;file>>data;
        auto layers=engine::material::layered::parseNormalized(data);
        nlohmann::json result=nlohmann::json::array();
        for(const auto& layer:layers){nlohmann::json slots=nlohmann::json::array();for(unsigned i=0;i<9;++i){const auto& s=layer.gpu.slots[i];slots.push_back({{"name",layer.textures[i]},{"uv",s.uvSet},{"sampler",s.sampler},{"channel",s.channel},{"row0",{s.row0[0],s.row0[1],s.row0[2]}},{"row1",{s.row1[0],s.row1[1],s.row1[2]}}});}result.push_back({{"id",layer.id},{"slots",slots},{"source",layer.gpu.source},{"flags",layer.gpu.flags},{"weight",layer.gpu.weight}});}
        std::cout<<result.dump();return 0;
    }catch(const std::exception& e){std::cerr<<e.what();return 1;}
}
