#pragma once

#include "../vec3.hpp"
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace engine::cloth {

struct Error : std::runtime_error {
    std::string code;
    Error(std::string code, std::string message);
};

struct DistanceConstraint { std::uint32_t a = 0, b = 0; float rest = 0; };
struct Membrane {std::array<float,4> inverse{};float area=0;};
struct Hinge {std::array<std::uint32_t,4> vertices{};float angle=0;};
struct Material {
    float arealDensityKgM2 = 0.2f, thicknessMetres = 0.003f;
    float stretchCompliance = 1e-8f, shearCompliance = 1e-7f, bendCompliance = 1e-5f;
    float damping = 0.4f, dragCoefficient = 1.0f;
    float bendAngleCompliance=1000; // rad/(N*m), distinct from legacy m/N.
};
struct Plane { bool present = false; Vec3 normal{0,1,0}; float offsetMetres = 0; };
struct Sphere { Vec3 centre; float radius = 0; };
struct Capsule { Vec3 a, b; float radius = 0; };
struct Ellipsoid { Vec3 centre{}; Vec3 radii{1, 1, 1}; };
struct Collision {
    float thicknessMetres = 0;
    Plane plane;
    std::vector<Sphere> spheres;
    std::vector<Capsule> capsules;
};
struct Pin { std::uint32_t vertex = 0; Vec3 anchor; };
struct Lod { float maxDistanceMetres = 0; std::uint32_t columns = 0, rows = 0; };

// ---- Garment skinning contract -------------------------------------------------
// A garment is an arbitrary triangle mesh that is driven by a character skeleton. The
// document carries the skeleton's inverse bind matrices, per-vertex joint/weight pairs,
// the four per-vertex physics channels, bone-local collision proxies and the bone world
// matrices of every pose in the motion set. Nothing here is measured at load time.
struct Bone { std::string name; std::array<float, 16> inverseBind{}; };
struct BoneSphere { std::uint32_t bone = 0; Vec3 centre{}; float radius = 0; };
struct BoneCapsule { std::uint32_t bone = 0; Vec3 a{}, b{}; float radius = 0; };
// Bone-local ellipsoid. A round capsule cannot represent a body whose torso is twice as deep
// as it is wide; the ellipsoid is fitted per bone from the character's own rest mesh.
struct BoneEllipsoid { std::uint32_t bone = 0; Vec3 centre{}; Vec3 radii{1, 1, 1}; };
struct PoseKey { std::string name; std::vector<std::array<float, 16>> bones; };

// Cooked cloth contract. Rest geometry, triangles, per-vertex mass and the three
// constraint families are produced offline; the runtime only validates and solves.
struct Asset {
    std::string id, hash;
    std::uint32_t revision = 0, seed = 0;
    bool skinned = false;
    std::uint32_t columns = 0, rows = 0;
    std::vector<Vec3> rest;
    std::vector<float> masses, areas;
    std::vector<std::array<std::uint32_t, 3>> triangles;
    std::vector<DistanceConstraint> stretch, shear, bend;
    std::vector<Pin> pins;
    Material material;
    Collision collision;
    std::vector<Lod> lod;
    std::uint32_t maxSubsteps = 1, iterations = 1;
    float totalAreaMetres2 = 0;
    bool triangleModel=false;
    std::vector<Membrane> membranes;
    std::vector<Hinge> hinges;
    std::vector<std::vector<std::uint32_t>> membraneColors,hingeColors,adjacency;
    // Garment skinning (empty for environment cloth). `garment` is the flag the solver
    // and every report branch on; it replaces the old grid-only assumption.
    bool garment = false;
    std::uint32_t influences = 0;
    std::vector<Bone> bones;
    std::vector<std::array<std::uint32_t, 4>> skinJoints;
    std::vector<std::array<float, 4>> skinWeights;
    std::vector<float> blend, maxDisplacement, thickness, anchor;
    std::vector<BoneSphere> boneSpheres;
    std::vector<BoneCapsule> boneCapsules;
    std::vector<BoneEllipsoid> boneEllipsoids;
    std::vector<PoseKey> poses;
    // Unique triangle edges with rest lengths. The membrane solve works in the material
    // frame; the strain limiter needs a per-edge rest length or it has nothing to clamp.
    std::vector<DistanceConstraint> edges;
    float strainLimit = 1.15f, maxSpeed = 12;
};

// Validates the authored parallel schedule, complete coverage and all vertices
// read/written per color. GPU code never infers a missing runtime coloring.
void validateTriangleTopology(const Asset& asset);

std::shared_ptr<const Asset> parseCookedCloth(const std::string& bytes);
std::shared_ptr<const Asset> loadCloth(const std::string& file);
struct Library {
    std::map<std::string, std::shared_ptr<const Asset>> cloths;
    std::string hash;
};
Library loadClothLibrary(const std::string& manifestFile, const std::string& assetRoot);

} // namespace engine::cloth
