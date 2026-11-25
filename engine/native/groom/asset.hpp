#pragma once

#include "../assets/package.hpp"
#include "../vec3.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::groom {

struct Error : std::runtime_error {
    std::string code;
    Error(std::string codeValue, std::string message);
};

struct Material {
    float albedo[3] = {0.1f, 0.07f, 0.05f};
    float roughness = 0.3f, radialRoughness = 0.25f, longitudinalRoughness = 0.3f;
    float absorption[3] = {0.6f, 0.42f, 0.3f};
    float melanin = 0.8f, redness = 0.3f;
};

enum class Simulation : std::uint8_t { Guide = 0, Rigid = 1 };
enum class ShadowMode : std::uint8_t { DeepOpacity = 0, None = 1 };

struct Group {
    std::string id;
    Material material;
    Simulation simulation = Simulation::Guide;
    ShadowMode shadow = ShadowMode::DeepOpacity;
};

struct Curve {
    std::uint32_t firstPoint = 0, pointCount = 0, group = 0, flags = 0;
    std::vector<Vec3> points;
    std::vector<float> radii;
};

struct Binding {
    std::uint32_t triangle = 0;
    float bary[3] = {1, 0, 0};
    float rootU = 0, rootV = 0, offsetMetres = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(Binding) == 32, "Binding must match the 32-byte package record");

struct GuideWeight {
    std::uint32_t guide[3] = {0, 0, 0}, strand = 0;
    float weight[3] = {1, 0, 0};
    std::uint32_t reserved = 0;
};
static_assert(sizeof(GuideWeight) == 32, "GuideWeight must match the 32-byte package record");

// Field order is the package ABI, not a preferred reading order. A struct that is
// smaller than its record makes every memcpy overflow the vector, so the size is pinned.
struct Card {
    std::uint32_t firstCurve = 0, curveCount = 0;
    float widthMetres = 0, coverage = 0;
    std::uint32_t layer = 0, material = 0, flags = 0, reserved = 0;
};
static_assert(sizeof(Card) == 32, "Card must match the 32-byte package record");

struct Level {
    std::uint32_t level = 0, segments = 0, strands = 0, cardCount = 0;
    float maxDistanceMetres = 0;
    std::vector<Curve> curves;
    std::vector<GuideWeight> weights;
    std::vector<Card> cards;
};

struct Groom {
    std::string id, schema;
    std::uint32_t style = 0, seed = 0;
    float lengthMetres = 0, widthMetres = 0;
    std::vector<float> scalpPositions, scalpNormals, scalpUvs;
    std::vector<std::uint32_t> scalpIndices;
    std::vector<Curve> guides;
    std::vector<Binding> bindings;
    std::vector<Group> groups;
    std::vector<Level> levels;
    std::vector<std::string> representations;
    float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};
};

Groom parseGroom(const std::string& bytes, const std::string& expectedDigest = {});
Groom loadGroom(const std::string& file, const std::string& expectedDigest = {});

// Segment index a level's curve should use for rendering. Cards are a separate
// representation; a level that declares cards but no curves is a cook bug.
const Level& levelFor(const Groom& groom, std::uint32_t level);

} // namespace engine::groom
