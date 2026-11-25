#include "game.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace engine {
namespace {
float distanceSquaredXZ(Vec3 a,Vec3 b){return (a.x-b.x)*(a.x-b.x)+(a.z-b.z)*(a.z-b.z);}
Vec3 vector(const gameplay::Json& v){return {v[0],v[1],v[2]};}
bool differs(Vec3 a,Vec3 b){return a.x!=b.x||a.y!=b.y||a.z!=b.z;}
// Keep the body's identity and contact indices while editing its registered shape.
void configureBody(physics::Body& body,const gameplay::Json& transform,const gameplay::Json* rigid,
                   const gameplay::Json& published,Vec3& publishedVelocity,Vec3 localOffset={}) {
    const Vec3 scale=vector(transform.at("scale"));
    const Vec3 half{std::max(.001f,std::abs(scale.x)*.5f),std::max(.001f,std::abs(scale.y)*.5f),std::max(.001f,std::abs(scale.z)*.5f)};
    const bool resized=differs(body.half,half),turned=published.is_null()||published.at("yaw")!=transform.at("yaw");bool changed=resized;body.half=half;
    if(turned) {body.orientation=physics::fromAxisAngle({0,1,0},transform.at("yaw"));changed=true;}
    if(published.is_null()||resized||turned||published.at("position")!=transform.at("position")) {body.position=vector(transform.at("position"))+physics::rotate(body.orientation,localOffset);changed=true;}
    const bool dynamic=rigid&&rigid->at("dynamic").get<bool>();
    const float mass=rigid?rigid->at("mass").get<float>():body.mass;
    changed=changed||body.dynamic!=dynamic||body.mass!=mass;
    body.dynamic=dynamic;body.mass=mass;body.invMass=dynamic?1.f/mass:0.f;
    if(rigid) {
        body.friction=rigid->at("friction");body.restitution=rigid->at("restitution");
        const Vec3 velocity=vector(rigid->at("velocity"));
        if(differs(velocity,publishedVelocity)){body.velocity=velocity;publishedVelocity=velocity;changed=true;}
    }
    if(!dynamic){body.velocity={};body.angularVelocity={};}
    if(changed){body.sleeping=false;body.sleepTimer=0;}
    body.invInertiaLocal={};
    const float dimensions[3]={half.y*half.y+half.z*half.z,half.x*half.x+half.z*half.z,half.x*half.x+half.y*half.y};
    for(int i=0;i<3;++i)body.invInertiaLocal.m[i*4]=mass*dimensions[i]>3.e-9f?3.f/(mass*dimensions[i]):0.f;
    const auto rotation=physics::toMatrix(body.orientation);
    body.invInertiaWorld=dynamic?physics::multiply(physics::multiply(rotation,body.invInertiaLocal),physics::transpose(rotation)):physics::Mat3{};
}
}
std::string Game::playerEntityId() const {
    for (const auto& e : scene_.entities) if (e.kind == "player") return e.id;
    return "";
}
bool Game::entityColliderBounds(const std::string& id,physics::Aabb& bounds) const {
    const auto h=gameplay_.world.find(id);if(!gameplay_.world.valid(h))return false;const auto* collider=gameplay_.world.get(h,"collider");
    if(!collider||!collider->at("enabled").get<bool>()||id==playerEntityId())return false;
    if(const auto* destruction=gameplay_.world.get(h,"destruction");destruction&&destruction->at("fractured").get<bool>())return false;
    int index=-1;for(size_t i=0;i<scene_.entities.size();++i)if(scene_.entities[i].id==id){index=bodyIndexForEntity(static_cast<int>(i));break;}
    if(index<0){const auto found=runtimeBodies_.find(id);if(found!=runtimeBodies_.end())index=found->second;}
    if(index>=0)if(const auto* body=physics_.body(static_cast<unsigned>(index));body&&body->active){bounds=physics::worldBounds(*body);return true;}
    return false;
}
void Game::initializeGameplay() {
    const bool contentLocked=gameplay_.contentLocked();
    gameplay_.initialize(scene_.assetRoot, determinism::registry().seed());
    std::string door;
    for (const auto& e : scene_.entities) if (e.kind == "door") { door=e.id; break; }
    for (const auto& e : scene_.entities) {
        const auto h = gameplay_.world.create(e.id);
        gameplay_.world.set(h,"transform",{{"position",{e.position.x,e.position.y,e.position.z}},{"scale",{e.scale.x,e.scale.y,e.scale.z}}});
        gameplay_.world.set(h,"renderable",gameplay::Json::object());
        gameplay_.world.set(h,"collider",{{"enabled",e.collider}});
        if(e.destructible)gameplay_.world.set(h,"destruction",gameplay::Json::object());
        if(e.hasPhysics)gameplay_.world.set(h,"rigidbody",{{"dynamic",e.dynamicBody},{"mass",e.physicsMass},{"friction",e.friction},{"restitution",e.restitution}});
        if (e.kind == "player") {
            gameplay_.world.set(h,"inventory",gameplay::Json::object());
            gameplay_.world.set(h,"progress",gameplay::Json::object());
            gameplay_.world.set(h,"hud",gameplay::Json::object());
            gameplay_.world.set(h,"animator",gameplay::Json::object());
        }
        if (e.kind == "key") gameplay_.world.set(h,"interactable",{{"range",1.65},{"verticalRange",1.8},{"actorOffset",0.8}});
        if (e.kind == "door") gameplay_.world.set(h,"interactable",{{"range",2.2}});
        if (e.kind == "goal") gameplay_.world.set(h,"trigger",{{"half",{0.9,1.0,0.9}}});
        for (auto it=e.components.begin();it!=e.components.end();++it) gameplay_.world.set(h,it.key(),it.value());
    }
    for (const auto& e : scene_.entities) {
        if(e.components.contains("behavior")) {
            for(const auto& b:e.components["behavior"]["instances"]) gameplay_.attach(e.id,b.at("id"),b.at("script"),b.value("properties",gameplay::Json::object()));
        } else if(e.kind=="key") gameplay_.attach(e.id,"pickup","gameplay/pickup");
        else if(e.kind=="door") gameplay_.attach(e.id,"lock","gameplay/locked_door");
        else if(e.kind=="goal") gameplay_.attach(e.id,"goal","gameplay/goal",{{"requiredDoor",door}});
    }
    appliedAnimationRequest_=0;
    publishedVelocity_.clear();runtimeBodies_.clear();publishedTransforms_=gameplay::Json::object();
    gameplay_.start();
    gameplay_.lockContent(contentLocked);
    syncGameplay();
}
void Game::syncGameplay() {
    const auto actor=gameplay_.world.find(playerEntityId());
    if(gameplay_.world.valid(actor)) {
        if(const auto* inventory=gameplay_.world.get(actor,"inventory")) hasKey_=inventory->at("items").value("gold_key",0)>0;
        if(const auto* progress=gameplay_.world.get(actor,"progress")) complete_=progress->at("flags").value("complete",false);
        if(const auto* hud=gameplay_.world.get(actor,"hud")) {
            feedbackTime_=hud->at("remaining").get<float>()/120.f;
            feedback_=hud->at("text").get<std::string>();
        }
    }
    if(gameplay_.world.valid(actor))if(const auto* animator=gameplay_.world.get(actor,"animator")) {
        const auto request=animator->at("request").get<unsigned>();
        if(request!=appliedAnimationRequest_) {
            appliedAnimationRequest_=request;
            if(animation_.asset()&&!animator->at("clip").get<std::string>().empty())try {
                auto mask=animation::BoneMask::all(static_cast<unsigned>(animation_.asset()->skeleton.bones.size()));mask.bits[0]&=~1ull;
                animation_.play(animator->at("clip"),mask);
            }catch(const animation::Error& error){gameplay_.reportDiagnostic(error.code,error.what());}
        }
    }
    // Materialize any registered collider, including entities created by Lua.
    std::map<std::string,std::size_t> authoredIndices;
    for(std::size_t i=0;i<scene_.entities.size();++i)authoredIndices.emplace(scene_.entities[i].id,i);
    const auto playerId=playerEntityId();
    for(const auto& [id,index]:runtimeBodies_)if(!gameplay_.world.valid(gameplay_.world.find(id)))physics_.deactivate(static_cast<unsigned>(index));
    for(auto h:gameplay_.world.query()) {
        const std::string id=gameplay_.world.entity(h).id;
        const int sceneIndex=authoredIndices.count(id)?static_cast<int>(authoredIndices.at(id)):-1;
        const bool player=id==playerId;
        const auto* transform=gameplay_.world.get(h,"transform");const auto* collider=gameplay_.world.get(h,"collider");
        const bool enabled=transform&&collider&&collider->at("enabled").get<bool>()&&!player&&(sceneIndex<0||!fractured_[sceneIndex]);
        int index=sceneIndex<0?(runtimeBodies_.count(id)?runtimeBodies_.at(id):-1):entityBody_[sceneIndex];
        if(index<0&&enabled) {
            physics::BodyDesc desc;desc.visible=false;desc.entity=sceneIndex;
            index=static_cast<int>(physics_.addBody(desc));
            if(sceneIndex<0)runtimeBodies_[id]=index;else entityBody_[sceneIndex]=index;
        }
        auto* body=index<0?nullptr:physics_.body(static_cast<unsigned>(index));
        if(body)body->active=enabled;
        if(!transform)continue;
        const auto previous=publishedTransforms_.value(id,gameplay::Json());
        if(player) {
            if(!previous.is_null()&&previous.at("position")!=transform->at("position")){player_=vector(transform->at("position"));verticalVelocity_=0;}
            if(sceneIndex>=0)scene_.entities[sceneIndex].scale=vector(transform->at("scale"));
        } else if(sceneIndex>=0) {scene_.entities[sceneIndex].position=vector(transform->at("position"));scene_.entities[sceneIndex].scale=vector(transform->at("scale"));}
        if(body&&enabled)configureBody(*body,*transform,gameplay_.world.get(h,"rigidbody"),previous,publishedVelocity_[id],characterBodyOffset(sceneIndex));
        publishedTransforms_[id]=*transform;
    }
    for(std::size_t i=0;i<scene_.entities.size();++i)if(!gameplay_.world.valid(gameplay_.world.find(scene_.entities[i].id))) {
        const int index=entityBody_[i];if(index>=0)physics_.deactivate(static_cast<unsigned>(index));
    }
    doorOpen_=false;
    for(const auto& e:scene_.entities)if(e.kind=="door") {
        const auto h=gameplay_.world.find(e.id);
        if(gameplay_.world.valid(h))if(const auto* collider=gameplay_.world.get(h,"collider"))doorOpen_=!collider->at("enabled").get<bool>();
        break;
    }
}
void Game::publishGameplayPhysics() {
    for(std::size_t i=0;i<scene_.entities.size();++i)if(scene_.entities[i].destructible) {
        const auto h=gameplay_.world.find(scene_.entities[i].id);
        if(gameplay_.world.valid(h))gameplay_.world.set(h,"destruction",{{"fractured",static_cast<bool>(fractured_[i])},{"damage",damage_[i]}});
    }
    auto publish=[&](const std::string& id,int index,int sceneIndex=-1) {
        const auto h=gameplay_.world.find(id);const auto* body=index<0?nullptr:physics_.body(static_cast<unsigned>(index));
        if(!body||!body->dynamic||!body->active||!gameplay_.world.valid(h)||!gameplay_.world.get(h,"transform"))return;
        const auto anchor=body->position-characterBodyOffset(sceneIndex,body->orientation);
        auto transform=*gameplay_.world.get(h,"transform");transform["position"]={anchor.x,anchor.y,anchor.z};
        float yaw,pitch,roll;physics::toEuler(body->orientation,yaw,pitch,roll);transform["yaw"]=yaw;
        gameplay_.world.set(h,"transform",transform);publishedTransforms_[id]=transform;publishedVelocity_[id]=body->velocity;
        if(const auto* value=gameplay_.world.get(h,"rigidbody")){auto rigid=*value;rigid["velocity"]={body->velocity.x,body->velocity.y,body->velocity.z};gameplay_.world.set(h,"rigidbody",rigid);}
    };
    for(std::size_t i=0;i<scene_.entities.size();++i)publish(scene_.entities[i].id,entityBody_[i],static_cast<int>(i));
    for(const auto& [id,index]:runtimeBodies_)publish(id,index);
}
bool Game::firstPerson() const {
    for(const auto& e:scene_.entities)if(e.kind=="player"&&e.components.contains("camera"))return e.components["camera"].value("mode",std::string("thirdPerson"))=="firstPerson";
    return false;
}
float Game::worldEdge() const {
    for(const auto& e:scene_.entities)if(e.kind=="player"&&e.components.contains("camera"))return e.components["camera"].value("worldExtent",10.f);
    return 10.f;
}
float Game::cameraFov() const {
    for(const auto& e:scene_.entities)if(e.kind=="player"&&e.components.contains("camera"))return e.components["camera"].value("fov",firstPerson()?1.15f:1.05f);
    return firstPerson()?1.15f:1.05f;
}
Vec3 Game::cameraEye() const {
    float height=1.64f*playerHeight()/1.8f;
    for(const auto& e:scene_.entities)if(e.kind=="player"&&e.components.contains("camera"))height=e.components["camera"].value("eyeHeight",height);
    return {player_.x,player_.y+playerParameter("eyeHeight",height),player_.z};
}
Vec3 Game::cameraTarget() const {
    const auto eye=cameraEye();return {eye.x+std::sin(yaw_)*std::cos(pitch_),eye.y-std::sin(pitch_),eye.z-std::cos(yaw_)*std::cos(pitch_)};
}
std::string Game::interactionTarget() const {
    std::string target;float closest=std::numeric_limits<float>::max();
    const auto eye=cameraEye(),look=cameraTarget();
    for(auto h:gameplay_.world.query("interactable")) {
        const auto& interaction=*gameplay_.world.get(h,"interactable");
        const auto* transform=gameplay_.world.get(h,"transform");
        if(!transform||!interaction["enabled"].get<bool>())continue;
        const auto& p=transform->at("position");Vec3 position{p[0],p[1],p[2]};
        const float distance=distanceSquaredXZ(player_,position),range=interaction["range"];
        if(distance>range*range||std::abs(player_.y+playerParameter("interactionHeight",interaction["actorOffset"].get<float>()*playerHeight()/1.8f)-position.y)>=interaction["verticalRange"].get<float>())continue;
        float score=distance;
        if(firstPerson()) {
            const Vec3 delta{position.x-eye.x,position.y-eye.y,position.z-eye.z};
            const float length=std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
            const float aim=(delta.x*(look.x-eye.x)+delta.y*(look.y-eye.y)+delta.z*(look.z-eye.z))/std::max(.001f,length);
            if(aim<.82f)continue;
            bool blocked=false;
            for(const auto& e:scene_.entities)if(e.id!=gameplay_.world.entity(h).id&&e.kind!="player"&&solid(e)) {
        const auto eh=gameplay_.world.find(e.id);
                if(const auto* broken=gameplay_.world.get(eh,"destruction");broken&&broken->at("fractured").get<bool>())continue;
                const auto* t=gameplay_.world.get(eh,"transform");
                if(!t)continue;
                const auto& ep=t->at("position");const auto& es=t->at("scale");
                const int entityIndex=static_cast<int>(&e-scene_.entities.data()),bodyIndex=bodyIndexForEntity(entityIndex);
                const auto offset=characterBodyOffset(entityIndex);physics::Aabb bounds;
                bounds.min={ep[0].get<float>()+offset.x-es[0].get<float>()*.5f,ep[1].get<float>()+offset.y-es[1].get<float>()*.5f,ep[2].get<float>()+offset.z-es[2].get<float>()*.5f};
                bounds.max={ep[0].get<float>()+offset.x+es[0].get<float>()*.5f,ep[1].get<float>()+offset.y+es[1].get<float>()*.5f,ep[2].get<float>()+offset.z+es[2].get<float>()*.5f};
                if(bodyIndex>=0)if(const auto* body=physics_.body(static_cast<unsigned>(bodyIndex));body&&body->active)bounds=physics::worldBounds(*body);
                const float lowBounds[3]={bounds.min.x,bounds.min.y,bounds.min.z},highBounds[3]={bounds.max.x,bounds.max.y,bounds.max.z};
                float enter=0,leave=.97f;const float origin[3]={eye.x,eye.y,eye.z},direction[3]={delta.x,delta.y,delta.z};
                for(int axis=0;axis<3&&enter<=leave;++axis){
                    const float low=lowBounds[axis],high=highBounds[axis];
                    if(std::abs(direction[axis])<.00001f){if(origin[axis]<low||origin[axis]>high){enter=1;leave=0;}}
                    else {float a=(low-origin[axis])/direction[axis],b=(high-origin[axis])/direction[axis];if(a>b)std::swap(a,b);enter=std::max(enter,a);leave=std::min(leave,b);}
                }
                if(enter<=leave){blocked=true;break;}
            }
            if(blocked)continue;
            score=(1-aim)*10.f+distance*.02f;
        }
        if(score<closest){closest=score;target=gameplay_.world.entity(h).id;}
    }
    return target;
}
std::string Game::interactionPrompt() const {
    const auto id=interactionTarget();if(id.empty())return "";
    const auto* value=gameplay_.world.get(gameplay_.world.find(id),"interactable");
    return value?value->value("prompt",std::string("交互")):"";
}
void Game::interact() {
    const auto target=interactionTarget();
    if(!target.empty())gameplay_.interact(target,playerEntityId());
    syncGameplay();
}

}
