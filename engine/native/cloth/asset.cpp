#include "asset.hpp"
#include "triangle_math.hpp"
#include "../assets/sha256.hpp"
#include "../../../third_party/json.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>

namespace engine::cloth {
using Json = nlohmann::json;
Error::Error(std::string value, std::string message) : std::runtime_error(std::move(message)), code(std::move(value)) {}

namespace {
void require(bool ok, const std::string& message) { if (!ok) throw Error("NG-E-CLOTH-ASSET", message); }
void keys(const Json& value, std::initializer_list<const char*> required,
          std::initializer_list<const char*> optional = {}) {
    require(value.is_object(), "Expected cloth object");
    std::set<std::string> allowed;
    for (auto key : required) { allowed.insert(key); require(value.contains(key), std::string("Missing cloth field: ") + key); }
    for (auto key : optional) allowed.insert(key);
    for (const auto& item : value.items()) require(allowed.count(item.key()) != 0, "Unsupported cloth field: " + item.key());
}
double scalar(const Json& value, double lo, double hi) {
    require(value.is_number(), "Cloth scalar must be numeric");
    const auto result = value.get<double>();
    require(std::isfinite(result) && result >= lo && result <= hi, "Cloth scalar outside range");
    return result;
}
std::uint32_t integer(const Json& value, std::uint32_t lo, std::uint32_t hi) {
    const auto result = scalar(value, lo, hi);
    require(std::floor(result) == result, "Cloth integer has a fractional part");
    return static_cast<std::uint32_t>(result);
}
std::string text(const Json& value) { require(value.is_string(), "Expected cloth string"); return value.get<std::string>(); }
std::string id(const Json& value) {
    auto result = text(value);
    require(!result.empty() && result.size() <= 64 && result[0] >= 'a' && result[0] <= 'z', "Invalid cloth identifier");
    for (char c : result) require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-', "Invalid cloth identifier");
    return result;
}
void array(const Json& value, std::size_t min, std::size_t max) {
    require(value.is_array() && value.size() >= min && value.size() <= max, "Invalid cloth array length");
}
Vec3 vector(const Json& value, double lo = -100000, double hi = 100000) {
    array(value, 3, 3);
    return {static_cast<float>(scalar(value[0], lo, hi)), static_cast<float>(scalar(value[1], lo, hi)),
            static_cast<float>(scalar(value[2], lo, hi))};
}
std::string read(const std::filesystem::path& path, std::size_t maximum = 32 * 1024 * 1024) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    require(!error && size <= maximum, "Cloth input missing or too large: " + path.u8string());
    std::ifstream stream(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(size));
    require(stream.gcount() == static_cast<std::streamsize>(size) && stream.peek() == std::char_traits<char>::eof(),
            "Cloth input changed during read");
    return bytes;
}
bool hashText(const std::string& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
Json authenticated(const std::string& bytes, const char* schema, const char* hashKey = "hash") {
    require(bytes.size() <= 32 * 1024 * 1024, "Cloth container too large");
    auto outer = Json::parse(bytes);
    require(outer.is_object() && outer.value("schema", std::string()) == schema && outer.contains("payloadJson"),
            "Unsupported cloth cook schema");
    const auto payload = text(outer.at("payloadJson"));
    require(hashText(text(outer.at(hashKey))) && assets::sha256(payload) == text(outer.at(hashKey)),
            "Cloth payload SHA-256 mismatch");
    auto parsed = Json::parse(payload);
    require(parsed.is_object() && parsed.value("schema", std::string()) == schema, "Cloth payload schema mismatch");
    return parsed;
}
std::vector<DistanceConstraint> constraints(const Json& values, std::size_t vertexCount) {
    require(values.is_array() && values.size() <= 2000000, "Invalid cloth constraint array");
    std::vector<DistanceConstraint> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        array(value, 3, 3);
        DistanceConstraint constraint;
        constraint.a = integer(value[0], 0, static_cast<std::uint32_t>(vertexCount - 1));
        constraint.b = integer(value[1], 0, static_cast<std::uint32_t>(vertexCount - 1));
        constraint.rest = static_cast<float>(scalar(value[2], 1e-6, 1000));
        require(constraint.a != constraint.b, "Cloth constraint connects a vertex to itself");
        result.push_back(constraint);
    }
    return result;
}
std::array<float, 16> matrix16(const Json& value) {
    array(value, 16, 16);
    std::array<float, 16> result{};
    for (unsigned index = 0; index < 16; ++index) result[index] = static_cast<float>(scalar(value[index], -1e6, 1e6));
    require(std::fabs(result[3]) < 1e-5f && std::fabs(result[7]) < 1e-5f && std::fabs(result[11]) < 1e-5f
            && std::fabs(result[15] - 1.0f) < 1e-5f, "Bone matrix is not affine");
    return result;
}
std::vector<float> channel(const Json& values, std::size_t count, double low, double high, const char* label) {
    array(values, count, count);
    std::vector<float> result;
    result.reserve(count);
    for (const auto& value : values) {
        const auto entry = static_cast<float>(scalar(value, low, high));
        require(std::isfinite(entry), std::string("Non-finite ") + label);
        result.push_back(entry);
    }
    return result;
}
}

void validateTriangleTopology(const Asset& a){
    require(a.triangleModel&&a.rest.size()>=4&&a.rest.size()<=16384&&a.membranes.size()==a.triangles.size()&&a.adjacency.size()==a.rest.size(),"Missing triangle solver topology");
    // A grid's triangle count is a function of its dimensions. A garment mesh has no such
    // relation, so the count is checked against the declared mesh instead of the grid.
    if(a.garment)require(a.triangles.size()>0&&a.triangles.size()==a.membranes.size(),"Garment triangle topology is incomplete");
    else require(a.triangles.size()==2ull*(a.columns-1)*(a.rows-1),"Triangle topology disagrees with grid dimensions");
    struct Edge {std::uint32_t a,b,opposite;};std::map<std::pair<unsigned,unsigned>,std::vector<Edge>> edges;std::set<std::array<unsigned,3>> faces;
    std::vector<std::vector<unsigned>> adjacency(a.rest.size());
    for(unsigned id=0;id<a.triangles.size();++id){const auto& t=a.triangles[id];for(auto v:t)require(v<a.rest.size(),"Triangle index outside cloth");auto key=t;std::sort(key.begin(),key.end());require(key[0]!=key[1]&&key[1]!=key[2]&&faces.insert(key).second,"Repeated or degenerate face");
        const auto e0=a.rest[t[1]]-a.rest[t[0]],e1=a.rest[t[2]]-a.rest[t[0]];const auto& m=a.membranes[id];const float area=.5f*detail::length3(detail::cross3(e0,e1));require(std::isfinite(area)&&area>1e-12f&&std::isfinite(m.area)&&std::fabs(m.area-area)<=area*1e-4f,"Invalid membrane rest area");
        for(auto v:m.inverse)require(std::isfinite(v)&&std::fabs(v)<=1e8f,"Invalid rest inverse matrix");const auto u=e0*m.inverse[0]+e1*m.inverse[1],v=e0*m.inverse[2]+e1*m.inverse[3];require(std::fabs(detail::dot3(u,u)-1)<1e-3f&&std::fabs(detail::dot3(v,v)-1)<1e-3f&&std::fabs(detail::dot3(u,v))<1e-3f,"Rest inverse does not encode orthogonal unit material directions");
        for(unsigned i=0;i<3;++i){adjacency[t[i]].push_back(id);unsigned x=t[i],y=t[(i+1)%3];auto& list=edges[{std::min(x,y),std::max(x,y)}];list.push_back({x,y,t[(i+2)%3]});require(list.size()<=2,"Non-manifold cloth edge");}
    }
    require(adjacency==a.adjacency,"Cloth normal adjacency does not match its faces");for(const auto& item:edges)if(item.second.size()==2)require(item.second[0].a==item.second[1].b&&item.second[0].b==item.second[1].a,"Inconsistent face orientation");
    std::set<std::pair<unsigned,unsigned>> hinged;for(const auto& h:a.hinges){auto vertices=h.vertices;std::sort(vertices.begin(),vertices.end());require(std::adjacent_find(vertices.begin(),vertices.end())==vertices.end(),"Repeated hinge vertex");for(auto id:h.vertices)require(id<a.rest.size(),"Hinge index outside cloth");
        const auto edge=std::make_pair(std::min(h.vertices[0],h.vertices[1]),std::max(h.vertices[0],h.vertices[1]));auto found=edges.find(edge);require(found!=edges.end()&&found->second.size()==2&&hinged.insert(edge).second,"Invalid or repeated hinge edge");const auto& list=found->second;require((list[0].opposite==h.vertices[2]&&list[1].opposite==h.vertices[3])||(list[1].opposite==h.vertices[2]&&list[0].opposite==h.vertices[3]),"Hinge opposite vertices disagree with faces");
        float angle;std::array<Vec3,4> gradient;std::array<Vec3,4> p;for(unsigned i=0;i<4;++i)p[i]=a.rest[h.vertices[i]];require(detail::dihedral(p,angle,gradient)&&std::isfinite(h.angle)&&std::fabs(detail::angleDifference(angle,h.angle))<1e-4f,"Invalid rest dihedral angle");}
    std::size_t expected=0;for(const auto& item:edges)expected+=item.second.size()==2;require(hinged.size()==expected,"Missing interior hinge");
    const auto colors=[&](const auto& batches,std::size_t count,const auto& vertices){require(count?(!batches.empty()&&batches.size()<=128):batches.empty(),"Invalid cloth color count");std::vector<bool> seen(count,false);for(const auto& batch:batches){require(!batch.empty(),"Empty cloth color");std::vector<bool> used(a.rest.size(),false);for(auto id:batch){require(id<count&&!seen[id],"Repeated or out-of-range color entry");seen[id]=true;for(auto v:vertices(id)){require(!used[v],"Cloth color has overlapping vertex access");used[v]=true;}}}require(std::all_of(seen.begin(),seen.end(),[](bool v){return v;}),"Incomplete cloth coloring");};
    colors(a.membraneColors,a.triangles.size(),[&](unsigned id){return a.triangles[id];});colors(a.hingeColors,a.hinges.size(),[&](unsigned id){return a.hinges[id].vertices;});
}

// Cooked garment: an arbitrary triangle mesh plus the skinning contract that drives it.
// Split from parseCookedCloth because the /1 and /2 documents are grids, where columns and
// rows define the mesh; a garment's identity comes from its baked arrays and its skeleton.
std::shared_ptr<const Asset> parseGarment(const std::string& bytes) {
    const auto payload = authenticated(bytes, "nativegpu.cloth-cook/3");
    const auto& source = payload.at("source");
    keys(source, {"schema", "id", "revision", "seed", "kind", "mesh", "material", "collision", "bones",
                  "skin", "physics", "proxies", "poses", "budget", "baked"});
    require(text(source.at("schema")) == "nativegpu.garment/1", "Unsupported garment source schema");
    require(text(source.at("kind")) == "garment", "A garment document must declare kind=garment");
    auto asset = std::make_shared<Asset>();
    asset->garment = true;
    asset->triangleModel = true;
    asset->skinned = true;
    asset->id = id(source.at("id"));
    asset->hash = assets::sha256(text(Json::parse(bytes).at("payloadJson")));
    asset->revision = integer(source.at("revision"), 0, 0xFFFFFFFFu);
    asset->seed = integer(source.at("seed"), 0, 0xFFFFFFFFu);

    const auto& mesh = source.at("mesh");
    keys(mesh, {"type", "vertexCount", "triangleCount"});
    require(text(mesh.at("type")) == "mesh", "Garment mesh type must be mesh");
    const auto declaredVertices = integer(mesh.at("vertexCount"), 4, 16384);
    const auto declaredTriangles = integer(mesh.at("triangleCount"), 2, 2000000);

    const auto& material = source.at("material");
    keys(material, {"arealDensityKgM2", "thicknessMetres", "stretchCompliance", "shearCompliance",
                    "bendAngleCompliance", "damping", "dragCoefficient"});
    asset->material.arealDensityKgM2 = static_cast<float>(scalar(material.at("arealDensityKgM2"), 0.001, 50));
    asset->material.thicknessMetres = static_cast<float>(scalar(material.at("thicknessMetres"), 0.0001, 0.2));
    asset->material.stretchCompliance = static_cast<float>(scalar(material.at("stretchCompliance"), 0, 1));
    asset->material.shearCompliance = static_cast<float>(scalar(material.at("shearCompliance"), 0, 1));
    asset->material.bendAngleCompliance = static_cast<float>(scalar(material.at("bendAngleCompliance"), 0, 1e8));
    asset->material.damping = static_cast<float>(scalar(material.at("damping"), 0, 50));
    asset->material.dragCoefficient = static_cast<float>(scalar(material.at("dragCoefficient"), 0, 10));

    const auto& collision = source.at("collision");
    keys(collision, {"thicknessMetres", "plane", "spheres", "capsules"});
    asset->collision.thicknessMetres = static_cast<float>(scalar(collision.at("thicknessMetres"), 0, 0.5));
    require(collision.at("plane").is_null(), "A garment takes its collision from the skeleton, not from a ground plane");
    array(collision.at("spheres"), 0, 0);
    array(collision.at("capsules"), 0, 0);

    const auto& budget = source.at("budget");
    keys(budget, {"maxVertices", "maxSubsteps", "iterations"});
    const auto maxVertices = integer(budget.at("maxVertices"), 4, 65536);
    asset->maxSubsteps = integer(budget.at("maxSubsteps"), 1, 16);
    asset->iterations = integer(budget.at("iterations"), 1, 64);

    // Baked geometry first: the skin and physics channels are sized by the real vertex count,
    // which must agree with the declaration rather than define it.
    const auto& baked = source.at("baked");
    keys(baked, {"positions", "triangles", "masses", "edges", "totalAreaMetres2", "topology"});
    array(baked.at("positions"), 4, maxVertices);
    require(baked.at("positions").size() == declaredVertices, "Baked vertex count does not match the mesh declaration");
    require(declaredVertices <= 16384, "Garment simulation mesh exceeds the 16384 vertex solver bound");
    for (const auto& position : baked.at("positions")) asset->rest.push_back(vector(position));
    array(baked.at("masses"), asset->rest.size(), asset->rest.size());
    float totalMass = 0;
    for (const auto& mass : baked.at("masses")) {
        const auto value = static_cast<float>(scalar(mass, 1e-6, 1000));
        asset->masses.push_back(value);
        totalMass += value;
    }
    require(totalMass > 0 && std::isfinite(totalMass), "Baked garment mass is invalid");
    asset->areas.resize(asset->rest.size());
    for (std::size_t index = 0; index < asset->areas.size(); ++index)
        asset->areas[index] = asset->masses[index] / asset->material.arealDensityKgM2;

    array(baked.at("triangles"), 2, 2000000);
    require(baked.at("triangles").size() == declaredTriangles, "Baked triangle count does not match the mesh declaration");
    for (const auto& triangle : baked.at("triangles")) {
        array(triangle, 3, 3);
        std::array<std::uint32_t, 3> indices{};
        for (unsigned i = 0; i < 3; ++i) indices[i] = integer(triangle[i], 0, static_cast<std::uint32_t>(asset->rest.size() - 1));
        require(indices[0] != indices[1] && indices[1] != indices[2] && indices[0] != indices[2], "Degenerate garment triangle");
        asset->triangles.push_back(indices);
    }
    asset->edges = constraints(baked.at("edges"), asset->rest.size());
    require(asset->edges.size() >= asset->triangles.size(), "Garment needs one rest edge per triangle at minimum");
    asset->totalAreaMetres2 = static_cast<float>(scalar(baked.at("totalAreaMetres2"), 1e-6, 1e6));

    array(source.at("bones"), 1, 256);
    for (const auto& bone : source.at("bones")) {
        keys(bone, {"name", "inverseBind"});
        Bone entry;
        entry.name = text(bone.at("name"));
        require(!entry.name.empty() && entry.name.size() <= 128, "Invalid garment bone name");
        entry.inverseBind = matrix16(bone.at("inverseBind"));
        asset->bones.push_back(std::move(entry));
    }
    const auto boneCount = static_cast<std::uint32_t>(asset->bones.size());

    const auto& skin = source.at("skin");
    keys(skin, {"maximumInfluences", "joints", "weights"});
    asset->influences = integer(skin.at("maximumInfluences"), 1, 4);
    require(asset->influences == 4, "Native garment skinning uses exactly four influences per vertex");
    array(skin.at("joints"), asset->rest.size() * 4, asset->rest.size() * 4);
    array(skin.at("weights"), asset->rest.size() * 4, asset->rest.size() * 4);
    for (std::size_t index = 0; index < skin.at("joints").size(); ++index)
        require(integer(skin.at("joints").at(index), 0, boneCount - 1) < boneCount, "Skin joint outside the skeleton");
    asset->skinJoints.resize(asset->rest.size());
    asset->skinWeights.resize(asset->rest.size());
    for (std::size_t vertex = 0; vertex < asset->rest.size(); ++vertex) {
        float sum = 0;
        for (unsigned slot = 0; slot < 4; ++slot) {
            const auto weight = static_cast<float>(scalar(skin.at("weights").at(vertex * 4 + slot), 0, 1));
            asset->skinWeights[vertex][slot] = weight;
            asset->skinJoints[vertex][slot] = integer(skin.at("joints").at(vertex * 4 + slot), 0, boneCount - 1);
            sum += weight;
        }
        require(std::fabs(sum - 1.0f) <= 2e-3f, "Garment vertex weights are not normalized");
    }

    const auto& physics = source.at("physics");
    keys(physics, {"blend", "maxDisplacement", "thickness", "anchor", "mass", "strainLimit", "maxSpeed"});
    const auto vertices = asset->rest.size();
    asset->blend = channel(physics.at("blend"), vertices, 0, 1, "blend");
    asset->maxDisplacement = channel(physics.at("maxDisplacement"), vertices, 0, 0.5, "maxDisplacement");
    asset->thickness = channel(physics.at("thickness"), vertices, 0, 0.1, "thickness");
    asset->anchor = channel(physics.at("anchor"), vertices, 0, 1, "anchor");
    const auto masses = channel(physics.at("mass"), vertices, 1e-5, 5, "mass");
    for (std::size_t index = 0; index < vertices; ++index)
        require(std::fabs(masses[index] - asset->masses[index]) <= 1e-6f,
                "physics.mass disagrees with the baked inverse mass");
    asset->strainLimit = static_cast<float>(scalar(physics.at("strainLimit"), 1, 3));
    asset->maxSpeed = static_cast<float>(scalar(physics.at("maxSpeed"), 0, 50));

    const auto& proxies = source.at("proxies");
    keys(proxies, {"spheres", "capsules"}, {"ellipsoids"});
    array(proxies.at("spheres"), 0, 64);
    for (const auto& sphere : proxies.at("spheres")) {
        keys(sphere, {"bone", "centre", "radiusMetres"});
        BoneSphere entry;
        entry.bone = integer(sphere.at("bone"), 0, boneCount - 1);
        entry.centre = vector(sphere.at("centre"));
        entry.radius = static_cast<float>(scalar(sphere.at("radiusMetres"), 0.001, 1000));
        asset->boneSpheres.push_back(entry);
    }
    array(proxies.at("capsules"), 0, 64);
    for (const auto& capsule : proxies.at("capsules")) {
        keys(capsule, {"bone", "a", "b", "radiusMetres"});
        BoneCapsule entry;
        entry.bone = integer(capsule.at("bone"), 0, boneCount - 1);
        entry.a = vector(capsule.at("a"));
        entry.b = vector(capsule.at("b"));
        entry.radius = static_cast<float>(scalar(capsule.at("radiusMetres"), 0.001, 1000));
        asset->boneCapsules.push_back(entry);
    }
    if (proxies.contains("ellipsoids")) {
        array(proxies.at("ellipsoids"), 0, 64);
        for (const auto& ellipsoid : proxies.at("ellipsoids")) {
            keys(ellipsoid, {"bone", "centre", "radii"});
            BoneEllipsoid entry;
            entry.bone = integer(ellipsoid.at("bone"), 0, boneCount - 1);
            entry.centre = vector(ellipsoid.at("centre"));
            entry.radii = vector(ellipsoid.at("radii"), 0.001, 1000);
            asset->boneEllipsoids.push_back(entry);
        }
    }
    require(!asset->boneSpheres.empty() || !asset->boneCapsules.empty() || !asset->boneEllipsoids.empty(),
            "A garment without skeleton collision proxies would report a clean pass by construction");

    array(source.at("poses"), 1, 64);
    for (const auto& pose : source.at("poses")) {
        keys(pose, {"name", "bones"});
        PoseKey entry;
        entry.name = text(pose.at("name"));
        require(!entry.name.empty() && entry.name.size() <= 32, "Invalid garment pose name");
        array(pose.at("bones"), boneCount, boneCount);
        for (const auto& matrix : pose.at("bones")) entry.bones.push_back(matrix16(matrix));
        asset->poses.push_back(std::move(entry));
    }

    const auto& topology = baked.at("topology");
    keys(topology, {"model", "membranes", "hinges", "membraneColors", "hingeColors", "adjacency"});
    require(topology.at("model") == "triangle-xpbd/1", "Unsupported garment membrane model");
    array(topology.at("membranes"), asset->triangles.size(), asset->triangles.size());
    for (const auto& value : topology.at("membranes")) {
        keys(value, {"inverse", "area"});
        array(value.at("inverse"), 4, 4);
        Membrane membrane;
        for (unsigned i = 0; i < 4; ++i) membrane.inverse[i] = static_cast<float>(scalar(value["inverse"][i], -1e8, 1e8));
        membrane.area = static_cast<float>(scalar(value["area"], 1e-12, 1e6));
        asset->membranes.push_back(membrane);
    }
    array(topology.at("hinges"), 0, asset->triangles.size() * 3);
    for (const auto& value : topology.at("hinges")) {
        keys(value, {"vertices", "angle"});
        array(value["vertices"], 4, 4);
        Hinge hinge;
        for (unsigned i = 0; i < 4; ++i) hinge.vertices[i] = integer(value["vertices"][i], 0, static_cast<unsigned>(asset->rest.size() - 1));
        hinge.angle = static_cast<float>(scalar(value["angle"], -3.141593, 3.141593));
        asset->hinges.push_back(hinge);
    }
    const auto colors = [&](const Json& batches, std::size_t maximum) {
        array(batches, maximum ? 1 : 0, 128);
        std::vector<std::vector<std::uint32_t>> out;
        for (const auto& batch : batches) {
            array(batch, 1, maximum);
            std::vector<std::uint32_t> ids;
            for (const auto& entry : batch) ids.push_back(integer(entry, 0, static_cast<unsigned>(maximum - 1)));
            out.push_back(std::move(ids));
        }
        return out;
    };
    asset->membraneColors = colors(topology["membraneColors"], asset->triangles.size());
    asset->hingeColors = colors(topology["hingeColors"], asset->hinges.size());
    array(topology.at("adjacency"), asset->rest.size(), asset->rest.size());
    for (const auto& adjacent : topology["adjacency"]) {
        array(adjacent, 1, 32);
        std::vector<std::uint32_t> ids;
        for (const auto& entry : adjacent) ids.push_back(integer(entry, 0, static_cast<unsigned>(asset->triangles.size() - 1)));
        asset->adjacency.push_back(std::move(ids));
    }
    validateTriangleTopology(*asset);
    return asset;
}

std::shared_ptr<const Asset> parseCookedCloth(const std::string& bytes) {
    try {
        const auto schema=Json::parse(bytes).value("schema",std::string());
        require(schema=="nativegpu.cloth-cook/1"||schema=="nativegpu.cloth-cook/2"||schema=="nativegpu.cloth-cook/3","Unsupported cloth cook schema");
        if(schema=="nativegpu.cloth-cook/3")return parseGarment(bytes);
        const bool triangleModel=schema=="nativegpu.cloth-cook/2";
        const auto payload = authenticated(bytes, schema.c_str());
        const auto& source = payload.at("source");
        keys(source, {"schema", "id", "revision", "seed", "kind", "mesh", "material", "pins", "collision", "lod", "budget", "baked"});
        require(text(source.at("schema")) == (triangleModel?"nativegpu.cloth/2":"nativegpu.cloth/1"), "Unsupported cloth source schema");
        auto asset = std::make_shared<Asset>();
        asset->triangleModel=triangleModel;
        asset->id = id(source.at("id"));
        asset->hash = assets::sha256(text(Json::parse(bytes).at("payloadJson")));
        asset->revision = integer(source.at("revision"), 0, 0xFFFFFFFFu);
        asset->seed = integer(source.at("seed"), 0, 0xFFFFFFFFu);
        const auto kind = text(source.at("kind"));
        require(kind == "environment" || kind == "skinned", "Invalid cloth kind");
        asset->skinned = kind == "skinned";
        const auto& mesh = source.at("mesh");
        keys(mesh, {"type", "columns", "rows", "widthMetres", "heightMetres"});
        require(text(mesh.at("type")) == "grid", "Only baked grid cloth is accepted by the native loader");
        asset->columns = integer(mesh.at("columns"), 2, 128);
        asset->rows = integer(mesh.at("rows"), 2, 128);
        scalar(mesh.at("widthMetres"), 0.02, 100);
        scalar(mesh.at("heightMetres"), 0.02, 100);
        const auto& material = source.at("material");
        keys(material, {"arealDensityKgM2", "thicknessMetres", "stretchCompliance", "shearCompliance",
                        triangleModel?"bendAngleCompliance":"bendCompliance", "damping", "dragCoefficient"});
        asset->material.arealDensityKgM2 = static_cast<float>(scalar(material.at("arealDensityKgM2"), 0.001, 50));
        asset->material.thicknessMetres = static_cast<float>(scalar(material.at("thicknessMetres"), 0.0001, 0.2));
        asset->material.stretchCompliance = static_cast<float>(scalar(material.at("stretchCompliance"), 0, 1));
        asset->material.shearCompliance = static_cast<float>(scalar(material.at("shearCompliance"), 0, 1));
        if(triangleModel)asset->material.bendAngleCompliance=static_cast<float>(scalar(material.at("bendAngleCompliance"),0,1e8));
        else asset->material.bendCompliance = static_cast<float>(scalar(material.at("bendCompliance"), 0, 1));
        asset->material.damping = static_cast<float>(scalar(material.at("damping"), 0, 50));
        asset->material.dragCoefficient = static_cast<float>(scalar(material.at("dragCoefficient"), 0, 10));
        const auto& collision = source.at("collision");
        keys(collision, {"thicknessMetres", "plane", "spheres", "capsules"});
        asset->collision.thicknessMetres = static_cast<float>(scalar(collision.at("thicknessMetres"), 0, 0.5));
        if (!collision.at("plane").is_null()) {
            const auto& plane = collision.at("plane");
            keys(plane, {"normal", "offsetMetres"});
            asset->collision.plane.present = true;
            asset->collision.plane.normal = vector(plane.at("normal"), -1, 1);
            const float length = std::sqrt(asset->collision.plane.normal.x * asset->collision.plane.normal.x +
                                           asset->collision.plane.normal.y * asset->collision.plane.normal.y +
                                           asset->collision.plane.normal.z * asset->collision.plane.normal.z);
            require(std::fabs(length - 1.0f) <= 1e-3f, "Cloth plane normal must be unit length");
            asset->collision.plane.offsetMetres = static_cast<float>(scalar(plane.at("offsetMetres"), -1000, 1000));
        }
        array(collision.at("spheres"), 0, 64);
        for (const auto& sphere : collision.at("spheres")) {
            keys(sphere, {"centre", "radiusMetres"});
            asset->collision.spheres.push_back({vector(sphere.at("centre")), static_cast<float>(scalar(sphere.at("radiusMetres"), 0.001, 1000))});
        }
        array(collision.at("capsules"), 0, 64);
        for (const auto& capsule : collision.at("capsules")) {
            keys(capsule, {"a", "b", "radiusMetres"});
            asset->collision.capsules.push_back({vector(capsule.at("a")), vector(capsule.at("b")),
                                                 static_cast<float>(scalar(capsule.at("radiusMetres"), 0.001, 1000))});
        }
        array(source.at("lod"), 1, 8);
        float previousDistance = 0;
        for (const auto& entry : source.at("lod")) {
            keys(entry, {"maxDistanceMetres", "columns", "rows"});
            Lod lod;
            lod.maxDistanceMetres = static_cast<float>(scalar(entry.at("maxDistanceMetres"), 0.1, 10000));
            lod.columns = integer(entry.at("columns"), 2, asset->columns);
            lod.rows = integer(entry.at("rows"), 2, asset->rows);
            require(lod.maxDistanceMetres > previousDistance, "Cloth LOD distances must increase");
            previousDistance = lod.maxDistanceMetres;
            if (asset->lod.empty()) require(lod.columns == asset->columns && lod.rows == asset->rows, "Cloth LOD 0 must match the simulation mesh");
            asset->lod.push_back(lod);
        }
        const auto& budget = source.at("budget");
        keys(budget, {"maxVertices", "maxSubsteps", "iterations"});
        const auto maxVertices = integer(budget.at("maxVertices"), 4, 65536);
        asset->maxSubsteps = integer(budget.at("maxSubsteps"), 1, 16);
        asset->iterations = integer(budget.at("iterations"), 1, 64);
        const auto& baked = source.at("baked");
        if(triangleModel)keys(baked,{"positions","triangles","masses","stretch","shear","bend","pins","totalAreaMetres2","topology"});
        else keys(baked, {"positions", "triangles", "masses", "stretch", "shear", "bend", "pins", "totalAreaMetres2"});
        array(baked.at("positions"), 4, maxVertices);
        require(baked.at("positions").size() == static_cast<std::size_t>(asset->columns) * asset->rows,
                "Baked vertex count does not match the grid declaration");
        for (const auto& position : baked.at("positions")) asset->rest.push_back(vector(position));
        array(baked.at("masses"), asset->rest.size(), asset->rest.size());
        array(baked.at("triangles"), 2, 2000000);
        for (const auto& triangle : baked.at("triangles")) {
            array(triangle, 3, 3);
            std::array<std::uint32_t, 3> indices{};
            for (unsigned i = 0; i < 3; ++i) indices[i] = integer(triangle[i], 0, static_cast<std::uint32_t>(asset->rest.size() - 1));
            require(indices[0] != indices[1] && indices[1] != indices[2] && indices[0] != indices[2], "Degenerate baked triangle");
            asset->triangles.push_back(indices);
        }
        asset->stretch = constraints(baked.at("stretch"), asset->rest.size());
        asset->shear = constraints(baked.at("shear"), asset->rest.size());
        asset->bend = constraints(baked.at("bend"), asset->rest.size());
        require(!asset->stretch.empty(), "Baked cloth has no structural constraints");
        array(baked.at("pins"), 0, asset->rest.size());
        std::set<std::uint32_t> pinned;
        for (const auto& pin : baked.at("pins")) {
            keys(pin, {"vertex", "anchor"});
            Pin target;
            target.vertex = integer(pin.at("vertex"), 0, static_cast<std::uint32_t>(asset->rest.size() - 1));
            target.anchor = vector(pin.at("anchor"));
            require(pinned.insert(target.vertex).second, "Duplicate baked pin");
            asset->pins.push_back(target);
        }
        asset->totalAreaMetres2 = static_cast<float>(scalar(baked.at("totalAreaMetres2"), 1e-6, 1e6));
        // Masses are derived from baked triangle areas. Recompute nothing here: an
        // offline/runtime disagreement would silently change the cloth's response.
        float totalMass = 0;
        for (const auto& mass : baked.at("masses")) {
            const auto value = static_cast<float>(scalar(mass, 1e-6, 1000));
            asset->masses.push_back(value);
            totalMass += value;
        }
        require(totalMass > 0 && std::isfinite(totalMass), "Baked cloth mass is invalid");
        asset->areas.resize(asset->rest.size());
        for (std::size_t i = 0; i < asset->areas.size(); ++i)
            asset->areas[i] = asset->masses[i] / asset->material.arealDensityKgM2;
        if(triangleModel){
            const auto& topology=baked.at("topology");keys(topology,{"model","membranes","hinges","membraneColors","hingeColors","adjacency"});
            require(topology.at("model")=="triangle-xpbd/1","Unsupported cloth membrane model");array(topology.at("membranes"),asset->triangles.size(),asset->triangles.size());
            for(const auto& value:topology.at("membranes")){keys(value,{"inverse","area"});array(value.at("inverse"),4,4);Membrane m;for(unsigned i=0;i<4;++i)m.inverse[i]=static_cast<float>(scalar(value["inverse"][i],-1e8,1e8));m.area=static_cast<float>(scalar(value["area"],1e-12,1e6));asset->membranes.push_back(m);}
            array(topology.at("hinges"),0,asset->triangles.size()*3);for(const auto& value:topology.at("hinges")){keys(value,{"vertices","angle"});array(value["vertices"],4,4);Hinge h;for(unsigned i=0;i<4;++i)h.vertices[i]=integer(value["vertices"][i],0,static_cast<unsigned>(asset->rest.size()-1));h.angle=static_cast<float>(scalar(value["angle"],-3.141593,3.141593));asset->hinges.push_back(h);}
            const auto colors=[&](const Json& batches,std::size_t maximum){array(batches,maximum?1:0,128);std::vector<std::vector<std::uint32_t>> out;for(const auto& batch:batches){array(batch,1,maximum);std::vector<std::uint32_t> ids;for(const auto& id:batch)ids.push_back(integer(id,0,static_cast<unsigned>(maximum-1)));out.push_back(std::move(ids));}return out;};
            asset->membraneColors=colors(topology["membraneColors"],asset->triangles.size());asset->hingeColors=colors(topology["hingeColors"],asset->hinges.size());
            array(topology.at("adjacency"),asset->rest.size(),asset->rest.size());for(const auto& adjacent:topology["adjacency"]){array(adjacent,1,32);std::vector<std::uint32_t> ids;for(const auto& id:adjacent)ids.push_back(integer(id,0,static_cast<unsigned>(asset->triangles.size()-1)));asset->adjacency.push_back(std::move(ids));}
            validateTriangleTopology(*asset);
        }
        return asset;
    } catch (const Error&) { throw; }
    catch (const std::exception& error) { throw Error("NG-E-CLOTH-ASSET", error.what()); }
}

std::shared_ptr<const Asset> loadCloth(const std::string& file) { return parseCookedCloth(read(std::filesystem::u8path(file))); }

Library loadClothLibrary(const std::string& manifestFile, const std::string& assetRoot) {
    try {
        const auto path = std::filesystem::canonical(std::filesystem::u8path(manifestFile));
        const auto bytes = read(path);
        const auto payload = authenticated(bytes, "nativegpu.cloth-library/1", "packageHash");
        keys(payload, {"schema", "libraryHash", "cloths", "runtimeIntegrated"});
        Library result;
        result.hash = assets::sha256(text(Json::parse(bytes).at("payloadJson")));
        const auto contained = [](const std::filesystem::path& parent, const std::filesystem::path& child) {
            const auto relative = child.lexically_relative(parent);
            return !relative.empty() && !relative.is_absolute() && *relative.begin() != std::filesystem::path("..");
        };
        require(payload.at("cloths").is_array() && payload.at("cloths").size() >= 1 && payload.at("cloths").size() <= 256,
                "Invalid cloth library contents");
        for (const auto& entry : payload.at("cloths")) {
            keys(entry, {"id", "hash", "file"});
            const auto name = id(entry.at("id")), expected = text(entry.at("hash"));
            require(hashText(expected) && text(entry.at("file")) == expected + ".ngcloth.json", "Invalid cloth content address");
            const auto file = std::filesystem::canonical(path.parent_path() / std::filesystem::u8path(text(entry.at("file"))));
            require(contained(path.parent_path(), file), "Cooked cloth escapes package directory");
            const auto asset = loadCloth(file.u8string());
            require(asset->id == name && asset->hash == expected, "Cloth package identity mismatch");
            require(result.cloths.emplace(name, asset).second, "Duplicate cloth library id");
        }
        (void)assetRoot;
        return result;
    } catch (const Error&) { throw; }
    catch (const std::exception& error) { throw Error("NG-E-CLOTH-ASSET", error.what()); }
}
} // namespace engine::cloth
