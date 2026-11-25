#include "../engine/native/animation.hpp"
#include <iostream>
#include <cmath>

using namespace engine::animation;
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
bool near(float a,float b){return std::abs(a-b)<.0006f;}
V3 posedVertex(const Asset& asset,const PoseBuffer& pose,unsigned index){
  const auto& vertex=asset.vertices[index];V3 position{vertex.position[0],vertex.position[1],vertex.position[2]};
  for(unsigned m=0;m<asset.morphs.size();++m)for(const auto& delta:asset.morphs[m].deltas)if(delta.vertex==index){position.x+=delta.position.x*pose.curves[MorphCurveBase+m];position.y+=delta.position.y*pose.curves[MorphCurveBase+m];position.z+=delta.position.z*pose.curves[MorphCurveBase+m];}
  const auto palette=asset.skeleton.palette(pose);V3 result{};for(unsigned influence=0;influence<4;++influence){auto p=point(palette[vertex.joints[influence]],position);result.x+=p.x*vertex.weights[influence];result.y+=p.y*vertex.weights[influence];result.z+=p.z*vertex.weights[influence];}return result;
}
}
int main(int argc,char** argv){
  try{
    check(argc==3,"Expected reference and converted character headers");auto base=loadAsset(argv[1]),converted=loadAsset(argv[2]);
    check(base->skeleton.bones.size()==converted->skeleton.bones.size()&&base->vertices.size()==converted->vertices.size()&&base->clips.size()==converted->clips.size(),"Coordinate conversion changed character topology");
    check(base->indices==converted->indices,"Coordinate conversion changed expected triangle winding");
    for(unsigned bone=0;bone<base->skeleton.bones.size();++bone){const auto& a=base->skeleton.bones[bone];const auto& b=converted->skeleton.bones[bone];check(a.name==b.name&&a.parent==b.parent,"Bone identity changed");for(unsigned i=0;i<16;++i)check(near(a.inverseBind[i],b.inverseBind[i]),"Inverse bind transform was not converted with the skeleton");}
    for(unsigned clip=0;clip<base->clips.size();++clip)for(double time:{0.,.25,.5,.75,1.}){
      auto a=base->clips[clip].sample(time),b=converted->clips[clip].sample(time);for(unsigned index:{0u,25u,static_cast<unsigned>(base->vertices.size()-1)}){const auto pa=posedVertex(*base,a,index),pb=posedVertex(*converted,b,index);check(near(pa.x,pb.x)&&near(pa.y,pb.y)&&near(pa.z,pb.z),"Converted animated/morphed vertex moved away from reference");}
    }
    ControllerConfig config;config.clips=base->controllerClips;config.motionMatching=config.proceduralRig=config.muscles=config.softTissue=false;Controller a,b;a.initialize(base,config);b.initialize(converted,config);
    for(unsigned tick=1;tick<=240;++tick){Intent intent;intent.tick=tick;intent.grounded=true;intent.speed=tick<120?0:3;a.tick(intent);b.tick(intent);const auto pa=posedVertex(*base,a.pose(),25),pb=posedVertex(*converted,b.pose(),25);check(near(pa.x,pb.x)&&near(pa.y,pb.y)&&near(pa.z,pb.z),"Converted runtime controller diverged from reference");}
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<",\"scope\":\"unit/basis conversion, native inverse binds, posed morph vertices and controller\"}\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
