#include "../../third_party/json.hpp"
#include "animation.hpp"
#include "assets/file_path.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace engine::animation {
namespace {
using json = nlohmann::json;
void require(bool ok, const char *code, const std::string &text) {
  if (!ok)
    throw Error(code, text);
}
std::vector<std::uint8_t> read(const std::filesystem::path &inputFile,
                               std::uintmax_t limit) {
  const auto file=engine::assets::nativeFilePath(inputFile);
  std::error_code ec;
  auto size = std::filesystem::file_size(file, ec);
  require(!ec && size > 0 && size <= limit, "NG-E-ANIM-IO",
          "Missing or oversized asset: " + file.u8string());
  std::ifstream f(file, std::ios::binary);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  f.read(reinterpret_cast<char *>(bytes.data()),
         static_cast<std::streamsize>(size));
  require(bool(f), "NG-E-ANIM-IO", "Cannot read asset");
  return bytes;
}
float number(const json &value) {
  require(value.is_number(), "NG-E-ANIM-FORMAT", "Expected finite number");
  float f = value.get<float>();
  require(std::isfinite(f) && std::abs(f) <= 100000, "NG-E-ANIM-FORMAT",
          "Number exceeds range");
  return f;
}
std::size_t size(const json &value, std::size_t max) {
  require(value.is_number_unsigned() || value.is_number_integer(),
          "NG-E-ANIM-FORMAT", "Expected integer");
  auto n = value.get<std::int64_t>();
  require(n >= 0 && static_cast<std::uint64_t>(n) <= max, "NG-E-ANIM-FORMAT",
          "Integer exceeds range");
  return static_cast<std::size_t>(n);
}
Transform transform(const json &value) {
  require(value.is_array() && value.size() == 10, "NG-E-ANIM-FORMAT",
          "Transform must contain 10 numbers");
  Transform t{{number(value[0]), number(value[1]), number(value[2])},
              normalized({number(value[3]), number(value[4]), number(value[5]),
                          number(value[6])}),
              {number(value[7]), number(value[8]), number(value[9])}};
  return t;
}
std::uint32_t u32(const std::vector<std::uint8_t> &bytes, std::size_t at) {
  require(at + 4 <= bytes.size(), "NG-E-ANIM-FORMAT",
          "Binary section exceeds file");
  return std::uint32_t(bytes[at]) | (std::uint32_t(bytes[at + 1]) << 8) |
         (std::uint32_t(bytes[at + 2]) << 16) |
         (std::uint32_t(bytes[at + 3]) << 24);
}
float f32(const std::vector<std::uint8_t> &bytes, std::size_t at) {
  auto bits = u32(bytes, at);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
} // namespace
std::shared_ptr<const Asset> loadAsset(const std::filesystem::path &header) {
  return loadAssetData(read(header,4*1024*1024),read(header.parent_path()/"animation.bin",64*1024*1024));
}
std::shared_ptr<const Asset> loadAssetData(const std::vector<std::uint8_t>& text,const std::vector<std::uint8_t>& bytes) {
  try {
    require(!text.empty()&&text.size()<=4*1024*1024&&!bytes.empty()&&bytes.size()<=64*1024*1024,"NG-E-ANIM-FORMAT","Invalid animation payload size");
    auto data = json::parse(text.begin(), text.end());
    require((data.at("format") == "nativegpu.animation/1" ||
             data.at("format") == "nativegpu.animation/2") &&
                data.at("sampleRate") == SampleRate &&
                data.at("quantization") == 10000,
            "NG-E-ANIM-FORMAT", "Unsupported animation format");
    require(data.at("binary") == "animation.bin", "NG-E-ANIM-FORMAT",
            "Binary must be the sibling animation.bin");
    require(bytes.size() == size(data.at("binaryBytes"), 64 * 1024 * 1024),
            "NG-E-ANIM-FORMAT", "Binary byte count mismatch");
    determinism::Hasher binaryHasher;
    for (auto b : bytes)
      binaryHasher.u8(b);
    require(data.at("binaryHash") == binaryHasher.hex(), "NG-E-ANIM-HASH",
            "Animation binary hash mismatch");
    auto out = std::make_shared<Asset>();
    determinism::Hasher fingerprint;
    for (auto b : text)
      fingerprint.u8(b);
    for (auto b : bytes)
      fingerprint.u8(b);
    out->contentHash = fingerprint.hex();
    const auto &bones = data.at("bones");
    require(bones.is_array() && !bones.empty() && bones.size() <= MaxBones,
            "NG-E-ANIM-SKELETON", "Invalid bones");
    for (const auto &b : bones) {
      Bone bone;
      bone.name = b.at("name").get<std::string>();
      require(b.at("parent").is_number_integer(), "NG-E-ANIM-SKELETON",
              "Parent must be an integer");
      auto parent = b.at("parent").get<std::int64_t>();
      require(parent >= -1 && parent < static_cast<std::int64_t>(
                                           out->skeleton.bones.size()),
              "NG-E-ANIM-SKELETON", "Invalid parent");
      bone.parent = static_cast<int>(parent);
      bone.rest = transform(b.at("rest"));
      auto inverse = b.at("inverseBind");
      require(inverse.is_array() && inverse.size() == 16, "NG-E-ANIM-SKELETON",
              "Invalid inverse bind");
      for (unsigned i = 0; i < 16; ++i)
        bone.inverseBind[i] = number(inverse[i]);
      out->skeleton.bones.push_back(bone);
    }
    const auto &mesh = data.at("mesh");
    auto vertices = size(mesh.at("vertices"), 100000),
         indices = size(mesh.at("indices"), 3000000);
    auto vertexOffset = size(mesh.at("vertexOffset"), bytes.size()),
         indexOffset = size(mesh.at("indexOffset"), bytes.size());
    std::size_t textureUvOffset=vertices*64,textureUvBytes=0;
    if(mesh.contains("textureUvOffset")||mesh.contains("textureUvBytes")){
      require(mesh.contains("textureUvOffset")&&mesh.contains("textureUvBytes"),"NG-E-ANIM-FORMAT","Incomplete texture UV section");
      textureUvOffset=size(mesh.at("textureUvOffset"),bytes.size());textureUvBytes=size(mesh.at("textureUvBytes"),bytes.size());
      require(textureUvOffset==vertices*64&&textureUvBytes==vertices*32,"NG-E-ANIM-FORMAT","Invalid texture UV section");out->hasTextureUvs=true;
    }
    std::size_t colorOffset=textureUvOffset+textureUvBytes,colorBytes=0;
    if(mesh.contains("colorOffset")||mesh.contains("colorBytes")){
      require(mesh.contains("colorOffset")&&mesh.contains("colorBytes"),"NG-E-ANIM-FORMAT","Incomplete color section");
      colorOffset=size(mesh.at("colorOffset"),bytes.size());colorBytes=size(mesh.at("colorBytes"),bytes.size());
      require(colorOffset==textureUvOffset+textureUvBytes&&colorBytes==vertices*16,"NG-E-ANIM-FORMAT","Invalid color section");out->hasVertexColors=true;
    }
    std::size_t tangentOffset=colorOffset+colorBytes,tangentBytes=0;
    if(mesh.contains("tangentOffset")||mesh.contains("tangentBytes")){
      require(mesh.contains("tangentOffset")&&mesh.contains("tangentBytes"),"NG-E-ANIM-FORMAT","Incomplete tangent section");
      tangentOffset=size(mesh.at("tangentOffset"),bytes.size());tangentBytes=size(mesh.at("tangentBytes"),bytes.size());
      require(tangentOffset==colorOffset+colorBytes&&tangentBytes==vertices*16,"NG-E-ANIM-FORMAT","Invalid tangent section");out->hasTangents=true;
    }
    std::size_t layerUvOffset=tangentOffset+tangentBytes,layerUvBytes=0;
    if(mesh.contains("layerUvOffset")||mesh.contains("layerUvBytes")){
      require(mesh.contains("layerUvOffset")&&mesh.contains("layerUvBytes"),"NG-E-ANIM-FORMAT","Incomplete layer UV section");
      layerUvOffset=size(mesh.at("layerUvOffset"),bytes.size());layerUvBytes=size(mesh.at("layerUvBytes"),bytes.size());
      require(layerUvOffset==tangentOffset+tangentBytes&&layerUvBytes==vertices*24,"NG-E-ANIM-FORMAT","Invalid layer UV section");out->hasLayerUvs=true;
    }
    require(vertices > 0 && indices > 0 && vertexOffset == 0 &&
                indexOffset == layerUvOffset + layerUvBytes &&
                indexOffset + indices * 4 <= bytes.size(),
            "NG-E-ANIM-FORMAT", "Invalid mesh sections");
    out->vertices.resize(vertices);
    out->indices.resize(indices);
    if(mesh.contains("parts")) {
      const auto& parts=mesh.at("parts");
      require(parts.is_array()&&!parts.empty()&&parts.size()<=256,"NG-E-ANIM-SKIN","Invalid mesh part count");
      unsigned next=0;
      for(const auto& value:parts) {
        MeshPart part;part.name=value.at("name").get<std::string>();part.material=value.at("material").get<std::string>();
        part.firstIndex=static_cast<unsigned>(size(value.at("firstIndex"),indices));part.indexCount=static_cast<unsigned>(size(value.at("indexCount"),indices));
        require(part.firstIndex==next&&part.indexCount>0&&part.indexCount%3==0&&part.indexCount<=indices-next,
          "NG-E-ANIM-SKIN","Mesh parts must cover the triangle list exactly once");
        next+=part.indexCount;out->meshParts.push_back(std::move(part));
      }
      require(next==indices,"NG-E-ANIM-SKIN","Mesh part coverage is incomplete");
    }
    for (std::size_t i = 0; i < vertices; ++i) {
      auto &v = out->vertices[i];
      auto at = vertexOffset + i * 64;
      for (unsigned k = 0; k < 3; ++k) {
        v.position[k] = f32(bytes, at + k * 4);
        v.normal[k] = f32(bytes, at + 12 + k * 4);
      }
      for (unsigned k = 0; k < 2; ++k)
        v.uv[k] = f32(bytes, at + 24 + k * 4);
      if(out->hasTextureUvs)for(unsigned slot=0;slot<4;slot++)for(unsigned k=0;k<2;k++){
        const auto value=f32(bytes,textureUvOffset+i*32+slot*8+k*4);
        require(std::abs(value)<=1000000,"NG-E-ANIM-FORMAT","Texture UV exceeds supported range");v.textureUvs[slot][k]=value;
      }
      if(out->hasVertexColors)for(unsigned k=0;k<4;k++){
        const auto value=f32(bytes,colorOffset+i*16+k*4);require(std::isfinite(value)&&value>=0&&value<=1,"NG-E-ANIM-FORMAT","Invalid vertex color");v.color[k]=value;
      }
      if(out->hasTangents)for(unsigned k=0;k<4;k++)v.tangent[k]=f32(bytes,tangentOffset+i*16+k*4);
      if(out->hasLayerUvs)for(unsigned slot=0;slot<3;slot++)for(unsigned k=0;k<2;k++){const float value=f32(bytes,layerUvOffset+i*24+slot*8+k*4);require(std::abs(value)<=1000000,"NG-E-ANIM-FORMAT","Layer UV exceeds supported range");v.layerUvs[slot][k]=value;}
      for (unsigned k = 0; k < 4; ++k) {
        v.joints[k] = u32(bytes, at + 32 + k * 4);
        v.weights[k] = f32(bytes, at + 48 + k * 4);
      }
    }
    for (std::size_t i = 0; i < indices; ++i)
      out->indices[i] = u32(bytes, indexOffset + i * 4);
    std::size_t cursor = indexOffset + indices * 4;
    if (data.at("format") == "nativegpu.animation/2") {
      auto vec = [](const json &v) {
        require(v.is_array() && v.size() == 3, "NG-E-ANIM-FORMAT",
                "Expected vector");
        return V3{number(v[0]), number(v[1]), number(v[2])};
      };
      const auto &morphs = data.at("morphs");
      require(morphs.is_array() && morphs.size() <= MaxMorphTargets,
              "NG-E-ANIM-SKIN", "Morph budget exceeded");
      for (const auto &m : morphs) {
        MorphTarget target;
        target.name = m.at("name");
        auto count = size(m.at("count"), vertices);
        auto offset = size(m.at("offset"), bytes.size());
        const auto stride=m.contains("stride")?size(m.at("stride"),40):28;
        require((stride==28||(stride==40&&out->hasTangents))&&offset == cursor && offset + count * stride <= bytes.size(),
                "NG-E-ANIM-FORMAT", "Invalid morph section");
        for (unsigned i = 0; i < count; ++i) {
          auto at = offset + i * stride;
          target.deltas.push_back(
              {u32(bytes, at),
               {f32(bytes, at + 4), f32(bytes, at + 8), f32(bytes, at + 12)},
               {f32(bytes, at + 16), f32(bytes, at + 20),
                f32(bytes, at + 24)},stride==40?V3{f32(bytes,at+28),f32(bytes,at+32),f32(bytes,at+36)}:V3{}});
        }
        cursor += count * stride;
        out->morphs.push_back(std::move(target));
      }
      auto boneIndex = [&](const json &value) {
        return unsigned(size(value, out->skeleton.bones.size() - 1));
      };
      const auto &rig = data.at("rig");
      out->rig.pelvis = rig.at("pelvis").get<int>();
      out->rig.lookBone = rig.at("lookBone").get<int>();
      out->rig.spine = -1;
      if (rig.contains("spine")) {
        const auto value = rig.at("spine").get<int>();
        require(value >= -1 && value < int(out->skeleton.bones.size()), "NG-E-ANIM-RIG", "Invalid spine bone");
        out->rig.spine = value;
      }
      out->rig.source = rig.value("source", std::string());
      out->rig.confidence = rig.contains("confidence") ? number(rig.at("confidence")) : 0.f;
      require(rig.at("legs").is_array() && rig.at("legs").size() <= 2,
              "NG-E-ANIM-RIG", "Rig leg budget");
      for (const auto &c : rig.at("legs")) {
        int toe = -1;
        if (c.contains("toe")) {
          toe = c.at("toe").get<int>();
          require(toe >= -1 && toe < int(out->skeleton.bones.size()), "NG-E-ANIM-RIG", "Invalid toe bone");
        }
        RigChain chain{boneIndex(c.at("root")), boneIndex(c.at("joint")),
                       boneIndex(c.at("tip")), toe,
                       vec(c.at("pole")), number(c.at("minBend")),
                       number(c.at("maxBend")), number(c.at("soleHeight")),
                       c.contains("footLength") ? number(c.at("footLength")) : .15f};
        require(chain.toe < 0 || out->skeleton.bones[chain.toe].parent == int(chain.tip),
                "NG-E-ANIM-RIG", "Toe must be parented to the foot");
        out->rig.legs.push_back(chain);
      }
      // Arms are optional. A rig without them keeps its procedural feet and
      // simply offers no hand contacts; rejecting the asset would break every
      // character imported before the field existed.
      if (rig.contains("arms")) {
        require(rig.at("arms").is_array() && rig.at("arms").size() <= 2,
                "NG-E-ANIM-RIG", "Rig arm budget");
        for (const auto &c : rig.at("arms")) {
          RigChain chain{boneIndex(c.at("root")), boneIndex(c.at("joint")),
                         boneIndex(c.at("tip")), -1, vec(c.at("pole")),
                         c.contains("minBend") ? number(c.at("minBend")) : .01f,
                         c.contains("maxBend") ? number(c.at("maxBend")) : 2.9f};
          require(out->skeleton.bones[chain.joint].parent == int(chain.root) &&
                      out->skeleton.bones[chain.tip].parent == int(chain.joint),
                  "NG-E-ANIM-RIG", "Arm chain must be a direct parent chain");
          out->rig.arms.push_back(chain);
        }
      }
      if (rig.contains("settings")) {
        const auto &s = rig.at("settings");
        require(s.is_object(), "NG-E-ANIM-RIG", "Invalid rig settings");
        auto setting = [&](const char *key, float &value, float low, float high) {
          if (!s.contains(key)) return;
          const float v = number(s.at(key));
          require(v >= low && v <= high, "NG-E-ANIM-RIG", std::string("Invalid rig setting ") + key);
          value = v;
        };
        auto &r = out->rig.settings;
        setting("height", r.height, .1f, 100.f);
        setting("turnThreshold", r.turnThreshold, .05f, 3.14f);
        setting("turnStepHeight", r.turnStepHeight, 0.f, 5.f);
        setting("turnStepDuration", r.turnStepDuration, .01f, 10.f);
        setting("turnCooldown", r.turnCooldown, 0.f, 10.f);
        setting("maxTwist", r.maxTwist, 0.f, 6.28f);
        setting("maxSlopeCosine", r.maxSlopeCosine, 0.f, 1.f);
        setting("swingPrediction", r.swingPrediction, 0.f, 10.f);
        setting("maxFootCorrection", r.maxFootCorrection, 0.f, 10.f);
        setting("pelvisLower", r.pelvisLower, -10.f, 0.f);
        setting("pelvisUpper", r.pelvisUpper, 0.f, 10.f);
        setting("maxStepHeight", r.maxStepHeight, 0.f, 10.f);
        setting("maxPelvisShift", r.maxPelvisShift, 0.f, 1.f);
        setting("soleRoll", r.soleRoll, 0.f, 1.5f);
        if (s.contains("footLock")) r.footLock = s.at("footLock").get<bool>();
        if (s.contains("turnInPlace")) r.turnInPlace = s.at("turnInPlace").get<bool>();
      }
      if (rig.contains("diagnostics")) {
        const auto &d = rig.at("diagnostics");
        if (d.contains("warnings") && d.at("warnings").is_array()) {
          require(d.at("warnings").size() <= 16, "NG-E-ANIM-RIG", "Rig warning budget");
          for (const auto &w : d.at("warnings")) {
            require(w.is_string() && w.get<std::string>().size() <= 256, "NG-E-ANIM-RIG", "Invalid rig warning");
            out->rig.warnings.push_back(w.get<std::string>());
          }
        }
      }
      require(data.at("muscles").is_array() &&
                  data.at("muscles").size() <= 64 &&
                  data.at("softRegions").is_array() &&
                  data.at("softRegions").size() <= 16,
              "NG-E-ANIM-SKIN", "Deformation budget exceeded");
      for (const auto &m : data.at("muscles")) {
        require(!out->morphs.empty(), "NG-E-ANIM-SKIN", "Missing muscle morph");
        out->muscles.push_back(
            {boneIndex(m.at("bone")),
             unsigned(size(m.at("target"), out->morphs.size() - 1)),
             number(m.at("angleStart")), number(m.at("angleEnd")),
             number(m.at("maxWeight"))});
      }
      for (const auto &r : data.at("softRegions")) {
        SoftRegion region;
        region.bone = boneIndex(r.at("bone"));
        region.center = vec(r.at("center"));
        region.radius = number(r.at("radius"));
        region.maxOffset = number(r.at("maxOffset"));
        require(r.at("vertices").is_array() &&
                    r.at("vertices").size() <= vertices,
                "NG-E-ANIM-SKIN", "Soft vertex budget");
        for (const auto &v : r.at("vertices"))
          region.vertices.push_back(unsigned(size(v, vertices - 1)));
        out->softRegions.push_back(std::move(region));
      }
    }
    determinism::Hasher geometryHasher;geometryHasher.u32(static_cast<unsigned>(vertices));geometryHasher.u32(static_cast<unsigned>(indices));geometryHasher.u32(static_cast<unsigned>(out->morphs.size()));
    for(std::size_t i=0;i<cursor;++i)geometryHasher.u8(bytes[i]);out->geometryHash=geometryHasher.hex();
    const auto &clips = data.at("clips");
    require(clips.is_array() && !clips.empty() && clips.size() <= 64,
            "NG-E-ANIM-CLIP", "Invalid clips");
    for (const auto &c : clips) {
      Clip clip;
      clip.name = c.at("name").get<std::string>();
      clip.boneCount = static_cast<unsigned>(bones.size());
      clip.frameCount = static_cast<unsigned>(size(c.at("frames"), 7201));
      clip.loop = c.at("loop").get<bool>();
      clip.matchable = c.value("matchable", false);
      clip.turnRate = c.contains("turnRate") ? number(c.at("turnRate")) : 0;
      clip.locomotionSpeed =
          c.contains("locomotionSpeed") ? number(c.at("locomotionSpeed")) : 0;
      clip.tags = c.contains("tags") ? unsigned(size(c.at("tags"), 65535)) : 1;
      if (c.contains("velocity")) {
        const auto &v = c.at("velocity");
        require(v.is_array() && v.size() == 3, "NG-E-ANIM-CLIP",
                "Invalid velocity");
        clip.motionVelocity = {number(v[0]), number(v[1]), number(v[2])};
      }
      if (c.contains("contacts")) {
        const auto &ranges = c.at("contacts");
        require(ranges.is_array() && ranges.size() == 2, "NG-E-ANIM-CLIP",
                "Invalid contacts");
        for (unsigned i = 0; i < 2; ++i) {
          require(ranges[i].is_array() && ranges[i].size() == 2,
                  "NG-E-ANIM-CLIP", "Invalid contact range");
          clip.contacts[i] = {number(ranges[i][0]), number(ranges[i][1])};
        }
        clip.contactSource = Clip::ContactSource::Authored;
        clip.contactConfidence = 1;
      }
      if (c.contains("contactSource")) {
        const auto source = c.at("contactSource").get<std::string>();
        require(source == "authored" || source == "inferred" || source == "unknown",
                "NG-E-ANIM-CLIP", "Invalid contact source");
        if (source == "authored") clip.contactSource = Clip::ContactSource::Authored;
        else if (source == "inferred") clip.contactSource = Clip::ContactSource::Inferred;
        else clip.contactSource = Clip::ContactSource::Unknown;
      }
      if (c.contains("contactConfidence")) {
        clip.contactConfidence = number(c.at("contactConfidence"));
        require(clip.contactConfidence >= 0 && clip.contactConfidence <= 1,
                "NG-E-ANIM-CLIP", "Invalid contact confidence");
      }
      require(clip.contactSource == Clip::ContactSource::Unknown || c.contains("contacts"),
              "NG-E-ANIM-CLIP", "Contact source requires contact ranges");
      const auto &steps = c.at("stepChannels");
      require(steps.is_array() && steps.size() == bones.size(),
              "NG-E-ANIM-CLIP", "Invalid STEP channel mask");
      for (unsigned b = 0; b < steps.size(); ++b)
        clip.stepChannels[b] = static_cast<std::uint8_t>(size(steps[b], 7));
      auto offset = size(c.at("offset"), bytes.size());
      std::size_t count = std::size_t(clip.frameCount) * clip.boneCount;
      require(clip.frameCount >= 2 && offset == cursor &&
                  offset + count * 40 <= bytes.size(),
              "NG-E-ANIM-CLIP", "Clip sections overlap or exceed binary");
      clip.samples.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        json values = json::array();
        for (unsigned k = 0; k < 10; ++k) {
          auto raw = u32(bytes, offset + i * 40 + k * 4);
          std::int32_t signedValue;
          std::memcpy(&signedValue, &raw, 4);
          values.push_back(static_cast<float>(signedValue) / 10000.f);
        }
        clip.samples.push_back(transform(values));
      }
      const auto &notifies = c.at("notifies");
      require(notifies.is_array() && notifies.size() <= 256, "NG-E-ANIM-CLIP",
              "Too many notifies");
      for (auto n : notifies)
        clip.notifies.push_back(
            {static_cast<unsigned>(size(n.at("frame"), clip.frameCount - 1)),
             n.at("name").get<std::string>()});
      cursor = offset + count * 40;
      if(c.contains("morphOffset")) {
        const auto targets=size(c.at("morphTargets"),MaxMorphTargets);
        const auto morphOffset=size(c.at("morphOffset"),bytes.size());
        require(targets>0&&targets==out->morphs.size()&&morphOffset==cursor&&
                    clip.frameCount*targets*4<=bytes.size()-cursor,
                "NG-E-ANIM-CLIP","Invalid morph sample section");
        clip.stepMorphMask=static_cast<std::uint16_t>(size(c.at("stepMorphMask"),(1u<<targets)-1));
        clip.morphSamples.resize(clip.frameCount);
        for(unsigned f=0;f<clip.frameCount;++f)for(unsigned m=0;m<targets;++m) {
          auto raw=u32(bytes,morphOffset+(f*targets+m)*4);std::int32_t value;
          std::memcpy(&value,&raw,4);clip.morphSamples[f][m]=float(value)/10000.f;
        }
        cursor+=clip.frameCount*targets*4;
      }
      out->clips.push_back(std::move(clip));
    }
    require(cursor == bytes.size(), "NG-E-ANIM-FORMAT",
            "Unexpected trailing data");
    out->validate();
    const auto rootMotion=data.value("rootMotion",std::string("extract"));
    require(rootMotion=="extract"||rootMotion=="preserve","NG-E-ANIM-CLIP","Invalid root motion policy");
    out->preserveRootTranslation=rootMotion=="preserve";
    if(data.contains("controller")) {
      const auto& clips=data.at("controller");
      require(clips.is_array()&&clips.size()==5,"NG-E-ANIM-CLIP","Controller requires five clip bindings");
      for(unsigned i=0;i<5;++i){out->controllerClips[i]=clips[i].get<std::string>();out->clipIndex(out->controllerClips[i]);}
    }
    if(data.contains("attachments")) {
      out->chestBone=data.at("attachments").value("chest",std::string());
      out->rightHandBone=data.at("attachments").value("rightHand",std::string());
      for(const auto& name:{out->chestBone,out->rightHandBone})if(!name.empty())
        require(std::any_of(out->skeleton.bones.begin(),out->skeleton.bones.end(),[&](const Bone& bone){return bone.name==name;}),"NG-E-ANIM-SKELETON","Unknown attachment bone");
    }
    return out;
  } catch (const Error &) {
    throw;
  } catch (const std::exception &e) {
    throw Error("NG-E-ANIM-FORMAT", e.what());
  }
}
} // namespace engine::animation
