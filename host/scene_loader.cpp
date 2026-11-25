#include "scene_loader.hpp"
#include "../third_party/json.hpp"
#include "../engine/native/assets/sha256.hpp"
#include "../engine/native/assets/file_path.hpp"
#include "../engine/audio/asset_data.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <cmath>
#include <regex>
#include <initializer_list>

namespace host {
using nlohmann::json;
static void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
static void exactKeys(const json& value, std::initializer_list<const char*> keys,
                      const std::string& where,
                      std::initializer_list<const char*> optional = {}) {
    require(value.is_object(), where + " must be an object");
    std::set<std::string> allowed;
    for (const char* key : keys) allowed.insert(key);
    for (const char* key : optional) allowed.insert(key);
    for (auto item = value.begin(); item != value.end(); ++item) {
        require(allowed.count(item.key()) != 0,
                where + "." + item.key() + " is not supported");
    }
    for (const char* key : keys) {
        require(value.find(key) != value.end(), where + "." + key + " is required");
    }
}
// Scene names are UTF-8, so count code points rather than bytes and reject
// ASCII control characters. This mirrors the Node.js validator.
static bool text(const std::string& value) {
    if (value.empty()) return false;
    std::size_t count = 0;
    for (std::size_t i = 0; i < value.size();) {
        const unsigned char lead = static_cast<unsigned char>(value[i]);
        std::size_t width = 0;
        if (lead < 0x80) width = 1;
        else if ((lead & 0xE0) == 0xC0) width = 2;
        else if ((lead & 0xF0) == 0xE0) width = 3;
        else if ((lead & 0xF8) == 0xF0) width = 4;
        if (!width || i + width > value.size()) return false;
        if (width == 1 && (lead < 0x20 || lead == 0x7F)) return false;
        for (std::size_t offset = 1; offset < width; ++offset) {
            if ((static_cast<unsigned char>(value[i + offset]) & 0xC0) != 0x80)
                return false;
        }
        i += width;
        if (++count > 128) return false;
    }
    return value.find_first_not_of(" \t\r\n\f\v") != std::string::npos;
}
static bool uuid(const std::string& s) {
    static const std::regex pattern("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-8][0-9a-fA-F]{3}-[89aAbB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$");
    return std::regex_match(s, pattern);
}
// Compare JSON doubles against decimal bounds before converting the stored value.
// A float bound such as 0.001f otherwise rejects the valid JSON number 0.001.
static float number(const json& value, const std::string& name, double low, double high) {
    require(value.is_number(), name + " must be a number");
    const double n = value.get<double>();
    require(std::isfinite(n) && n >= low && n <= high, name + " outside supported range");
    return static_cast<float>(n);
}
static engine::Vec3 vector(const json& value, const std::string& name, double low, double high) {
    require(value.is_array() && value.size() == 3, name + " must contain 3 numbers");
    return {number(value[0],name,low,high),number(value[1],name,low,high),number(value[2],name,low,high)};
}

// Loads a baked Voronoi shard set (tools/offline/voronoi.mjs, schema nativegpu.voronoi/1)
// and converts it into body-local triangle hulls plus the physics proxies.
//
// Two conversions matter here:
//   * The hull is translated so its centroid sits at the origin, because the shard body's
//     transform already places it. Leaving world-space coordinates in would double-apply
//     the entity transform.
//   * The physics proxy is the axis-aligned bounds of that translated hull, not the baked
//     OBB. The rigid body has no independent frame for the OBB axes (it inherits the
//     entity orientation), so an OBB-shaped proxy would be rotated by the wrong amount.
//     The AABB is conservative and exact in the frame the body actually uses.
static engine::VoronoiPackage loadVoronoiPackage(const std::filesystem::path& path,
                                                 const std::string& sourceName) {
    std::ifstream stream(engine::assets::nativeFilePath(path), std::ios::binary);
    require(stream.good(), "voronoi package not found: " + path.string());
    json data;
    try {
        stream >> data;
    } catch (const std::exception&) {
        throw std::runtime_error("voronoi package is not valid JSON: " + path.string());
    }
    require(data.is_object(), "voronoi package must be an object");
    require(data.at("schema").is_string() &&
                data.at("schema").get<std::string>() == "nativegpu.voronoi/1",
            "voronoi package schema must be nativegpu.voronoi/1");
    require(data.at("cells").is_array(), "voronoi package cells must be an array");
    const auto& cells = data.at("cells");
    require(!cells.empty() && cells.size() <= 256,
            "voronoi package must contain 1..256 cells");

    engine::VoronoiPackage package;
    package.source = sourceName;
    require(data.at("half").is_array() && data.at("half").size() == 3,
            "voronoi package half must be xyz");
    for (int axis = 0; axis < 3; ++axis) {
        package.half[axis] = data.at("half")[axis].get<float>();
        require(package.half[axis] > 0.0f, "voronoi package half must be positive");
    }
    if (data.contains("totals")) {
        const auto& totals = data.at("totals");
        if (totals.contains("volume")) package.volume = totals.at("volume").get<float>();
        if (totals.contains("boxVolume")) package.boxVolume = totals.at("boxVolume").get<float>();
    }

    for (const auto& cell : cells) {
        require(cell.at("vertices").is_array() && cell.at("vertices").size() >= 4,
                "voronoi cell needs at least 4 vertices");
        require(cell.at("faces").is_array() && cell.at("faces").size() >= 4,
                "voronoi cell needs at least 4 faces");
        const auto& points = cell.at("vertices");
        std::vector<engine::ConvexVertex> vertices;
        vertices.reserve(points.size());
        double sum[3]{0.0, 0.0, 0.0};
        for (const auto& point : points) {
            require(point.is_array() && point.size() == 3, "voronoi vertex must be xyz");
            for (int axis = 0; axis < 3; ++axis) sum[axis] += point[axis].get<double>();
        }
        const double count = static_cast<double>(points.size());
        const float origin[3]{static_cast<float>(sum[0] / count), static_cast<float>(sum[1] / count),
                              static_cast<float>(sum[2] / count)};
        float boundsMin[3]{1.0e30f, 1.0e30f, 1.0e30f};
        float boundsMax[3]{-1.0e30f, -1.0e30f, -1.0e30f};
        for (const auto& point : points) {
            engine::ConvexVertex vertex;
            for (int axis = 0; axis < 3; ++axis) {
                vertex.position[axis] = point[axis].get<float>() - origin[axis];
                boundsMin[axis] = std::min(boundsMin[axis], vertex.position[axis]);
                boundsMax[axis] = std::max(boundsMax[axis], vertex.position[axis]);
            }
            vertices.push_back(vertex);
        }

        std::vector<std::uint32_t> indices;
        // Cut faces go into their own mesh: they are the surfaces the bake created by
        // splitting the prop, so they get the interior tint while the original surface
        // keeps the authored material.
        std::vector<std::uint32_t> interiorIndices;
        for (const auto& face : cell.at("faces")) {
            require(face.at("indices").is_array() && face.at("indices").size() >= 3,
                    "voronoi face needs at least 3 indices");
            require(face.at("normal").is_array() && face.at("normal").size() == 3,
                    "voronoi face needs a normal");
            const auto& polygon = face.at("indices");
            const float nx = face.at("normal")[0].get<float>();
            const float ny = face.at("normal")[1].get<float>();
            const float nz = face.at("normal")[2].get<float>();
            const float offset = face.contains("offset") ? face.at("offset").get<float>() : 0.0f;
            // A face lies on the original box surface when its normal is axis aligned and
            // its plane sits at that half extent. Everything else is a fracture face.
            int dominant = 0;
            const float absComponents[3]{std::abs(nx), std::abs(ny), std::abs(nz)};
            if (absComponents[1] > absComponents[dominant]) dominant = 1;
            if (absComponents[2] > absComponents[dominant]) dominant = 2;
            const bool axisAligned = absComponents[dominant] > 0.999f;
            const bool onSurface = axisAligned &&
                                   std::abs(std::abs(offset) - package.half[dominant]) < 1.0e-3f;
            std::vector<std::uint32_t>& target = onSurface ? indices : interiorIndices;

            std::vector<std::uint32_t> loop;
            loop.reserve(polygon.size());
            for (const auto& index : polygon) {
                require(index.is_number_integer(), "voronoi face index must be an integer");
                const long long value = index.get<long long>();
                require(value >= 0 && static_cast<std::size_t>(value) < vertices.size(),
                        "voronoi face index out of range");
                loop.push_back(static_cast<std::uint32_t>(value));
            }
            // The bake does not promise a winding, so the normal decides. A fan whose
            // winding disagrees with the recorded outward normal would light the shard
            // inside-out, which reads as a hole rather than a face.
            const auto& p0 = vertices[loop[0]].position;
            const auto& p1 = vertices[loop[1]].position;
            const auto& p2 = vertices[loop[2]].position;
            const float e1[3]{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
            const float e2[3]{p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
            const float cross[3]{e1[1] * e2[2] - e1[2] * e2[1],
                                 e1[2] * e2[0] - e1[0] * e2[2],
                                 e1[0] * e2[1] - e1[1] * e2[0]};
            const bool flip = cross[0] * nx + cross[1] * ny + cross[2] * nz < 0.0f;
            for (std::size_t corner = 1; corner + 1 < loop.size(); ++corner) {
                target.push_back(loop[0]);
                target.push_back(flip ? loop[corner + 1] : loop[corner]);
                target.push_back(flip ? loop[corner] : loop[corner + 1]);
            }
        }
        require(indices.size() + interiorIndices.size() >= 12,
                "voronoi cell needs at least 4 triangles");

        // UVs are projected in the prop's own frame against the original box extents, so a
        // shard samples exactly the texels the intact wall showed at that position. The
        // box path maps each face to 0..1, and this reproduces that mapping.
        auto projectUv = [&](const float* position, int dominant, float* uv) {
            const int uAxis = (dominant + 1) % 3;
            const int vAxis = (dominant + 2) % 3;
            const float halfU = std::max(1.0e-4f, package.half[uAxis]);
            const float halfV = std::max(1.0e-4f, package.half[vAxis]);
            uv[0] = (position[uAxis] + origin[uAxis] + halfU) / (2.0f * halfU);
            uv[1] = (position[vAxis] + origin[vAxis] + halfV) / (2.0f * halfV);
        };
        auto buildFaceted = [&](const std::vector<std::uint32_t>& source,
                                engine::ConvexMesh& out) {
            std::vector<engine::ConvexVertex> faceted;
            faceted.reserve(source.size());
            for (std::size_t triangle = 0; triangle < source.size(); triangle += 3) {
                const auto& a = vertices[source[triangle]];
                const auto& b = vertices[source[triangle + 1]];
                const auto& c = vertices[source[triangle + 2]];
                const float e1[3]{b.position[0] - a.position[0], b.position[1] - a.position[1],
                                  b.position[2] - a.position[2]};
                const float e2[3]{c.position[0] - a.position[0], c.position[1] - a.position[1],
                                  c.position[2] - a.position[2]};
                float n[3]{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                           e1[0] * e2[1] - e1[1] * e2[0]};
                const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (length > 1.0e-9f) {
                    n[0] /= length; n[1] /= length; n[2] /= length;
                } else {
                    n[0] = 0.0f; n[1] = 1.0f; n[2] = 0.0f;
                }
                int dominant = 0;
                const float absN[3]{std::abs(n[0]), std::abs(n[1]), std::abs(n[2])};
                if (absN[1] > absN[dominant]) dominant = 1;
                if (absN[2] > absN[dominant]) dominant = 2;
                const engine::ConvexVertex* corner[3]{&a, &b, &c};
                for (int vertex = 0; vertex < 3; ++vertex) {
                    engine::ConvexVertex built = *corner[vertex];
                    for (int axis = 0; axis < 3; ++axis) built.normal[axis] = n[axis];
                    projectUv(built.position, dominant, built.uv);
                    faceted.push_back(built);
                }
            }
            out.vertices = std::move(faceted);
            out.indices.clear();
            out.indices.reserve(out.vertices.size());
            for (std::size_t index = 0; index < out.vertices.size(); ++index) {
                out.indices.push_back(static_cast<std::uint32_t>(index));
            }
            for (int axis = 0; axis < 3; ++axis) {
                out.min[axis] = boundsMin[axis];
                out.max[axis] = boundsMax[axis];
            }
            out.baked = true;
        };

        engine::ConvexMesh mesh;
        buildFaceted(indices, mesh);

        engine::VoronoiShardProxy proxy;
        for (int axis = 0; axis < 3; ++axis) {
            proxy.center[axis] = origin[axis];
            proxy.half[axis] = std::max(0.02f, 0.5f * (mesh.max[axis] - mesh.min[axis]));
        }
        proxy.volume = cell.contains("volume") ? cell.at("volume").get<float>() : 0.0f;
        require(proxy.volume > 0.0f, "voronoi cell needs a positive volume");
        proxy.mesh = static_cast<int>(package.meshes.size());
        if (!interiorIndices.empty()) {
            engine::ConvexMesh interior;
            buildFaceted(interiorIndices, interior);
            proxy.meshInterior = static_cast<int>(package.meshes.size()) + 1;
            package.shards.push_back(proxy);
            package.meshes.push_back(std::move(mesh));
            package.meshes.push_back(std::move(interior));
            continue;
        }

        package.shards.push_back(proxy);
        package.meshes.push_back(std::move(mesh));
    }

    require(data.at("bonds").is_array(), "voronoi package bonds must be an array");
    for (const auto& bond : data.at("bonds")) {
        engine::VoronoiLink link;
        link.a = bond.at("a").get<int>();
        link.b = bond.at("b").get<int>();
        link.area = bond.at("area").get<float>();
        require(link.a >= 0 && link.b >= 0 &&
                    static_cast<std::size_t>(link.a) < package.shards.size() &&
                    static_cast<std::size_t>(link.b) < package.shards.size(),
                "voronoi bond references a missing cell");
        package.links.push_back(link);
    }
    return package;
}

bool loadScene(const std::filesystem::path& inputPath, SceneDocument& result, std::string& error) {
    try {
        const auto path=engine::assets::nativeFilePath(inputPath);
        std::error_code ec;
        auto size = std::filesystem::file_size(path, ec);
        require(!ec && size <= 16 * 1024 * 1024, "Scene is missing or exceeds 16 MiB");
        std::ifstream input(path, std::ios::binary);
        require(input.good(), "Cannot open scene");
        json data = json::parse(input);
        exactKeys(data, {"schemaVersion","projectId","name","revision","entities","settings"}, "scene",
                  {"materials","lighting","audio","joints","vegetation","water"});
        require(data.at("schemaVersion").is_number_integer(), "Unsupported scene schemaVersion");
        const int schemaVersion = data.at("schemaVersion").get<int>();
        require(schemaVersion == 1 || schemaVersion == 2, "Unsupported scene schemaVersion");
        SceneDocument next;
        auto search = std::filesystem::absolute(inputPath).parent_path();
        for(;;) {
            if (std::filesystem::exists(engine::assets::nativeFilePath(search / "gameplay/components.json"))) { next.scene.assetRoot=search.u8string(); break; }
            const auto nested = search / "assets";
            if (std::filesystem::exists(engine::assets::nativeFilePath(nested / "gameplay/components.json"))) { next.scene.assetRoot=nested.u8string(); break; }
            if (!search.has_parent_path() || search == search.parent_path()) break;
            search=search.parent_path();
        }
        if(next.scene.assetRoot.empty()) {
            const auto fallback = std::filesystem::absolute(inputPath).parent_path().parent_path() / "assets";
            if (std::filesystem::exists(engine::assets::nativeFilePath(fallback / "gameplay/components.json"))) next.scene.assetRoot=fallback.u8string();
        }
        auto resourceRoot=engine::assets::weaklyCanonicalFilePath(std::filesystem::absolute(path).parent_path());
        for(auto parent=std::filesystem::absolute(inputPath).parent_path();;parent=parent.parent_path()) {
            if(parent.filename()==".screen"){resourceRoot=engine::assets::weaklyCanonicalFilePath(std::filesystem::u8path(next.scene.assetRoot));break;}
            if(!parent.has_parent_path()||parent==parent.parent_path())break;
        }
        if(data.contains("water")){
            const auto& w=data.at("water");exactKeys(w,{"file","sha256","origin"},"scene.water",{"roughness","absorption","scattering","obstaclesFromColliders","links"});
            auto& target=next.water;target.present=true;target.file=w.at("file").get<std::string>();target.sha256=w.at("sha256").get<std::string>();
            target.origin=vector(w.at("origin"),"water.origin",-1.e6,1.e6);
            if(w.contains("roughness"))target.roughness=number(w.at("roughness"),"water.roughness",.025,1);
            if(w.contains("absorption"))target.absorption=vector(w.at("absorption"),"water.absorption",0,100);
            if(w.contains("scattering"))target.scattering=vector(w.at("scattering"),"water.scattering",0,1);
            if(w.contains("obstaclesFromColliders")){
                require(w.at("obstaclesFromColliders").is_boolean(),"water.obstaclesFromColliders must be boolean");
                target.obstaclesFromColliders=w.at("obstaclesFromColliders").get<bool>();
            }
            const auto relative=std::filesystem::u8path(target.file);
            require(!target.file.empty()&&target.file.size()<=512&&!relative.is_absolute()&&target.file.find_first_of(":\\")==std::string::npos&&target.file.find("//")==std::string::npos&&target.file.size()>13&&target.file.substr(target.file.size()-13)==".ngwater.json","Invalid water dependency path");
            for(const auto& part:relative)require(part!=".."&&part!=".","Water dependency escapes scene");
            require(std::regex_match(target.sha256,std::regex("^[a-f0-9]{64}$")),"Invalid water dependency hash");
            const auto resolved=engine::assets::canonicalFilePath(resourceRoot/relative),local=resolved.lexically_relative(resourceRoot);
            require(!local.empty()&&*local.begin()!="..","Water dependency escaped scene directory");
            require(std::filesystem::file_size(resolved)<=2*1024*1024,"Water dependency exceeds 2 MiB");
            std::ifstream stream(resolved,std::ios::binary);target.cookedData.assign(std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>());
            require(engine::assets::sha256(target.cookedData)==target.sha256,"Water dependency changed");
            const auto cooked=json::parse(target.cookedData);
            require(cooked.at("schema")=="nativegpu.water-cook/1"&&engine::assets::sha256(cooked.at("payloadJson").get<std::string>())==cooked.at("hash").get<std::string>(),"Invalid cooked water payload");
            if(w.contains("links")){
                require(w.at("links").is_array()&&w.at("links").size()==1,"scene.water.links must contain exactly one entry");
                for(const auto& item:w.at("links")){
                    exactKeys(item,{"file","sha256","origin","source","landing","flowCubicMetresPerSecond"},"scene.water.links[]",
                              {"sourceRadiusMetres","landingRadiusMetres","transitSeconds","widthMetres","exitNormalSpeed"});
                    SceneDocument::Water::Link link;
                    link.file=item.at("file").get<std::string>();link.sha256=item.at("sha256").get<std::string>();
                    link.origin=vector(item.at("origin"),"water.links[].origin",-1.e6,1.e6);
                    const auto& source=item.at("source"),&landing=item.at("landing");
                    require(source.is_array()&&source.size()==2&&landing.is_array()&&landing.size()==2,"water.links regions require two coordinates");
                    link.sourceX=number(source[0],"water.links[].source[0]",-1000,1000);
                    link.sourceZ=number(source[1],"water.links[].source[1]",-1000,1000);
                    link.landingX=number(landing[0],"water.links[].landing[0]",-1000,1000);
                    link.landingZ=number(landing[1],"water.links[].landing[1]",-1000,1000);
                    if(item.contains("sourceRadiusMetres"))link.sourceRadiusMetres=number(item.at("sourceRadiusMetres"),"water.links[].sourceRadiusMetres",.05,100);
                    if(item.contains("landingRadiusMetres"))link.landingRadiusMetres=number(item.at("landingRadiusMetres"),"water.links[].landingRadiusMetres",.05,100);
                    if(item.contains("transitSeconds"))link.transitSeconds=number(item.at("transitSeconds"),"water.links[].transitSeconds",0,60);
                    if(item.contains("widthMetres"))link.widthMetres=number(item.at("widthMetres"),"water.links[].widthMetres",.1,50);
                    if(item.contains("exitNormalSpeed"))link.exitNormalSpeed=number(item.at("exitNormalSpeed"),"water.links[].exitNormalSpeed",0,50);
                    link.flowCubicMetresPerSecond=number(item.at("flowCubicMetresPerSecond"),"water.links[].flowCubicMetresPerSecond",0,10000);
                    const auto linkRelative=std::filesystem::u8path(link.file);
                    require(!link.file.empty()&&link.file.size()<=512&&!linkRelative.is_absolute()&&link.file.find_first_of(":\\")==std::string::npos&&link.file.find("//")==std::string::npos&&link.file.size()>13&&link.file.substr(link.file.size()-13)==".ngwater.json","Invalid linked water dependency path");
                    for(const auto& part:linkRelative)require(part!=".."&&part!=".","Linked water dependency escapes scene");
                    require(std::regex_match(link.sha256,std::regex("^[a-f0-9]{64}$")),"Invalid linked water dependency hash");
                    const auto linkResolved=engine::assets::canonicalFilePath(resourceRoot/linkRelative);
                    const auto linkLocal=linkResolved.lexically_relative(resourceRoot);
                    require(!linkLocal.empty()&&*linkLocal.begin()!="..","Linked water dependency escaped scene directory");
                    require(std::filesystem::file_size(linkResolved)<=2*1024*1024,"Linked water dependency exceeds 2 MiB");
                    std::ifstream linkStream(linkResolved,std::ios::binary);link.cookedData.assign(std::istreambuf_iterator<char>(linkStream),std::istreambuf_iterator<char>());
                    require(engine::assets::sha256(link.cookedData)==link.sha256,"Linked water dependency changed");
                    const auto linkCooked=json::parse(link.cookedData);
                    require(linkCooked.at("schema")=="nativegpu.water-cook/1"&&engine::assets::sha256(linkCooked.at("payloadJson").get<std::string>())==linkCooked.at("hash").get<std::string>(),"Invalid linked cooked water payload");
                    target.links.push_back(std::move(link));
                }
            }
        }
        engine::gameplay::Registry componentRegistry;
        componentRegistry.load(engine::gameplay::readJson((std::filesystem::u8path(next.scene.assetRoot)/"gameplay/components.json").u8string()));
        next.projectId = data.at("projectId").get<std::string>();
        require(uuid(next.projectId), "Invalid project UUID");
        next.name = data.at("name").get<std::string>();
        require(text(next.name), "Project name must contain 1-128 printable characters");
        require(data.at("revision").is_number_unsigned() || data.at("revision").is_number_integer(), "Invalid revision");
        const auto rev = data.at("revision").get<double>();
        require(rev >= 0 && rev <= 9007199254740991.0 && std::floor(rev) == rev, "Invalid revision");
        next.revision = static_cast<unsigned long long>(rev);
        if(data.contains("vegetation")) {
            const auto& vegetation=data.at("vegetation");
            exactKeys(vegetation,{"regions"},"scene.vegetation",{});
            require(vegetation.at("regions").is_array()&&vegetation.at("regions").size()<=256,"scene.vegetation.regions must be an array of at most 256 entries");
            const auto safeRelative=[](const std::string& value,const char* field) {
                const auto path=std::filesystem::u8path(value);
                require(!value.empty()&&!path.is_absolute()&&value.find(':')==std::string::npos&&value.find('\0')==std::string::npos,
                        std::string("NG-E-ASSET-PATH: ")+field+" must be project relative");
                for(const auto& part:path)require(part!="..",std::string("NG-E-ASSET-PATH: ")+field+" escapes the project");
            };
            for(const auto& item:vegetation.at("regions")) {
                exactKeys(item,{"file"},"scene.vegetation.regions[]",{"cooked","textures"});
                engine::VegetationRegionReference region;
                region.file=item.at("file").get<std::string>();
                region.cooked=item.value("cooked",region.cooked);
                region.textures=item.value("textures",region.textures);
                safeRelative(region.file,"vegetation region");safeRelative(region.cooked,"vegetation cooked directory");safeRelative(region.textures,"vegetation texture directory");
                require(std::filesystem::u8path(region.file).extension()==".ngvrg","Vegetation region must be a cooked .ngvrg package");
                next.scene.vegetationRegions.push_back(std::move(region));
            }
        }
        // Named materials are a v2 feature. Texture names are validated here; the host
        // resolves them to cooked slices and reports a missing file before rendering.
        std::map<std::string, std::size_t> materialByName;
        if (data.contains("materials")) {
            require(schemaVersion == 2, "scene.materials requires schemaVersion 2");
            const auto& table = data.at("materials");
            require(table.is_object(), "scene.materials must be an object");
            require(table.size() <= 256, "At most 256 materials are supported");
            const std::regex textureName("^[A-Za-z0-9_.-]{1,64}$");
            for (auto item = table.begin(); item != table.end(); ++item) {
                const std::string name = item.key();
                require(text(name), "Material name must contain 1-128 printable characters");
                require(materialByName.count(name) == 0, "Duplicate material name: " + name);
                const auto& value = item.value();
                exactKeys(value, {"albedo"}, "scene.materials." + name,
                          {"normal","orm","uvScale","roughness","metalness","wetness","wear",
                           "clearcoat","clearcoatRoughness","coat","coatRoughness","coatNormal","ior","alphaMode","alphaCutoff","emissive","emissiveFactor","emissiveStrength","textureMappings","occlusion","occlusionStrength","unlit","opacity","layered"});
                engine::MaterialDefinition material;
                material.name = name;
                material.albedo = value.at("albedo").get<std::string>();
                material.normal = value.contains("normal")
                    ? value.at("normal").get<std::string>() : material.albedo;
                material.orm = value.contains("orm")
                    ? value.at("orm").get<std::string>() : material.albedo;
                for (const std::string* texture : {&material.albedo, &material.normal, &material.orm}) {
                    require(text(*texture) && std::regex_match(*texture, textureName),
                            "Material texture names must be 1-64 characters of [A-Za-z0-9_.-]");
                }
                material.uvScale = value.contains("uvScale")
                    ? number(value.at("uvScale"), "uvScale", 0.05, 1000.0) : 1.0f;
                material.roughness = value.contains("roughness")
                    ? number(value.at("roughness"), "roughness", 0, 1) : 1.0f;
                material.metalness = value.contains("metalness")
                    ? number(value.at("metalness"), "metalness", 0, 1) : 1.0f;
                material.wetness = value.contains("wetness")
                    ? number(value.at("wetness"), "wetness", 0, 1) : 0.0f;
                material.wear = value.contains("wear")
                    ? number(value.at("wear"), "wear", 0, 1) : 0.0f;
                material.clearcoat = value.contains("clearcoat")
                    ? number(value.at("clearcoat"), "clearcoat", 0, 1) : 0.0f;
                material.clearcoatRoughness = value.contains("clearcoatRoughness")
                    ? number(value.at("clearcoatRoughness"), "clearcoatRoughness", 0, 1) : 0.1f;
                if(value.contains("ior")){material.ior=number(value.at("ior"),"ior",0,3.4028234663852886e38);require(engine::validMaterialIor(value.at("ior").get<double>()),"ior must be 0 or at least 1");}
                const auto alphaMode=value.value("alphaMode",std::string("OPAQUE"));
                require(alphaMode=="OPAQUE"||alphaMode=="MASK"||alphaMode=="BLEND","alphaMode must be OPAQUE, MASK or BLEND");
                material.alphaMasked=alphaMode=="MASK";
                material.alphaBlended=alphaMode=="BLEND";
                if(value.contains("opacity"))material.opacity=number(value.at("opacity"),"opacity",0,1);
                material.alphaCutoff=value.contains("alphaCutoff")?number(value.at("alphaCutoff"),"alphaCutoff",0,1):.5f;
                if(value.contains("emissive")){
                    material.emissive=value.at("emissive").get<std::string>();
                    require(std::regex_match(material.emissive,textureName),"Invalid emissive texture name");
                }
                if(value.contains("emissiveFactor")){
                    const auto factor=vector(value.at("emissiveFactor"),"emissiveFactor",0,1);
                    material.emissiveFactor[0]=factor.x;material.emissiveFactor[1]=factor.y;material.emissiveFactor[2]=factor.z;
                }
                if(value.contains("emissiveStrength"))material.emissiveStrength=number(value.at("emissiveStrength"),"emissiveStrength",0,1000000);
                if(value.contains("occlusion")){material.occlusion=value.at("occlusion").get<std::string>();require(std::regex_match(material.occlusion,textureName),"Invalid occlusion texture name");}
                for(const auto& slot:{std::make_pair("coat",&material.coat),std::make_pair("coatRoughness",&material.coatRoughness),std::make_pair("coatNormal",&material.coatNormal)})if(value.contains(slot.first)){*slot.second=value.at(slot.first).get<std::string>();require(std::regex_match(*slot.second,textureName),"Invalid coating texture name");}
                if(value.contains("occlusionStrength"))material.occlusionStrength=number(value.at("occlusionStrength"),"occlusionStrength",0,1);
                if(value.contains("unlit")){require(value.at("unlit").is_boolean(),"unlit must be boolean");material.unlit=value.at("unlit").get<bool>();}
                if(value.contains("textureMappings")){
                    const auto& mappings=value.at("textureMappings");exactKeys(mappings,{},"textureMappings",{"albedo","normal","orm","emissive","occlusion","coat","coatRoughness","coatNormal"});material.hasTextureMappings=!mappings.empty();
                    const char* slots[]={"albedo","normal","orm","emissive","occlusion","coat","coatRoughness","coatNormal"};
                    for(unsigned slot=0;slot<8;slot++)if(mappings.contains(slots[slot])){
                        const auto& v=mappings.at(slots[slot]);exactKeys(v,{},"texture mapping",{"offset","scale","rotation","wrapS","wrapT","magFilter","minFilter"});
                        float offset[2]{0,0},scale[2]{1,1};
                        for(const auto& pair: {std::make_pair("offset",offset),std::make_pair("scale",scale)})if(v.contains(pair.first)){
                            const auto& data=v.at(pair.first);require(data.is_array()&&data.size()==2,"Texture mapping requires two components");for(unsigned k=0;k<2;k++)pair.second[k]=number(data[k],pair.first,-1000000,1000000);
                        }
                        double rotation=0;if(v.contains("rotation")){number(v.at("rotation"),"rotation",-1000000,1000000);rotation=v.at("rotation").get<double>();}
                        const auto select=[&](const char* key,const std::vector<std::string>& options,const char* fallback){const auto text=v.value(key,std::string(fallback));const auto it=std::find(options.begin(),options.end(),text);require(it!=options.end(),std::string("Invalid texture ")+key);return static_cast<unsigned>(it-options.begin());};
                        const unsigned u=select("wrapS",{"repeat","clamp","mirror"},"repeat"),t=select("wrapT",{"repeat","clamp","mirror"},"repeat"),mag=select("magFilter",{"nearest","linear"},"linear"),min=select("minFilter",{"nearest","linear","nearest-mipmap-nearest","linear-mipmap-nearest","nearest-mipmap-linear","linear-mipmap-linear"},"linear-mipmap-linear");
                        auto& mapping=material.textureMappings[slot];const double c=std::cos(rotation),s=std::sin(rotation);mapping.row0[0]=static_cast<float>(c*scale[0]);mapping.row0[1]=static_cast<float>(-s*scale[1]);mapping.row0[2]=offset[0];mapping.row1[0]=static_cast<float>(s*scale[0]);mapping.row1[1]=static_cast<float>(c*scale[1]);mapping.row1[2]=offset[1];mapping.sampler=1+(u*3+t)*12+min*2+mag;
                    }
                }
                if(value.contains("layered")){
                    require(!material.alphaMasked&&!material.alphaBlended&&!material.unlit,"Layered material requires opaque lit surface");
                    material.layeredAuthoring=value.at("layered");
                    auto parsed=engine::material::layered::parseNormalizedMaterial(material.layeredAuthoring);
                    material.layers=std::move(parsed.layers);material.surfaceRelief=std::move(parsed.relief);
                    for(const auto& layer:material.layers)for(const auto& slot:layer.gpu.slots)require(slot.uvSet<8,"Layered material UV set must be in [0, 7]");
                    require(material.surfaceRelief.gpu.uvSet<8,"Surface relief UV set must be in [0, 7]");
                    if((material.surfaceRelief.gpu.flags&3u)!=0&&(material.surfaceRelief.gpu.flags&(1u<<4))==0)require(material.surfaceRelief.gpu.heightLayer<material.layers.size(),"Surface relief height layer is out of range");
                }
                materialByName[name] = next.scene.materials.size();
                next.scene.materials.push_back(std::move(material));
            }
        }
        if (data.contains("lighting")) {
            const auto& lighting = data.at("lighting");
            exactKeys(lighting, {}, "scene.lighting",
                      {"environment","ambient","skyColor","keyDirection","keyColor","keyIntensity",
                       "fogColor","fogDensity","fogHeightFalloff","fogBaseHeight",
                       "toneMap","exposure","cloudCoverage","cloudHeight",
                       "cloudThickness","cloudSpeed","quality","localShadows","ibl","ddgi",
                       "cloudShadows","temporal","maxShadowLights","fogVolumes"});
            auto color = [&](const char* key, float target[3]) {
                if (!lighting.contains(key)) return;
                const auto value = vector(lighting.at(key), std::string("lighting.") + key, 0, 1);
                target[0] = value.x; target[1] = value.y; target[2] = value.z;
            };
            color("ambient", next.scene.lighting.ambient);
            color("skyColor", next.scene.lighting.skyColor);
            color("keyColor", next.scene.lighting.keyColor);
            color("fogColor", next.scene.lighting.fogColor);
            if(lighting.contains("environment")){
                const auto& ref=lighting.at("environment");exactKeys(ref,{"file","sha256"},"lighting.environment",{"intensity","rotation"});
                const auto file=ref.at("file").get<std::string>(),hash=ref.at("sha256").get<std::string>();const auto relative=std::filesystem::u8path(file);
                require(!file.empty()&&file.size()<=512&&!relative.is_absolute()&&relative.extension()==".ngenv"&&file.find_first_of(":\\")==std::string::npos&&file.find("//")==std::string::npos,"NG-E-ASSET-ENVIRONMENT: invalid dependency path");
                require(std::none_of(file.begin(),file.end(),[](unsigned char c){return c<32;}),"NG-E-ASSET-ENVIRONMENT: invalid path");for(const auto& part:relative)require(part!=".."&&part!="."&&!part.empty(),"NG-E-ASSET-ENVIRONMENT: invalid path");require(std::regex_match(hash,std::regex("^[a-f0-9]{64}$")),"NG-E-ASSET-ENVIRONMENT: invalid SHA-256");
                const auto root=resourceRoot,resolved=engine::assets::canonicalFilePath(root/relative),inside=resolved.lexically_relative(root);require(!inside.empty()&&*inside.begin()!="..","NG-E-ASSET-ENVIRONMENT: dependency escaped scene directory");
                const auto size=std::filesystem::file_size(resolved);require(size<=256ull*1024*1024,"NG-E-ASSET-ENVIRONMENT: dependency exceeds 256 MiB");std::ifstream stream(resolved,std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());require(bytes.size()==size&&engine::assets::sha256(bytes)==hash,"NG-E-ASSET-ENVIRONMENT: dependency changed");
                auto& target=next.scene.lighting;target.panorama=engine::assets::decodeEnvironmentData(bytes,hash);target.environmentFile=file;target.environmentHash=hash;
                if(ref.contains("intensity"))target.environmentIntensity=number(ref.at("intensity"),"environment.intensity",0,10000);if(ref.contains("rotation"))target.environmentRotation=number(ref.at("rotation"),"environment.rotation",-3600,3600);
                if(!lighting.contains("toneMap"))target.toneMap=true;if(!lighting.contains("cloudCoverage"))target.cloudCoverage=0;
            }
            if (lighting.contains("keyDirection")) {
                const auto value = vector(lighting.at("keyDirection"), "lighting.keyDirection",
                                          -1, 1);
                next.scene.lighting.keyDirection[0] = value.x;
                next.scene.lighting.keyDirection[1] = value.y;
                next.scene.lighting.keyDirection[2] = value.z;
                require(value.x*value.x+value.y*value.y+value.z*value.z>1e-8f,"lighting.keyDirection must be nonzero");
            }
            if (lighting.contains("keyIntensity")) {
                next.scene.lighting.keyIntensity =
                    number(lighting.at("keyIntensity"), "lighting.keyIntensity", 0, 100);
            }
            if (lighting.contains("fogDensity")) {
                next.scene.lighting.fogDensity =
                    number(lighting.at("fogDensity"), "lighting.fogDensity", 0, 10);
            }
            if (lighting.contains("fogHeightFalloff")) {
                next.scene.lighting.fogHeightFalloff =
                    number(lighting.at("fogHeightFalloff"), "lighting.fogHeightFalloff", 0, 10);
            }
            if (lighting.contains("fogBaseHeight")) {
                next.scene.lighting.fogBaseHeight =
                    number(lighting.at("fogBaseHeight"), "lighting.fogBaseHeight", -1000, 1000);
            }
            if (lighting.contains("exposure")) {
                next.scene.lighting.exposure =
                    number(lighting.at("exposure"), "lighting.exposure", 0.01, 100);
            }
            if (lighting.contains("cloudCoverage")) {
                next.scene.lighting.cloudCoverage =
                    number(lighting.at("cloudCoverage"), "lighting.cloudCoverage", 0, 1);
            }
            if (lighting.contains("cloudHeight")) {
                next.scene.lighting.cloudHeight =
                    number(lighting.at("cloudHeight"), "lighting.cloudHeight", 1, 1000);
            }
            if (lighting.contains("cloudThickness")) {
                next.scene.lighting.cloudThickness =
                    number(lighting.at("cloudThickness"), "lighting.cloudThickness", 1, 1000);
            }
            if (lighting.contains("cloudSpeed")) {
                next.scene.lighting.cloudSpeed =
                    number(lighting.at("cloudSpeed"), "lighting.cloudSpeed", 0, 10);
            }
            if (lighting.contains("toneMap")) {
                require(lighting.at("toneMap").is_boolean(), "lighting.toneMap must be boolean");
                next.scene.lighting.toneMap = lighting.at("toneMap").get<bool>();
            }
            if(lighting.contains("quality")) {
                require(lighting.at("quality").is_number_unsigned() || lighting.at("quality").is_number_integer(), "lighting.quality must be integer");
                next.scene.lighting.quality=static_cast<unsigned>(number(lighting.at("quality"),"lighting.quality",0,2));
            }
            if(lighting.contains("maxShadowLights")) {
                require(lighting.at("maxShadowLights").is_number_integer(),"lighting.maxShadowLights must be integer");
                next.scene.lighting.maxShadowLights=static_cast<unsigned>(number(lighting.at("maxShadowLights"),"lighting.maxShadowLights",0,4));
            }
            auto flag=[&](const char* name,bool& target) {
                if(!lighting.contains(name))return;
                require(lighting.at(name).is_boolean(),std::string("lighting.")+name+" must be boolean");
                target=lighting.at(name).get<bool>();
            };
            flag("localShadows",next.scene.lighting.localShadows);flag("ibl",next.scene.lighting.ibl);
            flag("ddgi",next.scene.lighting.ddgi);flag("cloudShadows",next.scene.lighting.cloudShadows);
            flag("temporal",next.scene.lighting.temporal);
            if(lighting.contains("fogVolumes")) {
                const auto& volumes=lighting.at("fogVolumes");
                require(volumes.is_array()&&volumes.size()<=8,"lighting.fogVolumes supports at most 8 volumes");
                for(const auto& entry:volumes) {
                    exactKeys(entry,{"center","extent","density"},"lighting.fogVolumes[]");
                    engine::FogVolume volume;
                    volume.center=vector(entry.at("center"),"fogVolume.center",-100000,100000);
                    volume.extent=vector(entry.at("extent"),"fogVolume.extent",0.001,10000);
                    volume.density=number(entry.at("density"),"fogVolume.density",0,10);
                    next.scene.lighting.fogVolumes.push_back(volume);
                }
            }
        }
        if (data.contains("audio")) {
            const auto& audio = data.at("audio");
            exactKeys(audio, {}, "scene.audio", {"masterGainDb","loudnessTargetLufs","emitters","sounds"});
            next.audio.contentHash=engine::assets::sha256(audio.dump());
            if(audio.contains("sounds")) {
                const auto& sounds=audio.at("sounds");require(sounds.is_object()&&sounds.size()<=64,"audio.sounds supports at most 64 references");
                const auto root=resourceRoot;size_t totalBytes=0;
                std::map<std::string,std::shared_ptr<const engine::audio::Sound>> cache;
                for(const auto& item:sounds.items()) {
                    require(std::regex_match(item.key(),std::regex("^[A-Za-z0-9_./-]{1,64}$")),"Invalid audio sound name");
                    const auto& ref=item.value();exactKeys(ref,{"file","sha256"},"audio.sounds[]");
                    const auto file=ref.at("file").get<std::string>(),hash=ref.at("sha256").get<std::string>();const auto relative=std::filesystem::u8path(file);
                    require(!file.empty()&&file.size()<=512&&!relative.is_absolute()&&relative.extension()==".ngsnd"&&file.find_first_of(":\\") == std::string::npos,"Invalid audio sound path");
                    require(std::none_of(file.begin(),file.end(),[](unsigned char c){return c<32;}),"Invalid audio sound path");
                    for(const auto& part:relative)require(part!=".."&&part!="."&&!part.empty(),"Invalid audio sound path");
                    require(file.find("//")==std::string::npos&&std::regex_match(hash,std::regex("^[a-f0-9]{64}$")),"Invalid audio SHA-256/path");
                    const auto resolved=engine::assets::canonicalFilePath(root/relative),inside=resolved.lexically_relative(root);
                    require(!inside.empty()&&*inside.begin()!="..","Audio dependency escaped scene directory");
                    const auto size=std::filesystem::file_size(resolved);require(size<=64u*1024*1024,"Sound exceeds 64 MiB");
                    std::ifstream stream(resolved,std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());
                    require(bytes.size()==size&&engine::assets::sha256(bytes)==hash,"Audio dependency changed: "+file);
                    auto prior=cache.find(hash);std::shared_ptr<const engine::audio::Sound> sound;
                    if(prior!=cache.end())sound=prior->second;
                    else {totalBytes+=size;require(totalBytes<=256u*1024*1024,"Scene sounds exceed 256 MiB");auto decoded=std::make_shared<engine::audio::Sound>();std::string decodeError;require(engine::audio::decodeNgsndData(bytes,*decoded,decodeError),"Invalid audio dependency: "+decodeError);decoded->name=item.key();sound=decoded;cache[hash]=sound;}
                    next.audio.sounds[item.key()]=std::move(sound);
                }
            }
            if (audio.contains("masterGainDb"))
                next.audio.masterGainDb = number(audio.at("masterGainDb"), "audio.masterGainDb", -60, 6);
            if (audio.contains("loudnessTargetLufs"))
                next.audio.loudnessTargetLufs = number(audio.at("loudnessTargetLufs"), "audio.loudnessTargetLufs", -40, -5);
            if (audio.contains("emitters")) {
                const auto& emitters = audio.at("emitters");
                require(emitters.is_array() && emitters.size() <= 64, "audio.emitters supports at most 64 entries");
                std::set<std::string> emitterIds;
                for (const auto& entry : emitters) {
                    exactKeys(entry, {"id","entity","sound"}, "audio.emitters[]",
                              {"trigger","bus","gainDb","minDistance","maxDistance","loop","occlusion","spatial","offset","pitch"});
                    AudioEmitter emitter;
                    emitter.id = entry.at("id").get<std::string>();
                    require(text(emitter.id) && emitter.id.size() <= 64, "audio emitter id must be 1-64 printable characters");
                    auto lowerId=emitter.id;std::transform(lowerId.begin(),lowerId.end(),lowerId.begin(),[](unsigned char c){return char(std::tolower(c));});
                    require(emitterIds.insert(lowerId).second, "Duplicate audio emitter id: " + emitter.id);
                    emitter.entityId = entry.at("entity").get<std::string>();
                    require(uuid(emitter.entityId), "audio emitter entity must be a UUID");
                    emitter.sound = entry.at("sound").get<std::string>();
                    require(std::regex_match(emitter.sound,std::regex("^[A-Za-z0-9_./-]{1,64}$")), "audio emitter sound must be 1-64 characters");
                    if (entry.contains("trigger")) emitter.trigger = entry.at("trigger").get<std::string>();
                    if (entry.contains("bus")) {
                        emitter.bus = entry.at("bus").get<std::string>();
                        require(emitter.bus == "sfx" || emitter.bus == "ambience" || emitter.bus == "music" ||
                                emitter.bus == "voice" || emitter.bus == "ui",
                                "audio emitter bus must be sfx, ambience, music, voice or ui");
                    }
                    if (entry.contains("gainDb")) emitter.gainDb = number(entry.at("gainDb"), "audio.emitters[].gainDb", -60, 12);
                    if (entry.contains("minDistance")) emitter.minDistance = number(entry.at("minDistance"), "audio.emitters[].minDistance", 0.1, 100);
                    if (entry.contains("maxDistance")) emitter.maxDistance = number(entry.at("maxDistance"), "audio.emitters[].maxDistance", 1, 200);
                    if (entry.contains("loop")) { require(entry.at("loop").is_boolean(), "audio.emitters[].loop must be boolean"); emitter.loop = entry.at("loop").get<bool>(); }
                    if (entry.contains("occlusion")) { require(entry.at("occlusion").is_boolean(), "audio.emitters[].occlusion must be boolean"); emitter.occlusion = entry.at("occlusion").get<bool>(); }
                    if(entry.contains("spatial")){require(entry.at("spatial").is_boolean(),"audio.emitters[].spatial must be boolean");emitter.spatial=entry.at("spatial").get<bool>();}
                    if(entry.contains("offset"))emitter.offset=vector(entry.at("offset"),"audio.emitters[].offset",-100000,100000);
                    if(entry.contains("pitch"))emitter.pitch=number(entry.at("pitch"),"audio.emitters[].pitch",0.1,4);
                    require(emitter.maxDistance>=emitter.minDistance,"audio emitter maxDistance must be at least minDistance");
                    next.audio.emitters.push_back(std::move(emitter));
                }
            }
        }
        const auto& entities = data.at("entities");
        require(entities.is_array() && !entities.empty() && entities.size() <= 10000, "Scene needs 1..10000 entities");
        std::set<std::string> ids;
        const std::set<std::string> kinds = {"ground","wall","prop","player","key","door","goal","light","vehicle"};
        unsigned players = 0, grounds = 0;
        std::map<std::string,int> characterByHash;std::size_t characterSourceBytes=0;unsigned characterInstances=0;bool playerCharacter=false;
        for (const auto& item : entities) {
            exactKeys(item, {"id","name","kind","position","scale","color","collider"},
                      "scene.entities[]",
                      {"material","light","vehicle","physics","destructible","components","mesh","character","imported","heightfield"});
            engine::Entity e;
            if(item.contains("imported")) {
                const auto& origin=item.at("imported");exactKeys(origin,{"asset","part","instance"},"entity.imported");
                for(const auto* key:{"asset","part"})require(origin.at(key).is_string()&&std::regex_match(origin.at(key).get<std::string>(),std::regex("^[a-f0-9]{64}$")),"Invalid imported source key");
                require(origin.at("instance").is_string()&&uuid(origin.at("instance").get<std::string>()),"Invalid imported instance UUID");
                require(item.contains("mesh")||item.contains("character"),"Imported provenance requires asset geometry");
            }
            if (item.contains("mesh")) {
                const auto& ref = item.at("mesh");
                exactKeys(ref, {"file","sha256"}, "entity.mesh");
                e.meshFile = ref.at("file").get<std::string>();
                e.meshHash = ref.at("sha256").get<std::string>();
                require(std::regex_match(e.meshHash,std::regex("^[a-f0-9]{64}$")), "NG-E-ASSET-HASH: invalid mesh SHA-256");
                const auto relative = std::filesystem::u8path(e.meshFile);
                require(!relative.empty() && !relative.is_absolute() && e.meshFile.find(':')==std::string::npos && e.meshFile.find('\\')==std::string::npos,
                    "NG-E-ASSET-PATH: mesh must use a relative slash path");
                for (const auto& part : relative) require(part != "..", "NG-E-ASSET-PATH: mesh escapes scene directory");
                const auto root = resourceRoot;
                const auto meshPath = engine::assets::canonicalFilePath(root / relative);
                const auto resolvedRelative = meshPath.lexically_relative(root);
                require(!resolvedRelative.empty() && *resolvedRelative.begin() != "..", "NG-E-ASSET-PATH: mesh symlink escapes scene directory");
                require(std::filesystem::file_size(meshPath) <= 64*1024*1024, "NG-E-ASSET-BUDGET: mesh exceeds 64 MiB");
                std::ifstream stream(meshPath,std::ios::binary);
                const std::string bytes((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());
                require(engine::assets::sha256(bytes)==e.meshHash, "NG-E-ASSET-HASH: imported mesh changed; reimport asset");
                const auto meshJson=json::parse(bytes);
                exactKeys(meshJson,{"schema","vertices","indices"},"mesh");
                const bool layerUv=meshJson.at("schema")=="nativegpu.mesh/5",tangent=layerUv||meshJson.at("schema")=="nativegpu.mesh/4",vertexColor=tangent||meshJson.at("schema")=="nativegpu.mesh/3",multiUv=vertexColor||meshJson.at("schema")=="nativegpu.mesh/2";
                require(multiUv||meshJson.at("schema")=="nativegpu.mesh/1", "NG-E-ASSET-FORMAT: unsupported mesh schema");
                const auto& vertices=meshJson.at("vertices"); const auto& indices=meshJson.at("indices");
                require(vertices.is_array() && vertices.size()>=3 && vertices.size()<=1000000 && indices.is_array() && indices.size()>=3 && indices.size()%3==0 && indices.size()<=3000000,
                    "NG-E-ASSET-BUDGET: invalid mesh counts");
                engine::ConvexMesh mesh;mesh.hasTextureUvs=multiUv;mesh.hasLayerUvs=layerUv;
                for(const auto& vertex:vertices) {
                    require(vertex.is_array()&&vertex.size()==(layerUv?30:tangent?24:vertexColor?20:multiUv?16:8),"NG-E-ASSET-FORMAT: expected position/normal/UV/color/tangent/layer vertex");
                    engine::ConvexVertex v;
                    for(int k=0;k<3;k++){v.position[k]=number(vertex[k],"mesh.position",-.501,.501);v.normal[k]=number(vertex[k+3],"mesh.normal",-1,1);}
                    for(int k=0;k<2;k++)v.uv[k]=number(vertex[k+6],"mesh.uv",-1000000,1000000);
                    if(multiUv)for(int slot=0;slot<4;slot++)for(int k=0;k<2;k++)v.textureUvs[slot][k]=number(vertex[8+slot*2+k],"mesh.textureUv",-1000000,1000000);
                    if(vertexColor)for(int k=0;k<4;k++)v.color[k]=number(vertex[16+k],"mesh.color",0,1);
                    if(tangent){float length=0,dot=0;for(int k=0;k<4;k++)v.tangent[k]=number(vertex[20+k],"mesh.tangent",-1,1);for(int k=0;k<3;k++){length+=v.tangent[k]*v.tangent[k];dot+=v.tangent[k]*v.normal[k];}require((layerUv&&length==0&&v.tangent[3]==0)||(std::abs(length-1)<.02f&&std::abs(dot)<.02f&&std::abs(v.tangent[3])==1),"NG-E-ASSET-FORMAT: invalid tangent frame");}
                    if(layerUv)for(int slot=0;slot<3;slot++)for(int k=0;k<2;k++)v.layerUvs[slot][k]=number(vertex[24+slot*2+k],"mesh.layerUv",-1000000,1000000);
                    for(int k=0;k<3;k++){mesh.min[k]=std::min(mesh.min[k],v.position[k]);mesh.max[k]=std::max(mesh.max[k],v.position[k]);}
                    mesh.vertices.push_back(v);
                }
                for(const auto& index:indices){require(index.is_number_unsigned() || index.is_number_integer(),"Invalid mesh index");const auto v=index.get<long long>();require(v>=0&&static_cast<std::size_t>(v)<vertices.size(),"Invalid mesh index");mesh.indices.push_back(static_cast<unsigned>(v));}
                require(next.scene.importedMeshes.size()<4096,"NG-E-ASSET-BUDGET: too many imported meshes");
                e.importedMesh=static_cast<int>(next.scene.importedMeshes.size());
                next.scene.importedMeshes.push_back(std::move(mesh));
            }
            if (item.contains("heightfield")) {
                const auto& field = item.at("heightfield");
                exactKeys(field, {"file","sha256","enabled","origin","size","minHeightMetres","maxHeightMetres","maxErrorMetres"}, "entity.heightfield");
                const auto file = field.at("file").get<std::string>(), hash = field.at("sha256").get<std::string>();
                const auto relative = std::filesystem::u8path(file);
                require(!file.empty() && !relative.is_absolute() && relative.extension()==".ngtex" && file.find(':')==std::string::npos && file.find('\\')==std::string::npos, "NG-E-ASSET-HEIGHTFIELD: invalid path");
                for (const auto& part : relative) require(part != ".." && part != ".", "NG-E-ASSET-HEIGHTFIELD: invalid path");
                require(std::regex_match(hash,std::regex("^[a-f0-9]{64}$")), "NG-E-ASSET-HEIGHTFIELD: invalid SHA-256");
                const auto path = engine::assets::canonicalFilePath(resourceRoot / relative);
                const auto inside = path.lexically_relative(resourceRoot);
                require(!inside.empty() && *inside.begin() != "..", "NG-E-ASSET-HEIGHTFIELD: dependency escaped the asset root");
                require(std::filesystem::file_size(path) <= 16ull*1024*1024, "NG-E-ASSET-HEIGHTFIELD: asset exceeds 16 MiB");
                std::ifstream stream(path, std::ios::binary);
                const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
                require(engine::assets::sha256(bytes) == hash, "NG-E-ASSET-HEIGHTFIELD: heightfield changed; rebake it");
                require(bytes.size() >= 32 && bytes.compare(0,4,"NGTX") == 0, "NG-E-ASSET-HEIGHTFIELD: bad container");
                const auto u16=[&](std::size_t at){return static_cast<unsigned>(static_cast<unsigned char>(bytes[at]) | (static_cast<unsigned char>(bytes[at+1])<<8));};
                const auto u32=[&](std::size_t at){return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at]) | (static_cast<unsigned char>(bytes[at+1])<<8) | (static_cast<unsigned char>(bytes[at+2])<<16) | (static_cast<unsigned char>(bytes[at+3])<<24));};
                require(u16(4) == 1 && u16(6) == 9 && bytes[13] == 5, "NG-E-ASSET-HEIGHTFIELD: requires linear R16 height data");
                const unsigned width = u16(8), height = u16(10);
                require(width > 1 && height > 1 && bytes[12] >= 1 && 32 + bytes[12]*8 <= bytes.size(), "NG-E-ASSET-HEIGHTFIELD: invalid dimensions");
                const std::uint32_t offset = u32(32), size = u32(36);
                require(offset + size <= bytes.size() && size == static_cast<std::uint64_t>(width) * height * 2, "NG-E-ASSET-HEIGHTFIELD: invalid mip payload");
                const auto& originJson = field.at("origin"); const auto& extentJson = field.at("size");
                require(originJson.is_array() && originJson.size()==2 && extentJson.is_array() && extentJson.size()==2, "NG-E-ASSET-HEIGHTFIELD: origin/size must have two components");
                const float originX = number(originJson[0], "heightfield.origin", -1000000, 1000000), originZ = number(originJson[1], "heightfield.origin", -1000000, 1000000);
                const float sizeX = number(extentJson[0], "heightfield.size", .01f, 1000000), sizeZ = number(extentJson[1], "heightfield.size", .01f, 1000000);
                const float minimum = number(field.at("minHeightMetres"), "heightfield.minHeightMetres", -1000, 1000);
                const float maximum = number(field.at("maxHeightMetres"), "heightfield.maxHeightMetres", -1000, 1000);
                require(maximum >= minimum, "NG-E-ASSET-HEIGHTFIELD: maximum height is below minimum");
                require(field.at("enabled").is_boolean(), "NG-E-ASSET-HEIGHTFIELD: enabled must be boolean");
                e.heightfieldEnabled = field.at("enabled").get<bool>();
                e.hasHeightfield = true;
                e.heightfieldOrigin[0] = originX; e.heightfieldOrigin[1] = originZ;
                e.heightfieldSize[0] = sizeX; e.heightfieldSize[1] = sizeZ;
                e.heightfieldWidth = width; e.heightfieldHeight = height;
                e.heightfieldMaxError = number(field.at("maxErrorMetres"), "heightfield.maxErrorMetres", 0, 1);
                e.heightfieldHeights.resize(static_cast<std::size_t>(width) * height);
                const float span = maximum - minimum;
                for (std::size_t sample = 0; sample < e.heightfieldHeights.size(); ++sample) {
                    const unsigned value = static_cast<unsigned>(static_cast<unsigned char>(bytes[offset + sample*2]) | (static_cast<unsigned char>(bytes[offset + sample*2+1])<<8));
                    e.heightfieldHeights[sample] = minimum + (static_cast<float>(value) / 65535.0f) * span;
                }
            }
            e.id = item.at("id").get<std::string>();
            std::string canonicalId = e.id;
            std::transform(canonicalId.begin(), canonicalId.end(), canonicalId.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            require(uuid(e.id) && ids.insert(canonicalId).second,
                    "Invalid or duplicate entity UUID");
            e.name = item.at("name").get<std::string>();
            require(text(e.name), "Entity name must contain 1-128 printable characters");
            e.kind = item.at("kind").get<std::string>();
            require(kinds.count(e.kind) != 0, "Unknown entity kind: " + e.kind);
            e.position = vector(item.at("position"), "position", -100000, 100000);
            e.scale = vector(item.at("scale"), "scale", 0.001, 10000);
            if(item.contains("character")) {
                require(!item.contains("mesh")&&!item.contains("destructible"),"Character geometry cannot also use a rigid mesh or fracture asset");
                const auto& value=item.at("character");exactKeys(value,{"file","sha256","binarySha256"},"entity.character",{"clip","speed","timeOffset","yaw","scale","offset","rootMotion","materials","anchor","controller","rig"});
                auto& ref=e.character;ref.authoring=value;ref.file=value.at("file").get<std::string>();ref.sha256=value.at("sha256").get<std::string>();ref.binarySha256=value.at("binarySha256").get<std::string>();
                const std::regex hashPattern("^[a-f0-9]{64}$");require(std::regex_match(ref.sha256,hashPattern)&&std::regex_match(ref.binarySha256,hashPattern),"NG-E-ASSET-HASH: invalid character SHA-256");
                const auto relative=std::filesystem::u8path(ref.file);require(!relative.empty()&&!relative.is_absolute()&&ref.file.find(':')==std::string::npos&&ref.file.find('\\')==std::string::npos&&ref.file.find('\0')==std::string::npos&&relative.extension()==".json","NG-E-ASSET-PATH: character must reference a relative JSON header");
                for(const auto& part:relative)require(part!="..","NG-E-ASSET-PATH: character escapes scene directory");
                const auto root=resourceRoot;
                auto verified=[&](const std::filesystem::path& file,const std::string& expected,std::uintmax_t limit) {
                    const auto resolved=engine::assets::canonicalFilePath(file),local=resolved.lexically_relative(root);
                    require(!local.empty()&&*local.begin()!="..","NG-E-ASSET-PATH: character symlink escapes scene directory");
                    require(std::filesystem::file_size(resolved)>0&&std::filesystem::file_size(resolved)<=limit,"NG-E-ASSET-BUDGET: invalid character file size");
                    std::ifstream stream(resolved,std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(stream)),std::istreambuf_iterator<char>());
                    require(engine::assets::sha256(bytes)==expected,"NG-E-ASSET-HASH: character dependency changed; reimport asset");return std::vector<std::uint8_t>(bytes.begin(),bytes.end());
                };
                const auto header=verified(root/relative,ref.sha256,4*1024*1024),binary=verified((root/relative).parent_path()/"animation.bin",ref.binarySha256,64*1024*1024);
                const auto key=ref.sha256+":"+ref.binarySha256;auto found=characterByHash.find(key);
                if(found==characterByHash.end()) {
                    require(characterSourceBytes+header.size()+binary.size()<=256*1024*1024,"NG-E-ASSET-BUDGET: scene character sources exceed 256 MiB");characterSourceBytes+=header.size()+binary.size();
                    const auto index=static_cast<int>(next.scene.characterAssets.size());next.scene.characterAssets.push_back(engine::animation::loadAssetData(header,binary));found=characterByHash.emplace(key,index).first;
                }
                e.characterAsset=found->second;const auto& asset=*next.scene.characterAssets[static_cast<std::size_t>(e.characterAsset)];
                ref.clip=value.value("clip",std::string());asset.clipIndex(ref.clip.empty()?asset.controllerClips[0]:ref.clip);
                ref.speed=value.contains("speed")?number(value.at("speed"),"character.speed",0,16):1;
                ref.timeOffset=value.contains("timeOffset")?number(value.at("timeOffset"),"character.timeOffset",0,120):0;
                require(e.kind!="player"||!ref.clip.empty()||(ref.speed==1&&ref.timeOffset==0),"Player locomotion timing is controlled by gameplay; choose a fixed clip for speed/timeOffset overrides");
                ref.yaw=value.contains("yaw")?number(value.at("yaw"),"character.yaw",-1000,1000):0;
                if(value.contains("scale"))ref.scale=vector(value.at("scale"),"character.scale",.0001,10000);
                if(value.contains("offset"))ref.offset=vector(value.at("offset"),"character.offset",-100000,100000);
                ref.rootMotion=value.value("rootMotion",std::string("asset"));require(ref.rootMotion=="asset"||ref.rootMotion=="preserve"||ref.rootMotion=="extract","Invalid character rootMotion policy");
                ref.anchor=value.value("anchor",std::string("center"));require((ref.anchor=="feet"||ref.anchor=="center")&&(e.kind!="player"||!value.contains("anchor")||ref.anchor=="feet"),"Invalid character anchor (player uses feet)");
                if(value.contains("controller")) {
                    require(e.kind=="player","character.controller requires a player entity");const auto& profile=value.at("controller");exactKeys(profile,{},"character.controller",{"stepHeight","eyeHeight","interactionHeight","orbitDistance","orbitHeight","targetHeight"});
                    for(const auto& field:profile.items())number(field.value(),"character.controller."+field.key(),field.key()=="orbitDistance"?.001:0,100000);ref.controller=profile;
                }
                if(value.contains("rig")) {
                    require(e.kind=="player","character.rig requires a player entity");const auto& profile=value.at("rig");exactKeys(profile,{},"character.rig",{"enabled","footLock","turnInPlace","height","turnThreshold","turnStepHeight","turnStepDuration","turnCooldown","maxTwist","maxSlopeCosine","swingPrediction","maxFootCorrection","pelvisLower","pelvisUpper","maxStepHeight","maxPelvisShift","soleRoll"});
                    for(const auto& field:profile.items()){const auto& key=field.key();
                        if(key=="enabled"||key=="footLock"||key=="turnInPlace"){require(field.value().is_boolean(),"character.rig."+key+" must be boolean");continue;}
                        float low=0,high=10;
                        if(key=="height"){low=.1f;high=100.f;}
                        else if(key=="turnThreshold"){low=.05f;high=3.14f;}
                        else if(key=="turnStepHeight"){high=5;}
                        else if(key=="turnStepDuration"){low=.01f;high=10;}
                        else if(key=="turnCooldown"){high=10;}
                        else if(key=="maxTwist"){high=6.28f;}
                        else if(key=="maxSlopeCosine"){high=1;}
                        else if(key=="pelvisLower"){low=-10;high=0;}
                        else if(key=="pelvisUpper"){high=10;}
                        else if(key=="maxPelvisShift"){low=0;high=1;}
                        else if(key=="soleRoll"){low=0;high=1.5f;}
                        else if(key=="swingPrediction"||key=="maxFootCorrection"||key=="maxStepHeight"){high=10;}
                        number(field.value(),"character.rig."+key,low,high);
                    }
                    ref.rig=profile;
                }
                if(value.contains("materials")){require(value.at("materials").is_object()&&value.at("materials").size()<=256,"Invalid character material map");for(const auto& material:value.at("materials").items())require(std::any_of(asset.meshParts.begin(),asset.meshParts.end(),[&](const auto& part){return part.material==material.key();}),"Character material map references an unknown source slot");}
                for(const auto& part:asset.meshParts) {
                    const auto name=value.contains("materials")?value.at("materials").value(part.material,part.material):part.material;
                    const auto material=materialByName.find(name);require(material!=materialByName.end(),"Missing scene material for character part: "+name);ref.materials.push_back(static_cast<int>(material->second));
                }
                ++characterInstances;if(e.kind=="player")playerCharacter=true;
            }
            auto c = vector(item.at("color"), "color", 0, 1);
            e.color[0]=c.x; e.color[1]=c.y; e.color[2]=c.z;
            if (item.contains("material")) {
                const auto& material = item.at("material");
                if (material.is_string()) {
                    require(schemaVersion == 2, "Named material references require schemaVersion 2");
                    const std::string reference = material.get<std::string>();
                    const auto found = materialByName.find(reference);
                    require(found != materialByName.end(),
                            "Unknown material reference: " + reference);
                    e.materialIndex = static_cast<int>(found->second);
                    const engine::MaterialDefinition& definition =
                        next.scene.materials[found->second];
                    e.material.roughness = definition.roughness;
                    e.material.metalness = definition.metalness;
                    e.hasMaterial = true;
                } else {
                    exactKeys(material, {"albedo","roughness","metalness","pattern"},
                              "scene.entities[].material");
                    auto albedo = vector(material.at("albedo"), "material.albedo", 0, 1);
                    e.material.albedo[0]=albedo.x; e.material.albedo[1]=albedo.y; e.material.albedo[2]=albedo.z;
                    e.material.roughness = number(material.at("roughness"), "material.roughness", 0, 1);
                    e.material.metalness = number(material.at("metalness"), "material.metalness", 0, 1);
                    e.material.pattern = material.at("pattern").get<std::string>();
                    require(e.material.pattern == "checker" || e.material.pattern == "brick" ||
                            e.material.pattern == "metal", "material.pattern must be checker, brick or metal");
                    e.hasMaterial = true;
                }
            }
            if (item.contains("vehicle")) {
                require(e.kind == "vehicle", "vehicle is only supported on a vehicle entity");
                const auto& vehicle = item.at("vehicle");
                exactKeys(vehicle, {"speed"}, "scene.entities[].vehicle", {"direction"});
                e.vehicleSpeed = number(vehicle.at("speed"), "vehicle.speed", 0, 80);
                if (vehicle.contains("direction")) {
                    const float direction = number(vehicle.at("direction"),
                                                   "vehicle.direction", -1, 1);
                    require(direction == 1.0f || direction == -1.0f,
                            "vehicle.direction must be 1 or -1");
                    e.vehicleDirection = direction > 0.0f ? 1 : -1;
                }
            } else if (e.kind == "vehicle") {
                require(false, "vehicle entity requires a vehicle object");
            }
            if (e.kind == "light") {
                require(item.contains("light"), "light entity requires a light object");
                const auto& light = item.at("light");
                exactKeys(light, {"color","intensity","range"}, "scene.entities[].light",
                          {"castSurfaceShadow","castVolumetricShadow","volumetricIntensity"});
                auto lightColor = vector(light.at("color"), "light.color", 0, 1);
                e.lightColor[0]=lightColor.x; e.lightColor[1]=lightColor.y; e.lightColor[2]=lightColor.z;
                e.lightIntensity = number(light.at("intensity"), "light.intensity", 0.001, 100);
                e.lightRange = number(light.at("range"), "light.range", 0.001, 1000);
                require(next.scene.lights.size() < 64, "At most 64 lights are supported");
                engine::Light sceneLight;
                sceneLight.position = e.position;
                std::copy(e.lightColor, e.lightColor + 3, sceneLight.color);
                sceneLight.intensity = e.lightIntensity;
                sceneLight.range = e.lightRange;
                if(light.contains("castSurfaceShadow")) {
                    require(light.at("castSurfaceShadow").is_boolean(),"light.castSurfaceShadow must be boolean");
                    e.castSurfaceShadow=sceneLight.castSurfaceShadow=light.at("castSurfaceShadow").get<bool>();
                }
                if(light.contains("castVolumetricShadow")) {
                    require(light.at("castVolumetricShadow").is_boolean(),"light.castVolumetricShadow must be boolean");
                    e.castVolumetricShadow=sceneLight.castVolumetricShadow=light.at("castVolumetricShadow").get<bool>();
                }
                if(light.contains("volumetricIntensity")) e.volumetricIntensity=sceneLight.volumetricIntensity=
                    number(light.at("volumetricIntensity"),"light.volumetricIntensity",0,10);
                next.scene.lights.push_back(sceneLight);
                e.hasLight = true;
            } else {
                require(!item.contains("light"), "light is only supported on a light entity");
            }
            e.collider = item.at("collider").get<bool>();
            if (item.contains("physics")) {
                require(e.kind == "ground" || e.kind == "wall" || e.kind == "prop",
                        "physics is only supported on ground, wall and prop entities");
                const auto& physics = item.at("physics");
                exactKeys(physics, {"mode"}, "scene.entities[].physics",
                          {"mass","friction","restitution"});
                const std::string mode = physics.at("mode").get<std::string>();
                require(mode == "static" || mode == "dynamic",
                        "physics.mode must be static or dynamic");
                require(!(mode == "dynamic" && e.kind == "ground"),
                        "ground entities cannot be dynamic");
                e.hasPhysics = true;
                e.dynamicBody = mode == "dynamic";
                if (physics.contains("mass")) {
                    e.physicsMass = number(physics.at("mass"), "physics.mass", 0.05, 1000);
                }
                if (physics.contains("friction")) {
                    e.friction = number(physics.at("friction"), "physics.friction", 0, 1.5);
                }
                if (physics.contains("restitution")) {
                    e.restitution = number(physics.at("restitution"),
                                           "physics.restitution", 0, 0.95);
                }
                require(!e.dynamicBody || e.collider,
                        "a dynamic physics body requires collider=true");
            }
            if (item.contains("destructible")) {
                const auto& destructible = item.at("destructible");
                    exactKeys(destructible, {"enabled","strength","fragments"},
                              "scene.entities[].destructible",
                              {"debrisLifetime","structural","seed","mode","cells","voronoi"});
                require(destructible.at("enabled").is_boolean(),
                        "destructible.enabled must be boolean");
                const bool enabled = destructible.at("enabled").get<bool>();
                if (enabled) {
                    require(e.kind == "wall" || e.kind == "prop",
                            "destructible is only supported on wall and prop entities");
                    require(e.collider, "a destructible entity requires collider=true");
                    e.destructible = true;
                    e.strength = number(destructible.at("strength"),
                                        "destructible.strength", 1, 1000000);
                    require(destructible.at("fragments").is_number_integer(),
                            "destructible.fragments must be an integer");
                    const int fragments = destructible.at("fragments").get<int>();
                    require(fragments >= 4 && fragments <= 28,
                            "destructible.fragments must be in [4, 28]");
                    e.fragments = fragments;
                    if (destructible.contains("debrisLifetime")) {
                        e.debrisLifetime = number(destructible.at("debrisLifetime"),
                                                  "destructible.debrisLifetime", 1, 120);
                    }
                    if (destructible.contains("structural")) {
                        require(destructible.at("structural").is_boolean(),
                                "destructible.structural must be boolean");
                        e.structural = destructible.at("structural").get<bool>();
                    }
                    if (destructible.contains("mode")) {
                        require(destructible.at("mode").is_string(),
                                "destructible.mode must be a string");
                        const std::string mode = destructible.at("mode").get<std::string>();
                        require(mode == "entity" || mode == "bonded",
                                "destructible.mode must be \"entity\" or \"bonded\"");
                        e.bonded = mode == "bonded";
                    }
                    if (destructible.contains("cells")) {
                        require(destructible.at("cells").is_number_integer(),
                                "destructible.cells must be an integer");
                        const int cells = destructible.at("cells").get<int>();
                        require(cells >= 1 && cells <= 256,
                                "destructible.cells must be in [1, 256]");
                        e.bondedCells = cells;
                    }
                    if (destructible.contains("seed")) {
                        require(destructible.at("seed").is_number_integer(),
                                "destructible.seed must be an integer");
                        const long long seed = destructible.at("seed").get<long long>();
                        require(seed >= 0 && seed <= 4294967295LL,
                                "destructible.seed must be in [0, 4294967295]");
                        e.fractureSeed = static_cast<unsigned>(seed);
                    }
                    if (destructible.contains("voronoi")) {
                        require(destructible.at("voronoi").is_string(),
                                "destructible.voronoi must be a path string");
                        const std::string path = destructible.at("voronoi").get<std::string>();
                        require(!path.empty() && path.size() < 260,
                                "destructible.voronoi must be a non-empty path");
                        require(e.bonded,
                                "destructible.voronoi requires mode \"bonded\"");
                        e.voronoi = path;
                    }
                }
            }
            players += e.kind == "player"; grounds += e.kind == "ground";
            if(item.contains("components")) {
                require(item["components"].is_object(),"components must be an object");
                for(auto it=item["components"].begin();it!=item["components"].end();++it) e.components[it.key()]=componentRegistry.normalize(it.key(),it.value());
            }
            next.scene.entities.push_back(e);
        }
        require(players == 1 && grounds >= 1, "Scene needs exactly one player and at least one ground");
        if (data.contains("joints")) {
            require(data.at("joints").is_array(), "scene.joints must be an array");
            require(data.at("joints").size() <= 256, "scene.joints exceeds 256 entries");
            std::set<std::string> entityIds;
            for (const auto& entity : next.scene.entities) entityIds.insert(entity.id);
            for (const auto& item : data.at("joints")) {
                exactKeys(item, {"type","bodyA"}, "scene.joints[]",
                          {"bodyB","anchorA","anchorB","axis","hingeSpan","restLength",
                           "compliance","lowerLimit","upperLimit","breakForce",
                           "collideConnected","motorSpeed","motorMaxTorque","friction"});
                engine::SceneJoint joint;
                require(item.at("type").is_string(), "joint.type must be a string");
                joint.type = item.at("type").get<std::string>();
                require(joint.type == "point" || joint.type == "hinge" || joint.type == "rope",
                        "joint.type must be \"point\", \"hinge\" or \"rope\"");
                require(item.at("bodyA").is_string(), "joint.bodyA must be an entity id");
                joint.bodyA = item.at("bodyA").get<std::string>();
                require(entityIds.count(joint.bodyA) != 0,
                        "joint.bodyA names an entity that does not exist: " + joint.bodyA);
                if (item.contains("bodyB")) {
                    require(item.at("bodyB").is_string(), "joint.bodyB must be an entity id");
                    joint.bodyB = item.at("bodyB").get<std::string>();
                    require(entityIds.count(joint.bodyB) != 0,
                            "joint.bodyB names an entity that does not exist: " + joint.bodyB);
                }
                if (item.contains("anchorA")) {
                    joint.anchorA = vector(item.at("anchorA"), "joint.anchorA", -1000, 1000);
                }
                if (item.contains("anchorB")) {
                    joint.anchorB = vector(item.at("anchorB"), "joint.anchorB", -1000, 1000);
                }
                if (item.contains("axis")) {
                    joint.axis = vector(item.at("axis"), "joint.axis", -1, 1);
                    const float length = std::sqrt(joint.axis.x * joint.axis.x +
                                                   joint.axis.y * joint.axis.y +
                                                   joint.axis.z * joint.axis.z);
                    require(length > 1.0e-3f, "joint.axis must not be zero");
                }
                if (item.contains("hingeSpan")) {
                    joint.hingeSpan = number(item.at("hingeSpan"), "joint.hingeSpan", 0.01, 10);
                }
                if (item.contains("restLength")) {
                    joint.restLength = number(item.at("restLength"), "joint.restLength", 0, 100);
                }
                if (item.contains("compliance")) {
                    joint.compliance = number(item.at("compliance"), "joint.compliance", 0, 1);
                }
                if (item.contains("lowerLimit")) {
                    joint.lowerLimit = number(item.at("lowerLimit"), "joint.lowerLimit", -3.15, 3.15);
                }
                if (item.contains("upperLimit")) {
                    joint.upperLimit = number(item.at("upperLimit"), "joint.upperLimit", -3.15, 3.15);
                }
                require(joint.lowerLimit <= joint.upperLimit,
                        "joint.lowerLimit must not exceed upperLimit");
                if (item.contains("breakForce")) {
                    joint.breakForce = number(item.at("breakForce"), "joint.breakForce", 0, 1.0e6);
                }
                if (item.contains("collideConnected")) {
                    require(item.at("collideConnected").is_boolean(),
                            "joint.collideConnected must be boolean");
                    joint.collideConnected = item.at("collideConnected").get<bool>();
                }
                if (item.contains("motorSpeed")) {
                    joint.motorSpeed = number(item.at("motorSpeed"), "joint.motorSpeed", -100, 100);
                }
                if (item.contains("motorMaxTorque")) {
                    joint.motorMaxTorque = number(item.at("motorMaxTorque"),
                                                  "joint.motorMaxTorque", 0, 1.0e6);
                }
                if (item.contains("friction")) {
                    joint.friction = number(item.at("friction"), "joint.friction", 0, 1);
                }
                require(joint.motorMaxTorque <= 0.0f || joint.type == "hinge",
                        "joint.motorMaxTorque is only supported on hinge joints");
                require(joint.friction <= 0.0f || joint.type == "hinge",
                        "joint.friction is only supported on hinge joints");
                next.scene.joints.push_back(joint);
            }
        }
        // Baked Voronoi shard sets. Resolved after the entity loop so two entities that
        // name the same package share one upload, and loaded relative to the scene file
        // so a packaged build keeps working from any working directory.
        {
            const std::filesystem::path sceneDir = path.parent_path();
            for (const auto& entity : next.scene.entities) {
                if (entity.voronoi.empty()) continue;
                bool known = false;
                for (const auto& existing : next.scene.voronoiPackages) {
                    if (existing.source == entity.voronoi) { known = true; break; }
                }
                if (known) continue;
                std::filesystem::path assetPath = entity.voronoi;
                if (assetPath.is_relative() && !sceneDir.empty() &&
                    std::filesystem::exists(engine::assets::nativeFilePath(sceneDir / assetPath))) {
                    assetPath = sceneDir / assetPath;
                }
                next.scene.voronoiPackages.push_back(
                    loadVoronoiPackage(assetPath, entity.voronoi));
            }
        }
        const auto& settings = data.at("settings");
        for(auto& emitter:next.audio.emitters) {
            auto folded=[](std::string s){std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));});return s;};
            const auto found=std::find_if(next.scene.entities.begin(),next.scene.entities.end(),[&](const auto& entity){return folded(entity.id)==folded(emitter.entityId);});
            require(found!=next.scene.entities.end(),"Audio emitter entity does not exist");emitter.entityId=found->id;
        }
        require(characterInstances+(playerCharacter?0:1)<=64,"NG-E-ASSET-BUDGET: at most 64 character instances including the player");
        exactKeys(settings, {"walkSpeed","runSpeed","jumpSpeed","gravity"}, "scene.settings");
        next.scene.walkSpeed=number(settings.at("walkSpeed"),"walkSpeed",0.1,30);
        next.scene.runSpeed=number(settings.at("runSpeed"),"runSpeed",0.1,60);
        require(next.scene.runSpeed>=next.scene.walkSpeed,"runSpeed must be >= walkSpeed");
        next.scene.jumpSpeed=number(settings.at("jumpSpeed"),"jumpSpeed",0.1,30);
        next.scene.gravity=number(settings.at("gravity"),"gravity",0.1,100);
        engine::gameplay::Runtime scripting;scripting.initialize(next.scene.assetRoot);
        for(const auto& e:next.scene.entities)scripting.world.create(e.id);
        for(const auto& e:next.scene.entities) {
            if(e.components.contains("behavior"))for(const auto& b:e.components["behavior"]["instances"])scripting.attach(e.id,b.at("id"),b.at("script"),b.value("properties",engine::gameplay::Json::object()));
            else if(e.kind=="key")scripting.attach(e.id,"pickup","gameplay/pickup");
            else if(e.kind=="door")scripting.attach(e.id,"lock","gameplay/locked_door");
            else if(e.kind=="goal")scripting.attach(e.id,"goal","gameplay/goal");
        }
        result=std::move(next);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error=std::string("SCENE_INVALID: ") + e.what();
        return false;
    }
}
}
