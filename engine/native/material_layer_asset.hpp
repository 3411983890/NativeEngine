#pragma once
#include "material_layer_gpu.hpp"
#include "../../third_party/json.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::material::layered {
// Input is the normalized nativegpu.layered-material/1 or .../2 authoring
// representation. Texture names remain symbolic until the host has resolved
// project resources.
struct LayerAsset { std::string id; GpuLayer gpu; std::array<std::string,9> textures; };

struct SurfaceReliefAsset {
    GpuSurfaceRelief gpu;
    std::string layer;   // height source layer id (source 0)
    std::string texture; // baked height texture name (source 1)
    bool hasComposite=false;
    unsigned resolution=2048;
    float groundMetres=16.0f;
    std::string baseLayer, overlayLayer, overlayMaskLayer;
};

struct MaterialAsset { std::vector<LayerAsset> layers; SurfaceReliefAsset relief; };

inline MaterialAsset parseNormalizedMaterial(const nlohmann::json& material) {
    using Json=nlohmann::json;
    const auto require=[](bool ok,const char* message){if(!ok)throw std::invalid_argument(message);};
    const auto keys=[&](const Json& value,std::initializer_list<const char*> allowed){require(value.is_object(),"Layer value must be an object");for(auto it=value.begin();it!=value.end();++it){bool known=false;for(auto key:allowed)if(it.key()==key){known=true;break;}require(known,"Unsupported layer field");}};
    const auto number=[&](const Json& value,float min,float max){require(value.is_number(),"Layer value must be numeric");const double n=value.get<double>();require(std::isfinite(n)&&n>=min&&n<=max,"Layer number out of range");return static_cast<float>(n);};
    const auto integer=[&](const Json& value,int min,int max){require(value.is_number_integer(),"Layer value must be an integer");const auto n=value.get<long long>();require(n>=min&&n<=max,"Layer integer out of range");return static_cast<unsigned>(n);};
    const auto name=[&](const Json& value){require(value.is_string(),"Layer identifier must be a string");auto s=value.get<std::string>();require(!s.empty()&&s.size()<=64&&s.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")==std::string::npos,"Invalid layer identifier");return s;};
    const auto channel=[&](const Json& v){require(v.is_string(),"Invalid layer channel");const std::string s=v.get<std::string>();const auto at=std::string("rgba").find(s);require(s.size()==1&&at!=std::string::npos,"Invalid layer channel");return static_cast<unsigned>(at);};
    const auto index=[&](const Json& value,const std::vector<std::string>& choices){require(value.is_string(),"Invalid layer option");const auto it=std::find(choices.begin(),choices.end(),value.get<std::string>());require(it!=choices.end(),"Unsupported layer option");return static_cast<unsigned>(it-choices.begin());};
    const auto mapping=[&](const Json& uv,GpuTexture& slot){
        keys(uv,{"set","mapping"});
        const auto& set=uv.at("set");require(set.is_number_integer(),"Invalid UV set");const int n=set.get<int>();require(n>=0&&n<=15,"UV set out of range");slot.uvSet=static_cast<unsigned>(n);
        const auto& m=uv.at("mapping");keys(m,{"offset","scale","rotation","wrapS","wrapT","magFilter","minFilter"});const auto& scale=m.at("scale");const auto& offset=m.at("offset");require(scale.is_array()&&scale.size()==2&&offset.is_array()&&offset.size()==2,"Invalid UV transform");
        for(const auto* component:{&scale[0],&scale[1],&offset[0],&offset[1],&m.at("rotation")})(void)number(*component,-1000000,1000000);
        const double x=scale[0].get<double>(),y=scale[1].get<double>(),ox=offset[0].get<double>(),oy=offset[1].get<double>(),rotation=m.at("rotation").get<double>();
        const double c=std::cos(rotation),s=std::sin(rotation);slot.row0[0]=static_cast<float>(c*x);slot.row0[1]=static_cast<float>(-s*y);slot.row0[2]=static_cast<float>(ox);slot.row1[0]=static_cast<float>(s*x);slot.row1[1]=static_cast<float>(c*y);slot.row1[2]=static_cast<float>(oy);
        const auto u=index(m.at("wrapS"),{"repeat","clamp","mirror"}),v=index(m.at("wrapT"),{"repeat","clamp","mirror"}),min=index(m.at("minFilter"),{"nearest","linear","nearest-mipmap-nearest","linear-mipmap-nearest","nearest-mipmap-linear","linear-mipmap-linear"}),mag=index(m.at("magFilter"),{"nearest","linear"});slot.sampler=1+(u*3+v)*12+min*2+mag;
    };

    require(material.is_object(),"Layered material must be an object");
    const std::string schema=material.value("schema",std::string{});
    require(schema=="nativegpu.layered-material/1"||schema=="nativegpu.layered-material/2","Unsupported layered material schema");
    const bool v2=schema=="nativegpu.layered-material/2";
    if(v2)keys(material,{"schema","name","layers","surfaceRelief"});else keys(material,{"schema","name","layers"});
    (void)name(material.at("name"));
    const auto& layers=material.at("layers");require(layers.is_array()&&!layers.empty()&&layers.size()<=4,"Layer count out of range");
    constexpr const char* names[]={"albedo","normal","orm","roughness","metallic","occlusion","emissive","height","mask"};
    std::set<std::string> ids;MaterialAsset result;
    for(const auto& value:layers){
        keys(value,{"id","name","enabled","uv","textures","baseColor","roughness","metallic","occlusionStrength","normalStrength","emissiveFactor","emissiveStrength","heightScale","heightOffset","blend"});
        LayerAsset out;out.id=name(value.at("id"));require(ids.insert(out.id).second,"Duplicate layer id");
        require(value.at("name").is_string(),"Invalid layer display name");const auto display=value.at("name").get<std::string>();require(!display.empty()&&display.size()<=128&&display.find_first_not_of(" \t\r\n")!=std::string::npos,"Invalid layer display name");GpuTexture defaultUv;mapping(value.at("uv"),defaultUv);
        require(value.at("enabled").is_boolean(),"Invalid layer enabled flag");const bool enabled=value.at("enabled").get<bool>();
        auto& gpu=out.gpu;const auto& textures=value.at("textures");keys(textures,{"albedo","normal","orm","roughness","metallic","occlusion","emissive","height"});
        require(!(textures.contains("orm")&&(textures.contains("roughness")||textures.contains("metallic"))),"Conflicting ORM and scalar maps");
        for(unsigned i=0;i<8;++i)if(textures.contains(names[i])){const auto& ref=textures.at(names[i]);keys(ref,{"name","uv","channel"});out.textures[i]=name(ref.at("name"));mapping(ref.at("uv"),gpu.slots[i]);if(ref.contains("channel"))gpu.slots[i].channel=channel(ref.at("channel"));}
        const auto& b=value.at("blend");keys(b,{"mode","source","weight","texture","channel","invert","gain","bias","width","heightBias"});gpu.source=index(b.at("source"),{"constant","textureMask","vertexColor"});
        gpu.slots[8].channel=channel(b.at("channel"));
        if(gpu.source==1){const auto& ref=b.at("texture");keys(ref,{"name","uv","channel"});out.textures[8]=name(ref.at("name"));mapping(ref.at("uv"),gpu.slots[8]);if(ref.contains("channel"))(void)channel(ref.at("channel"));}else require(!b.contains("texture"),"Unexpected blend texture");
        require(b.at("invert").is_boolean(),"Invalid layer inversion");gpu.flags=(b.at("invert").get<bool>()?1u:0u)|(index(b.at("mode"),{"mask","height"})?2u:0u)|(enabled?0u:4u);
        gpu.weight=number(b.at("weight"),0,1);gpu.gain=number(b.at("gain"),0,1000);gpu.bias=number(b.at("bias"),-1000,1000);gpu.width=number(b.at("width"),.0001f,1000);gpu.heightBias=number(b.at("heightBias"),-1000,1000);
        const auto copy3=[&](const Json& v,float* out){require(v.is_array()&&v.size()==3,"Invalid layer color");for(unsigned i=0;i<3;++i)out[i]=number(v[i],0,1);};
        copy3(value.at("baseColor"),gpu.baseColor);copy3(value.at("emissiveFactor"),gpu.emissiveFactor);
        gpu.roughness=number(value.at("roughness"),0,1);gpu.metallic=number(value.at("metallic"),0,1);gpu.occlusionStrength=number(value.at("occlusionStrength"),0,1);gpu.normalStrength=number(value.at("normalStrength"),0,8);gpu.emissiveStrength=number(value.at("emissiveStrength"),0,1000000);gpu.heightScale=number(value.at("heightScale"),-1000,1000);gpu.heightOffset=number(value.at("heightOffset"),-1000,1000);
        if(result.layers.empty())require(gpu.flags==0&&gpu.source==0&&gpu.weight==1&&gpu.gain==1&&gpu.bias==0,"Base layer must have full constant coverage");
        result.layers.push_back(std::move(out));
    }
    if(!v2)return result;

    const auto& reliefValue=material.at("surfaceRelief");
    keys(reliefValue,{"mode","heightSource","amplitudeMetres","referenceHeight","invert","minSteps","maxSteps","refineSteps","fadeStartMetres","fadeEndMetres","grazingFade","selfShadow","shadowSteps","displacementBake"});
    require(reliefValue.contains("displacementBake"),"surfaceRelief requires displacementBake");
    auto& relief=result.relief;auto& gpu=relief.gpu;
    gpu.flags=index(reliefValue.at("mode"),{"off","pom","bakedDisplacement"});
    require(reliefValue.at("invert").is_boolean(),"Invalid relief inversion");if(reliefValue.at("invert").get<bool>())gpu.flags|=1u<<2;
    require(reliefValue.at("selfShadow").is_boolean(),"Invalid relief selfShadow");if(reliefValue.at("selfShadow").get<bool>())gpu.flags|=1u<<3;
    const auto& source=reliefValue.at("heightSource");keys(source,{"kind","layer","texture"});
    const std::string kind=source.value("kind",std::string{"layer"});
    if(kind=="layer"){
        require(!source.contains("texture"),"Layer height source cannot carry a texture");relief.layer=name(source.at("layer"));
        require(ids.count(relief.layer)!=0,"Relief height layer must exist");gpu.heightLayer=static_cast<unsigned>(std::distance(result.layers.begin(),std::find_if(result.layers.begin(),result.layers.end(),[&](const auto& item){return item.id==relief.layer;})));
    }else if(kind=="texture"){
        require(!source.contains("layer"),"Texture height source cannot carry a layer");const auto& ref=source.at("texture");keys(ref,{"name","uv","channel"});
        relief.texture=name(ref.at("name"));GpuTexture textureSlot;mapping(ref.at("uv"),textureSlot);
        std::copy(textureSlot.row0,textureSlot.row0+3,gpu.row0);std::copy(textureSlot.row1,textureSlot.row1+3,gpu.row1);
        gpu.sampler=textureSlot.sampler;gpu.uvSet=textureSlot.uvSet;gpu.channel=textureSlot.channel;
        if(ref.contains("channel"))gpu.channel=channel(ref.at("channel"));
        gpu.flags|=1u<<4;
    }else throw std::invalid_argument("Unsupported relief height source");
    gpu.amplitudeMetres=number(reliefValue.at("amplitudeMetres"),0,1);
    gpu.referenceHeight=number(reliefValue.at("referenceHeight"),0,1);
    gpu.minSteps=static_cast<float>(integer(reliefValue.at("minSteps"),1,128));
    gpu.maxSteps=static_cast<float>(integer(reliefValue.at("maxSteps"),1,256));
    require(gpu.maxSteps>=gpu.minSteps,"Relief maxSteps must be at least minSteps");
    gpu.refineSteps=static_cast<float>(integer(reliefValue.at("refineSteps"),0,8));
    gpu.fadeStartMetres=number(reliefValue.at("fadeStartMetres"),0,1000);
    gpu.fadeEndMetres=number(reliefValue.at("fadeEndMetres"),0,1000);
    require(gpu.fadeEndMetres>=gpu.fadeStartMetres,"Relief fade end must be at least fade start");
    gpu.grazingFade=number(reliefValue.at("grazingFade"),0,1);
    gpu.shadowSteps=static_cast<float>(integer(reliefValue.at("shadowSteps"),1,32));

    const auto& bake=reliefValue.at("displacementBake");
    keys(bake,{"tileMetres","cellsPerTile","lodLevels","maxErrorMetres","composite"});
    (void)number(bake.at("tileMetres"),.25f,64);(void)integer(bake.at("cellsPerTile"),4,256);(void)integer(bake.at("lodLevels"),1,8);(void)number(bake.at("maxErrorMetres"),0,.25);
    if(bake.contains("composite")&&!bake.at("composite").is_null()){
        const auto& composite=bake.at("composite");keys(composite,{"resolution","groundMetres","baseLayer","overlayLayer","overlayMaskLayer"});
        relief.hasComposite=true;relief.resolution=integer(composite.at("resolution"),16,8192);relief.groundMetres=number(composite.at("groundMetres"),.25f,4096);
        relief.baseLayer=name(composite.at("baseLayer"));relief.overlayLayer=name(composite.at("overlayLayer"));
        require(ids.count(relief.baseLayer)!=0&&ids.count(relief.overlayLayer)!=0,"Composite layers must exist");
        if(composite.contains("overlayMaskLayer")&&!composite.at("overlayMaskLayer").is_null()){relief.overlayMaskLayer=name(composite.at("overlayMaskLayer"));require(ids.count(relief.overlayMaskLayer)!=0,"Composite mask layer must exist");}
    }
    return result;
}

inline std::vector<LayerAsset> parseNormalized(const nlohmann::json& material) {
    return parseNormalizedMaterial(material).layers;
}
}
