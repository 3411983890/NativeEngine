#include "../engine/native/animation.hpp"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

using namespace engine::animation;
namespace {
unsigned checks=0;
void check(bool value,const char* message) {
  ++checks;if(!value)throw std::runtime_error(message);
}
bool near(float a,float b){return std::abs(a-b)<.0002f;}
std::uint64_t digest(const Controller& controller) {
  engine::determinism::Hasher hash;controller.digest(hash);return hash.value();
}
}
int main(int argc,char** argv) {
  try {
    check(argc==3||argc==4,"Expected character header and isolated negative-case directory");
    const std::filesystem::path header=argv[1],scratch=argv[2];
    auto asset=loadAsset(header);
    check(asset->meshParts.size()==2&&asset->skeleton.bones.size()==32,"Multi-skin material ranges were lost");
    check(asset->morphs.size()==2,"Per-node morph targets were merged");
    auto clip=asset->clips[0];clip.loop=false;
    auto start=clip.sample(0),middle=clip.sample(.5),end=clip.sample(1);
    check(near(start.curves[MorphCurveBase],0)&&near(middle.curves[MorphCurveBase],.5f)&&near(end.curves[MorphCurveBase],1),"Morph interpolation and endpoints");
    check(near(middle.curves[MorphCurveBase+1],.2f),"Unanimated mesh default weight");
    clip.stepMorphMask=1;
    check(near(clip.sample(.5+.5/60).curves[MorphCurveBase],.5f),"Morph STEP hold between baked frames");
    auto mixed=blend(start,end,unsigned(asset->skeleton.bones.size()),.25f,BoneMask::all(unsigned(asset->skeleton.bones.size())));
    check(near(mixed.curves[MorphCurveBase],.25f),"Morph blending");
    ControllerConfig config;config.clips=asset->controllerClips;
    config.motionMatching=config.proceduralRig=config.muscles=config.softTissue=false;
    Controller a,b;a.initialize(asset,config);b.initialize(asset,config);
    Intent intent;intent.grounded=true;
    for(unsigned i=0;i<60;++i){intent.tick=i+1;a.tick(intent);b.tick(intent);check(digest(a)==digest(b),"Morph controller determinism");}
    check(near(a.featureState().morphWeights[0],.5f)&&near(a.featureState().morphWeights[1],.2f),"Authored weights must survive disabled procedural deformation");
    auto rigid=std::make_shared<Asset>(*asset);rigid->preserveRootTranslation=true;
    for(auto& clip:rigid->clips)for(unsigned f=0;f<clip.frameCount;++f)clip.samples[f*clip.boneCount].translation.y=float(f)/(clip.frameCount-1);
    Controller prop;prop.initialize(rigid,config);for(unsigned i=0;i<60;++i){intent.tick=i+1;prop.tick(intent);}
    check(near(prop.pose().translation[0][1],.5f)&&near(prop.rootMotion().y,0),"Rigid object root animation must stay in the rendered pose");
    rigid->preserveRootTranslation=false;Controller extracted;extracted.initialize(rigid,config);
    for(unsigned i=0;i<60;++i){intent.tick=i+1;extracted.tick(intent);}
    check(near(extracted.pose().translation[0][1],0)&&extracted.rootMotion().y>0,"Character root extraction must remain available");
    if(argc==4) {
      const bool bvh=std::string(argv[3])=="bvh";
      const auto& added=asset->clips.back();check(added.name==(bvh?"walk_Motion":"vendor_Raise"),"Standalone motion clip must be appended");
      const auto boneName=bvh?"chest":"left_arm";
      auto pose=added.sample(.5);const auto arm=std::find_if(asset->skeleton.bones.begin(),asset->skeleton.bones.end(),[&](const Bone& b){return b.name==boneName;});
      check(arm!=asset->skeleton.bones.end(),"Retargeted target arm missing");const auto index=unsigned(arm-asset->skeleton.bones.begin());
      check(near(pose.translation[0][0],.25f)&&near(pose.rotation[index][2],.3827f),"Retargeted units and rotation did not survive native loading");
      ControllerConfig action=config;action.clips.fill(added.name);Controller playback;playback.initialize(asset,action);
      for(unsigned tick=1;tick<=60;++tick){intent.tick=tick;playback.tick(intent);}
      check(near(playback.pose().rotation[index][2],.3827f)&&playback.rootMotion().x>0,"Controller did not execute appended action");
      check(near(playback.featureState().morphWeights[0],.2f)&&near(playback.featureState().morphWeights[1],.2f),"Appended action must retain target default shapes");
      if(bvh) {
        const auto hip=std::find_if(asset->skeleton.bones.begin(),asset->skeleton.bones.end(),[](const Bone& b){return b.name=="hips";});
        check(hip!=asset->skeleton.bones.end(),"BVH carrier hip missing");const auto index=unsigned(hip-asset->skeleton.bones.begin());
        check(near(pose.translation[index][0],0)&&near(pose.translation[index][1],.82f),"BVH offsets and in-place hip translation");
        check(near(playback.pose().translation[0][0],0)&&playback.rootMotion().x>0,"BVH root displacement must extract separately from the rendered pose");
      }
    }
    auto malformed=std::make_shared<Asset>(*asset);malformed->clips[0].morphSamples.pop_back();
    try{malformed->validate();throw std::runtime_error("Truncated morph samples accepted");}catch(const Error& e){check(e.code=="NG-E-ANIM-CLIP","Morph sample rejection code");}
    std::filesystem::create_directories(scratch);
    std::filesystem::copy_file(header.parent_path()/"animation.bin",scratch/"animation.bin",std::filesystem::copy_options::overwrite_existing);
    nlohmann::json data;std::ifstream(header)>>data;
    auto reject=[&](const nlohmann::json& value) {
      {std::ofstream file(scratch/"character.json");file<<value.dump();}
      try{loadAsset(scratch/"character.json");throw std::runtime_error("Malformed header accepted");}
      catch(const Error&){++checks;}
    };
    auto bad=data;bad["clips"][0]["morphOffset"]=0;reject(bad);
    bad=data;bad["clips"][0]["morphTargets"]=3;reject(bad);
    bad=data;bad["mesh"]["parts"][1]["firstIndex"]=0;reject(bad);
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<",\"scope\":\"external character, morph curves, material ranges and deterministic controller\"}\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
