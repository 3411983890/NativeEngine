#include "asset.hpp"
#include "../assets/sha256.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace engine::groom {
using engine::assets::Package;
using engine::assets::SectionKind;

namespace {
[[noreturn]] void fail(const std::string& code, const std::string& message) { throw Error(code, message); }

const nlohmann::json& field(const nlohmann::json& value, const char* key, const char* where) {
    if (!value.is_object() || !value.contains(key)) fail("NG-E-GROOM-FORMAT", std::string(where) + " is missing " + key);
    return value.at(key);
}
std::uint32_t unsignedField(const nlohmann::json& value, const char* key, std::uint32_t maximum, const char* where) {
    const auto& entry = field(value, key, where);
    if (!entry.is_number()) fail("NG-E-GROOM-FORMAT", std::string(where) + "." + key + " is not numeric");
    const auto result = entry.get<double>();
    if (!std::isfinite(result) || result < 0 || result > maximum || std::floor(result) != result) {
        fail("NG-E-GROOM-FORMAT", std::string(where) + "." + key + " is out of range");
    }
    return static_cast<std::uint32_t>(result);
}
float floatField(const nlohmann::json& value, const char* key, const char* where) {
    const auto& entry = field(value, key, where);
    if (!entry.is_number()) fail("NG-E-GROOM-FORMAT", std::string(where) + "." + key + " is not numeric");
    const auto result = entry.get<double>();
    if (!std::isfinite(result) || std::fabs(result) > 1e6) fail("NG-E-GROOM-FORMAT", std::string(where) + "." + key + " is not finite");
    return static_cast<float>(result);
}
std::string textField(const nlohmann::json& value, const char* key, const char* where) {
    const auto& entry = field(value, key, where);
    if (!entry.is_string()) fail("NG-E-GROOM-FORMAT", std::string(where) + "." + key + " is not a string");
    return entry.get<std::string>();
}

std::vector<Curve> readCurves(const Package& package, std::uint32_t level, std::uint32_t expectedStrands) {
    const auto& headers = package.require(SectionKind::CurveHeader, level);
    const auto points = package.floats(SectionKind::CurvePointF32x3, level);
    const auto radii = package.floats(SectionKind::CurveRadiusF32, level);
    if (headers.count != expectedStrands) fail("NG-E-GROOM-FORMAT", "curve header count disagrees with the LOD table");
    std::vector<Curve> curves(headers.count);
    std::uint32_t expectedFirst = 0;
    for (std::uint32_t index = 0; index < headers.count; ++index) {
        const auto* entry = headers.data + index * 16;
        Curve& curve = curves[index];
        std::memcpy(&curve.firstPoint, entry, 4);
        std::memcpy(&curve.pointCount, entry + 4, 4);
        std::memcpy(&curve.group, entry + 8, 4);
        std::memcpy(&curve.flags, entry + 12, 4);
        if (curve.firstPoint != expectedFirst) fail("NG-E-GROOM-FORMAT", "curve points are not contiguous");
        if (curve.pointCount < 2 || curve.pointCount > 256) fail("NG-E-GROOM-ATTRIBUTE", "curve point count out of range");
        if (curve.firstPoint + curve.pointCount > points.size() / 3) fail("NG-E-GROOM-FORMAT", "curve extends past its point pool");
        expectedFirst += curve.pointCount;
        curve.points.resize(curve.pointCount);
        curve.radii.resize(curve.pointCount);
        for (std::uint32_t point = 0; point < curve.pointCount; ++point) {
            const auto offset = (curve.firstPoint + point) * 3;
            curve.points[point] = {points[offset], points[offset + 1], points[offset + 2]};
            curve.radii[point] = radii[curve.firstPoint + point];
            if (!std::isfinite(curve.points[point].x) || !std::isfinite(curve.points[point].y) || !std::isfinite(curve.points[point].z)) {
                fail("NG-E-GROOM-ATTRIBUTE", "curve point is not finite");
            }
            if (!(curve.radii[point] > 0) || curve.radii[point] > 0.05f) fail("NG-E-GROOM-ATTRIBUTE", "curve radius out of range");
        }
    }
    if (expectedFirst * 3 != points.size()) fail("NG-E-GROOM-FORMAT", "curve point pool has trailing data");
    return curves;
}
}

Error::Error(std::string codeValue, std::string message) : std::runtime_error(std::move(message)), code(std::move(codeValue)) {}

Groom parseGroom(const std::string& bytes, const std::string& expectedDigest) {
    if (!expectedDigest.empty() && engine::assets::sha256(bytes) != expectedDigest) {
        fail("NG-E-GROOM-FORMAT", "groom bytes do not match the expected digest");
    }
    const auto package = engine::assets::readPackage(bytes);
    if (package.schema != "nativegpu.groom-cook/1") fail("NG-E-GROOM-FORMAT", "unsupported groom schema " + package.schema);
    const auto& meta = package.meta;

    Groom groom;
    groom.schema = package.schema;
    groom.id = textField(meta, "id", "groom meta");
    groom.seed = unsignedField(meta, "seed", 0xFFFFFFFFu, "groom meta");
    const auto style = textField(meta, "style", "groom meta");
    groom.style = style == "brows" ? 1u : style == "lashes" ? 2u : style == "beard" ? 3u : style == "fur" ? 4u : 0u;
    groom.lengthMetres = floatField(meta, "lengthMetres", "groom meta");
    groom.widthMetres = floatField(meta, "widthMetres", "groom meta");

    const auto& representations = field(meta, "representations", "groom meta");
    if (!representations.is_array()) fail("NG-E-GROOM-FORMAT", "groom representations must be an array");
    for (const auto& entry : representations) if (entry.is_string()) groom.representations.push_back(entry.get<std::string>());

    // Scalp geometry: required for binding, even for a style that is drawn from cards.
    const auto scalp = field(meta, "scalp", "groom meta");
    groom.scalpPositions = package.floats(SectionKind::PositionF32x3, 900);
    groom.scalpNormals = package.floats(SectionKind::NormalF32x3, 900);
    groom.scalpUvs = package.floats(SectionKind::UvF32x2, 900);
    const auto indices = package.uints(SectionKind::IndexU32, 900);
    groom.scalpIndices = indices;
    const auto vertices = groom.scalpPositions.size() / 3;
    if (vertices == 0 || groom.scalpIndices.empty()) fail("NG-E-GROOM-FORMAT", "groom has no scalp mesh");
    if (unsignedField(scalp, "vertices", 1000000, "groom scalp") != vertices) fail("NG-E-GROOM-FORMAT", "scalp vertex count disagrees with its buffer");
    if (unsignedField(scalp, "triangles", 2000000, "groom scalp") * 3 != groom.scalpIndices.size()) fail("NG-E-GROOM-FORMAT", "scalp triangle count disagrees with its buffer");
    for (const auto index : groom.scalpIndices) if (index >= vertices) fail("NG-E-GROOM-FORMAT", "scalp index outside its vertex buffer");

    const auto& groupList = field(meta, "groups", "groom meta");
    if (!groupList.is_array() || groupList.empty() || groupList.size() > 8) fail("NG-E-GROOM-FORMAT", "groom groups must have 1–8 entries");
    for (const auto& entry : groupList) {
        Group group;
        group.id = textField(entry, "id", "groom group");
        const auto simulation = textField(entry, "simulation", "groom group");
        group.simulation = simulation == "rigid" ? Simulation::Rigid : Simulation::Guide;
        const auto shadow = textField(entry, "shadow", "groom group");
        group.shadow = shadow == "none" ? ShadowMode::None : ShadowMode::DeepOpacity;
        const auto& material = field(entry, "material", "groom group");
        const auto& albedo = field(material, "albedo", "groom material");
        const auto& absorption = field(material, "absorption", "groom material");
        for (int channel = 0; channel < 3; ++channel) {
            group.material.albedo[channel] = albedo.at(channel).get<float>();
            group.material.absorption[channel] = absorption.at(channel).get<float>();
            if (!(group.material.albedo[channel] >= 0 && group.material.albedo[channel] <= 1)) fail("NG-E-GROOM-FORMAT", "material albedo out of range");
            if (!(group.material.absorption[channel] >= 0 && group.material.absorption[channel] <= 1)) fail("NG-E-GROOM-FORMAT", "material absorption out of range");
        }
        group.material.roughness = floatField(material, "roughness", "groom material");
        group.material.radialRoughness = floatField(material, "radialRoughness", "groom material");
        group.material.longitudinalRoughness = floatField(material, "longitudinalRoughness", "groom material");
        group.material.melanin = floatField(material, "melanin", "groom material");
        group.material.redness = floatField(material, "redness", "groom material");
        groom.groups.push_back(std::move(group));
    }

    groom.guides = readCurves(package, 1000, unsignedField(field(meta, "counts", "groom meta"), "guides", 8192, "groom counts"));
    const auto& bindings = package.require(SectionKind::Binding, 1000);
    if (bindings.count != groom.guides.size()) fail("NG-E-GROOM-BINDING", "binding count disagrees with the guide count");
    groom.bindings.resize(bindings.count);
    std::memcpy(groom.bindings.data(), bindings.data, bindings.bytes);
    for (const auto& binding : groom.bindings) {
        if (binding.triangle * 3 + 2 >= groom.scalpIndices.size()) fail("NG-E-GROOM-BINDING", "binding references a triangle outside the scalp");
        const auto sum = binding.bary[0] + binding.bary[1] + binding.bary[2];
        if (!(std::fabs(sum - 1.0f) < 1e-3f)) fail("NG-E-GROOM-BINDING", "binding barycentric weights do not sum to 1");
        for (const auto weight : binding.bary) if (weight < -1e-3f || weight > 1.0f + 1e-3f) fail("NG-E-GROOM-BINDING", "binding weight outside [0,1]");
    }

    const auto& lodList = field(meta, "lod", "groom meta");
    if (!lodList.is_array() || lodList.empty() || lodList.size() > 4) fail("NG-E-GROOM-FORMAT", "groom LOD table must have 1–4 entries");
    for (std::size_t index = 0; index < lodList.size(); ++index) {
        const auto& entry = lodList[index];
        Level level;
        level.level = unsignedField(entry, "level", 3, "groom lod");
        level.strands = unsignedField(entry, "strands", 400000, "groom lod");
        level.segments = unsignedField(entry, "segments", 64, "groom lod");
        level.cardCount = unsignedField(entry, "cards", 400000, "groom lod");
        level.maxDistanceMetres = floatField(entry, "maxDistanceMetres", "groom lod");
        if (level.level != index) fail("NG-E-GROOM-FORMAT", "groom LOD levels must be ordered from 0");
        if (index > 0 && level.strands > groom.levels.back().strands) fail("NG-E-GROOM-FORMAT", "a coarser LOD adds strands");
        level.curves = readCurves(package, level.level, level.strands);
        for (const auto& curve : level.curves) {
            if (curve.group >= groom.groups.size()) fail("NG-E-GROOM-FORMAT", "curve references an unknown group");
            if (curve.pointCount != level.segments + 1) fail("NG-E-GROOM-FORMAT", "curve point count disagrees with the LOD segment count");
        }
        const auto& weightSection = package.require(SectionKind::GuideWeight, level.level);
        if (weightSection.count != level.strands) fail("NG-E-GROOM-FORMAT", "guide weight count disagrees with the strand count");
        level.weights.resize(weightSection.count);
        std::memcpy(level.weights.data(), weightSection.data, weightSection.bytes);
        for (const auto& weight : level.weights) {
            for (int slot = 0; slot < 3; ++slot) {
                if (weight.guide[slot] >= groom.guides.size()) fail("NG-E-GROOM-FORMAT", "guide weight references an unknown guide");
                if (!(weight.weight[slot] >= -1e-5f && weight.weight[slot] <= 1.0f + 1e-5f)) fail("NG-E-GROOM-FORMAT", "guide weight outside [0,1]");
            }
            const auto sum = weight.weight[0] + weight.weight[1] + weight.weight[2];
            if (!(std::fabs(sum - 1.0f) < 1e-3f)) fail("NG-E-GROOM-FORMAT", "guide weights do not sum to 1");
        }
        const auto& cardSection = package.require(SectionKind::Card, level.level);
        if (cardSection.count != level.cardCount) fail("NG-E-GROOM-FORMAT", "card count disagrees with the LOD table");
        level.cards.resize(cardSection.count);
        std::memcpy(level.cards.data(), cardSection.data, cardSection.bytes);
        std::uint32_t covered = 0;
        for (const auto& card : level.cards) {
            if (card.firstCurve != covered) fail("NG-E-GROOM-FORMAT", "cards do not tile the strand range");
            if (card.curveCount == 0 || card.curveCount > 16) fail("NG-E-GROOM-FORMAT", "card curve count out of range");
            if (!(card.widthMetres > 0) || card.widthMetres > 0.05f) fail("NG-E-GROOM-FORMAT", "card width out of range");
            covered += card.curveCount;
        }
        if (covered != level.strands) fail("NG-E-GROOM-FORMAT", "cards do not cover every strand");
        groom.levels.push_back(std::move(level));
    }

    const auto& bounds = field(meta, "bounds", "groom meta");
    const auto& minimum = field(bounds, "min", "groom bounds");
    const auto& maximum = field(bounds, "max", "groom bounds");
    for (int axis = 0; axis < 3; ++axis) {
        groom.boundsMin[axis] = minimum.at(axis).get<float>();
        groom.boundsMax[axis] = maximum.at(axis).get<float>();
        if (!(groom.boundsMax[axis] > groom.boundsMin[axis])) fail("NG-E-GROOM-FORMAT", "groom bounds are empty");
    }
    return groom;
}

Groom loadGroom(const std::string& file, const std::string& expectedDigest) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) fail("NG-E-GROOM-FORMAT", "groom cannot be opened: " + file);
    std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return parseGroom(bytes, expectedDigest);
}

const Level& levelFor(const Groom& groom, std::uint32_t level) {
    if (level >= groom.levels.size()) fail("NG-E-GROOM-FORMAT", "groom LOD index outside the level table");
    return groom.levels[level];
}

} // namespace engine::groom
