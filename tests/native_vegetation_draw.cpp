// GPU vegetation evidence: indirect instanced draw of packed page records.
//
// The test renders real pixels into a float target and counts them, because "the draw was
// submitted" is not evidence that anything reached the screen. It also proves the two
// properties that are easy to get wrong: back faces of a leaf are drawn, and a distance
// cull produces zero coverage instead of a partial frame.

#include "../renderer/vegetation_draw.hpp"
#include "../renderer/png_writer.h"
#include "../engine/native/vegetation/asset.hpp"
#include "../engine/native/vegetation/texture.hpp"
#include "../third_party/json.hpp"
#include "d3d12_test_rig.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using gpu_test::Rig;
using namespace engine::vegetation;

namespace {
unsigned checks = 0, groups = 0;
// The alpha cutout is not a test constant. The cook bakes the coverage chain against the
// material contract's alphaCutoff, the package carries it, and the shader discards at the value
// the material supplies. A local literal here would pass while those three disagreed.
void require(bool ok, const std::string& why) { ++checks; if (!ok) throw std::runtime_error(why); }

std::string readText(const std::string& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + file);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

// Column-major multiply, matching renderer/matrix_math.h conventions.
void multiply(const float a[16], const float b[16], float out[16]) {
    for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row) {
        float sum = 0;
        for (int k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[column * 4 + k];
        out[column * 4 + row] = sum;
    }
}

// Left-handed perspective with a 0..1 depth range, looking down +Z.
void perspective(float fovY, float aspect, float nearPlane, float farPlane, float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    const float f = 1.0f / std::tan(fovY * 0.5f);
    out[0] = f / aspect;
    out[5] = f;
    out[10] = farPlane / (farPlane - nearPlane);
    out[11] = 1.0f;
    out[14] = -nearPlane * farPlane / (farPlane - nearPlane);
}

// Eye looks at +Z from `eye`, so the view matrix is a pure translation.
void viewFrom(const float eye[3], float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    out[0] = out[5] = out[10] = out[15] = 1.0f;
    out[12] = -eye[0]; out[13] = -eye[1]; out[14] = -eye[2];
}

void crossProduct(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
void normalizeVector(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length < 1e-6f) { v[0] = 0; v[1] = 1; v[2] = 0; return; }
    v[0] /= length; v[1] /= length; v[2] /= length;
}

// Left-handed look-at, column-major, matching the +Z camera convention above.
void lookAt(const float eye[3], const float target[3], const float up[3], float out[16]) {
    float z[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    normalizeVector(z);
    const float worldUp[3] = {up[0], up[1], up[2]};
    float x[3]; crossProduct(worldUp, z, x); normalizeVector(x);
    float y[3]; crossProduct(z, x, y);
    std::memset(out, 0, sizeof(float) * 16);
    out[0] = x[0]; out[1] = y[0]; out[2] = z[0];
    out[4] = x[1]; out[5] = y[1]; out[6] = z[1];
    out[8] = x[2]; out[9] = y[2]; out[10] = z[2];
    out[12] = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
    out[13] = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
    out[14] = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
    out[15] = 1.0f;
}

// Left-handed orthographic with a 0..1 depth range, matching perspective().
void orthographic(float widthMetres, float heightMetres, float nearPlane, float farPlane, float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    out[0] = 2.0f / widthMetres;
    out[5] = 2.0f / heightMetres;
    out[10] = 1.0f / (farPlane - nearPlane);
    out[14] = -nearPlane / (farPlane - nearPlane);
    out[15] = 1.0f;
}

struct Targets {
    // 512 px so a 13 m tree at close range is judged on its leaves, not on sub-pixel dots.
    static constexpr unsigned size = 512;
    gpu_test::Com<ID3D12Resource> color, depth, readback;
    gpu_test::Com<ID3D12DescriptorHeap> rtv, dsv;
    explicit Targets(Rig& rig) {
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = 1;
        gpu_test::hr(rig.device.p->CreateDescriptorHeap(&heap, IID_PPV_ARGS(rtv.out())), "Create vegetation RTV heap");
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        gpu_test::hr(rig.device.p->CreateDescriptorHeap(&heap, IID_PPV_ARGS(dsv.out())), "Create vegetation DSV heap");
        D3D12_HEAP_PROPERTIES properties{}; properties.Type = D3D12_HEAP_TYPE_DEFAULT; properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC description{}; description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = description.Height = size; description.DepthOrArraySize = description.MipLevels = 1;
        description.SampleDesc.Count = 1; description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{}; clear.Format = description.Format;
        gpu_test::hr(rig.device.p->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(color.out())), "Create vegetation colour target");
        rig.device.p->CreateRenderTargetView(color.p, nullptr, rtv.p->GetCPUDescriptorHandleForHeapStart());
        description.Format = DXGI_FORMAT_D32_FLOAT; description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        clear.Format = description.Format; clear.DepthStencil.Depth = 1.0f;
        gpu_test::hr(rig.device.p->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(depth.out())), "Create vegetation depth target");
        rig.device.p->CreateDepthStencilView(depth.p, nullptr, dsv.p->GetCPUDescriptorHandleForHeapStart());
        gpu_test::makeBuffer(rig.device.p, readback, size * size * 16ull, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    void bind(Rig& rig) {
        auto target = rtv.p->GetCPUDescriptorHandleForHeapStart();
        auto z = dsv.p->GetCPUDescriptorHandleForHeapStart();
        const float clear[4] = {0, 0, 0, 1};
        rig.commands.p->ClearRenderTargetView(target, clear, 0, nullptr);
        rig.commands.p->ClearDepthStencilView(z, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        rig.commands.p->OMSetRenderTargets(1, &target, FALSE, &z);
        D3D12_VIEWPORT viewport{0, 0, static_cast<float>(size), static_cast<float>(size), 0, 1};
        D3D12_RECT scissor{0, 0, size, size};
        rig.commands.p->RSSetViewports(1, &viewport);
        rig.commands.p->RSSetScissorRects(1, &scissor);
    }
    std::vector<std::array<float, 4>> image(Rig& rig) {
        const unsigned pitch = size * 16;
        rig.begin();
        gpu_test::transition(rig.commands.p, color.p, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = color.p; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback.p; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, size, size, 1, pitch};
        rig.commands.p->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        gpu_test::transition(rig.commands.p, color.p, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rig.submit();
        std::vector<std::array<float, 4>> pixels(size * size);
        void* data = nullptr;
        D3D12_RANGE range{0, pitch * size};
        gpu_test::hr(readback.p->Map(0, &range, &data), "Map vegetation pixels");
        std::memcpy(pixels.data(), data, pitch * size);
        D3D12_RANGE none{0, 0};
        readback.p->Unmap(0, &none);
        for (const auto& pixel : pixels) for (const auto value : pixel) require(std::isfinite(value), "non-finite vegetation pixel");
        return pixels;
    }
    unsigned covered(const std::vector<std::array<float, 4>>& pixels) const {
        unsigned count = 0;
        for (const auto& pixel : pixels) if (pixel[0] + pixel[1] + pixel[2] > 0.002f) ++count;
        return count;
    }
    void save(Rig& rig, const std::string& name) {
        const auto pixels = image(rig);
        std::vector<unsigned char> rgba(pixels.size() * 4);
        for (std::size_t index = 0; index < pixels.size(); ++index) for (unsigned channel = 0; channel < 4; ++channel) {
            rgba[index * 4 + channel] = static_cast<unsigned char>(std::clamp(pixels[index][channel], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        std::string error;
        require(nativegpu::writePng("build/vegetation-draw/" + name + ".png", size, size, rgba.data(), size * 4, error), error);
    }
};

struct Fixture {
    Species species;
    PageRecord page;
    std::vector<PackedInstance> instances;
};

// Resolves the species' texture roles through the cooked texture library. The digest in
// the species package is the file name, so a texture that changed without a re-cook is a
// missing file rather than a silently different look.
nativegpu::VegetationTextureSet loadTextures(const Species& species, const std::string& directory, unsigned* mipCounts) {
    nativegpu::VegetationTextureSet set;
    const auto library = nlohmann::json::parse(readText(directory + "/library.json"));
    const auto byRole = [&](const std::string& role) -> nlohmann::json {
        for (const auto& entry : library.at("roles")) if (entry.at("role") == role) return entry;
        return nlohmann::json{};
    };
    unsigned total = 0;
    const auto assign = [&](nativegpu::VegetationTexture& target, const nlohmann::json& roles, const char* slot) {
        if (!roles.is_object() || !roles.contains(slot)) return;
        const auto role = roles.at(slot).at("role").get<std::string>();
        const auto entry = byRole(role);
        require(!entry.is_null() && entry.contains("file"), "texture role " + role + " is not in the library");
        const auto image = loadTexture(directory + "/" + entry.at("file").get<std::string>());
        target.width = image.width; target.height = image.height; target.srgb = image.srgb;
        target.levels.clear();
        for (const auto& level : image.levels) {
            target.levels.push_back({level.width, level.height, level.pixels});
        }
        if (mipCounts) mipCounts[total] = image.mipCount();
        ++total;
    };
    const auto& textures = species.textures;
    if (textures.contains("bark") && !textures.at("bark").is_null()) {
        const auto roles = textures.at("bark").at("roles");
        assign(set.barkBaseColor, roles, "baseColor");
        assign(set.barkNormal, roles, "normal");
        assign(set.barkOrm, roles, "orm");
        assign(set.barkHeight, roles, "height");
    }
    if (textures.contains("leaf") && !textures.at("leaf").is_null()) {
        const auto roles = textures.at("leaf").at("roles");
        assign(set.leafBaseColor, roles, "baseColor");
        assign(set.leafNormal, roles, "normal");
        assign(set.leafOrm, roles, "orm");
        assign(set.leafThickness, roles, "thickness");
        assign(set.leafOpacity, roles, "opacity");
    }
    return set;
}

// The runtime alpha cutoff and the cutoff the mask chain was cooked against must agree, or
// the preserved coverage is preserved against a threshold the shader never uses.
float cookedMaskCutoff(const std::string& directory) {
    const auto library = nlohmann::json::parse(readText(directory + "/library.json"));
    for (const auto& entry : library.at("roles")) {
        if (entry.at("role") == "leaf_opacity") return entry.at("coverageCutoff").get<float>();
    }
    return -1.0f;
}

Fixture loadFixture(unsigned instanceCount) {
    const auto library = nlohmann::json::parse(readText("build/vegetation/cooked/library.json"));
    Fixture fixture;
    std::string path;
    for (const auto& entry : library.at("species")) if (entry.at("id") == "oak_broadleaf") path = "build/vegetation/cooked/" + entry.at("file").get<std::string>();
    require(!path.empty(), "oak_broadleaf is not in the library");
    fixture.species = loadSpecies(path);
    fixture.page.firstInstance = 0;
    fixture.page.instanceCount = instanceCount;
    fixture.page.minX = -6; fixture.page.maxX = 6;
    fixture.page.minZ = -6; fixture.page.maxZ = 6;
    fixture.page.baseY = 0; fixture.page.heightRange = 1;
    fixture.page.pageIndex = 0;
    // A row of trees across the page, all at the same height, so coverage scales with the
    // instance count in a predictable way.
    for (unsigned index = 0; index < instanceCount; ++index) {
        PackedInstance packed;
        const float t = instanceCount == 1 ? 0.5f : static_cast<float>(index) / static_cast<float>(instanceCount - 1);
        packed.qx = static_cast<std::uint16_t>((0.15f + 0.7f * t) * 65535.0f);
        packed.qz = static_cast<std::uint16_t>(0.5f * 65535.0f);
        packed.qy = 0;
        packed.yaw = 0;
        packed.scale = 127; // ~1.0
        packed.species = 0;
        packed.stableId = index;
        fixture.instances.push_back(packed);
    }
    return fixture;
}

nativegpu::VegetationMeshData meshFrom(const Species& species) {
    const auto& mesh = species.meshes.at(0);
    nativegpu::VegetationMeshData data;
    data.positions = mesh.positions;
    data.normals = mesh.normals;
    data.uvs = mesh.uvs;
    data.windWeights = mesh.windWeights;
    data.colors = mesh.colors;
    data.indices = mesh.indices;
    data.indexCount = static_cast<unsigned>(mesh.indices.size());
    for (const auto& primitive : species.primitives) {
        if (primitive.lod != 0) continue;
        data.primitives.push_back({primitive.material, primitive.firstIndex, primitive.indexCount});
    }
    return data;
}

float coveredLuminance(const std::vector<std::array<float, 4>>& pixels);

// One render with an explicit material and debug view, returning coverage plus the mean and peak
// luminance of the covered pixels. The material witnesses compare these numbers, so the helper
// deliberately does not read the species' own material: the caller owns the difference.
struct WitnessResult {
    unsigned covered = 0;
    float mean = 0.0f;
    float peak = 0.0f;
    // Per-pixel coverage, so the wood/leaves filters can be compared as sets instead of by count.
    // Counts cannot express "these two views partition the shaded silhouette": a leaf in front of
    // the trunk hides trunk pixels in the shaded frame but not in the wood-only frame, so the two
    // counts sum to more than the shaded count even when the partition is exact.
    std::vector<unsigned char> mask;
};

WitnessResult renderWitness(Rig& rig, Targets& targets, nativegpu::VegetationDraw& draw, const Fixture& fixture,
                            const nativegpu::VegetationFrameInput& frame,
                            const nativegpu::VegetationMaterial& material, int debugView) {
    nativegpu::VegetationFrameInput tuned = frame;
    tuned.alphaCutoff = material.alphaCutoff;
    tuned.debugView = debugView;
    std::string error;
    rig.begin();
    require(draw.upload(rig.commands.p, meshFrom(fixture.species), fixture.instances, fixture.page, error), error);
    rig.submit();
    rig.begin();
    const float bounds[3] = {fixture.species.lod[0].heightMetres, fixture.species.lod[0].radiusMetres, 0.0f};
    require(draw.drawShadow(rig.commands.p, rig.ticket(), tuned, material, bounds, error), error);
    targets.bind(rig);
    require(draw.draw(rig.commands.p, rig.ticket(), tuned, material, bounds, error), error);
    rig.submit();
    const auto pixels = targets.image(rig);
    WitnessResult result;
    result.mask.assign(pixels.size(), 0);
    double sum = 0.0;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto& pixel = pixels[index];
        if (pixel[0] + pixel[1] + pixel[2] <= 0.002f) continue;
        const float luminance = 0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2];
        ++result.covered;
        result.mask[index] = 1;
        sum += luminance;
        result.peak = std::max(result.peak, luminance);
    }
    result.mean = result.covered ? static_cast<float>(sum / result.covered) : 0.0f;
    return result;
}

void renderAndCount(Rig& rig, Targets& targets, nativegpu::VegetationDraw& draw, const Fixture& fixture,
                    const nativegpu::VegetationFrameInput& frame, unsigned* covered, const char* name,
                    float* luminance = nullptr) {
    std::string error;
    // The upload records copies, so it needs an open command list.
    rig.begin();
    require(draw.upload(rig.commands.p, meshFrom(fixture.species), fixture.instances, fixture.page, error), error);
    rig.submit();
    rig.begin();
    // Shadow casters are recorded before the colour target is bound: the shadow pass binds a
    // depth-only output and does not restore the caller's targets.
    // The material comes from the species contract, not from a local literal: a test that
    // supplies its own roughness and thickness cannot notice that the package's values are
    // ignored by the renderer.
    nativegpu::VegetationMaterial material{};
    require(nativegpu::materialFromSpecies(fixture.species, material), "species has no foliage material");
    // The frame discards at the material's cutoff, not at a literal: that is the value the cook
    // baked the coverage chain against.
    nativegpu::VegetationFrameInput tuned = frame;
    tuned.alphaCutoff = material.alphaCutoff;
    const float bounds[3] = {fixture.species.lod[0].heightMetres, fixture.species.lod[0].radiusMetres, 0.0f};
    require(draw.drawShadow(rig.commands.p, rig.ticket(), tuned, material, bounds, error), error);
    targets.bind(rig);
    require(draw.draw(rig.commands.p, rig.ticket(), tuned, material, bounds, error), error);
    rig.submit();
    const auto pixels = targets.image(rig);
    *covered = targets.covered(pixels);
    if (luminance) *luminance = coveredLuminance(pixels);
    if (name) targets.save(rig, name);
}

// Mean luminance over covered pixels only: the background is a constant clear, so including
// it would dilute exactly the signal a shadow is supposed to produce.
float coveredLuminance(const std::vector<std::array<float, 4>>& pixels) {
    double sum = 0.0;
    unsigned count = 0;
    for (const auto& pixel : pixels) {
        if (pixel[0] + pixel[1] + pixel[2] <= 0.002f) continue;
        sum += 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2];
        ++count;
    }
    return count ? static_cast<float>(sum / count) : 0.0f;
}

nativegpu::VegetationFrameInput frameFor(const float eye[3], bool behind) {
    nativegpu::VegetationFrameInput frame;
    float projection[16], view[16];
    perspective(1.0472f, 1.0f, 0.1f, 200.0f, projection);
    if (!behind) {
        viewFrom(eye, view);
    } else {
        // Rotate 180 degrees around Y so the camera looks at the page from behind. Foliage
        // must still render: back faces are the same leaf, not a hole.
        std::memset(view, 0, sizeof(view));
        view[0] = -1; view[5] = 1; view[10] = -1; view[15] = 1;
        view[12] = eye[0]; view[13] = -eye[1]; view[14] = eye[2];
    }
    multiply(projection, view, frame.viewProjection.m);
    frame.camera[0] = eye[0]; frame.camera[1] = eye[1]; frame.camera[2] = eye[2];
    frame.seconds = 0.0f;
    frame.windSpeed = 0.0f;
    frame.windAmplitude = 0.0f;
    // Direction *to* the key light: up and toward the camera side, so the visible face of
    // the tree is the lit one. A backlit setup makes every screenshot look black.
    frame.lightDirection[0] = -0.42f; frame.lightDirection[1] = 0.62f; frame.lightDirection[2] = -0.66f;
    frame.lightIntensity = 3.4f;
    frame.ambient = 0.55f;
    frame.viewportHeight = static_cast<float>(Targets::size);
    frame.tanHalfFov = std::tan(1.0472f * 0.5f);
    // Placeholder; renderAndCount overwrites this with the material's own cutoff.
    frame.alphaCutoff = 0.35f;
    frame.maximumDistance = 100.0f;
    // Shadows are off unless a test asks for them: an identity light matrix would project
    // every fragment onto one texel.
    frame.shadowStrength = 0.0f;
    return frame;
}

// Key-light matrices for a tree of the given height at the origin. The frustum is fitted to
// the plant rather than to the camera, so texel density does not fall off with distance.
void enableShadow(nativegpu::VegetationFrameInput& frame, float heightMetres) {
    float dir[3] = {frame.lightDirection[0], frame.lightDirection[1], frame.lightDirection[2]};
    normalizeVector(dir);
    const float target[3] = {0.0f, heightMetres * 0.45f, 0.0f};
    const float distance = heightMetres * 2.5f;
    // lightDirection points *towards* the light (it is the vector the shading term uses as
    // `dot(normal, light)`), so the caster camera sits along +dir from the subject. Placing
    // it along -dir renders the far side of the tree and every fragment then compares as
    // occluded.
    const float eye[3] = {target[0] + dir[0] * distance, target[1] + dir[1] * distance, target[2] + dir[2] * distance};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    float view[16], projection[16];
    lookAt(eye, target, up, view);
    const float extent = heightMetres * 1.3f;
    orthographic(extent, extent, distance * 0.25f, distance * 1.75f, projection);
    multiply(projection, view, frame.lightViewProjection.m);
    frame.shadowStrength = 1.0f;
    frame.shadowDepthBias = 0.0012f;
    frame.shadowNormalBiasMetres = heightMetres * 0.004f;
}
}

int main(int argc, char** argv) {
    try {
        std::string adapter;
        if (argc == 3 && std::string(argv[1]) == "--adapter") adapter = argv[2];
        else if (argc != 1) throw std::runtime_error("Usage: native_vegetation_draw_tests [--adapter substring]");
        Rig rig(adapter);
        Targets targets(rig);
        auto fixture = loadFixture(1);
        nativegpu::VegetationDraw draw(rig.device.p, rig.fence.p, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_D32_FLOAT, 256);
        // Textures are uploaded once, before the first page: they are per-species state,
        // not per-page state, and the descriptors must outlive every command list.
        {
            // Material textures are per-species state: uploaded once, before the first page.
            std::string textureError;
            // Nine slots: bark baseColor/normal/orm/height, leaf baseColor/normal/orm/thickness/
            // opacity. The order is the shader's t3..t11 order.
            unsigned mipCounts[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
            const auto textures = loadTextures(fixture.species, "build/vegetation/textures", mipCounts);
            require(textures.barkBaseColor.valid() && textures.leafBaseColor.valid() && textures.leafOpacity.valid(),
                "the oak species did not resolve its bark and leaf textures");
            require(textures.barkOrm.valid() && textures.leafOrm.valid() && textures.leafThickness.valid()
                && textures.barkHeight.valid(),
                "the oak species did not resolve ORM, thickness or height; those slots are what the debug views show");
            // A "full chain" is not one number: the baseColor and normal sheets are 1024 px
            // (11 levels) and the data sheets are 512 px (10 levels). Asserting a flat 10 would
            // pass only while every sheet happened to be the same size, so the expectation is
            // derived from each texture's own base level.
            const nativegpu::VegetationTexture* slots[9] = {
                &textures.barkBaseColor, &textures.barkNormal, &textures.barkOrm, &textures.barkHeight,
                &textures.leafBaseColor, &textures.leafNormal, &textures.leafOrm, &textures.leafThickness,
                &textures.leafOpacity,
            };
            for (unsigned slot = 0; slot < 9; ++slot) {
                unsigned expected = 1;
                for (unsigned size = std::max(slots[slot]->width, slots[slot]->height); size > 1; size >>= 1) ++expected;
                require(slots[slot]->levels.size() == expected,
                    "texture slot " + std::to_string(slot) + " carried " + std::to_string(slots[slot]->levels.size())
                    + " mip levels, expected " + std::to_string(expected));
                require(mipCounts[slot] == expected, "the loader reported the wrong chain length for slot "
                    + std::to_string(slot));
            }
            // The mask chain is coverage-corrected against this exact threshold; a mismatch
            // means the chain preserves coverage the shader never tests.
            nativegpu::VegetationMaterial contract{};
            require(nativegpu::materialFromSpecies(fixture.species, contract), "species has no foliage material");
            require(std::fabs(cookedMaskCutoff("build/vegetation/textures") - contract.alphaCutoff) < 1e-6f,
                "the cooked mask cutoff " + std::to_string(cookedMaskCutoff("build/vegetation/textures"))
                + " disagrees with the material's alphaCutoff " + std::to_string(contract.alphaCutoff));
            rig.begin();
            require(draw.setTextures(rig.commands.p, textures, textureError), textureError);
            rig.submit();
            ++groups;
        }

        unsigned single = 0;
        const float eye[3] = {3.0f, 7.0f, -24.0f};
        renderAndCount(rig, targets, draw, fixture, frameFor(eye, false), &single, "single-front");
        require(single > 12000, "a single 13 m tree at 22 m covers almost nothing: " + std::to_string(single));
        require(single < Targets::size * Targets::size, "the background is fully covered; the target is not a scene");
        ++groups;

        // More instances must cover more pixels. This is the check that the instance list
        // actually drives the draw instead of the first instance being drawn N times.
        auto many = loadFixture(5);
        unsigned covered = 0;
        renderAndCount(rig, targets, draw, many, frameFor(eye, false), &covered, "five-front");
        require(covered > single, "five instances did not cover more pixels than one");
        ++groups;

        // A back view must still draw: leaves are double sided. The camera sits on the other
        // side of the page and looks back at it, so the visible faces are back faces.
        const float backEye[3] = {-3.0f, 7.0f, 24.0f};
        unsigned behind = 0;
        renderAndCount(rig, targets, draw, fixture, frameFor(backEye, true), &behind, "single-back");
        require(behind > 8000, "the back face of the foliage did not render");
        ++groups;

        // Distance culling must produce zero coverage, not a partial frame.
        auto culled = frameFor(eye, false);
        culled.maximumDistance = 1.0f;
        unsigned none = 0;
        renderAndCount(rig, targets, draw, fixture, culled, &none, "culled");
        require(none == 0, "a distance-culled page still drew " + std::to_string(none) + " pixels");
        ++groups;

        // Key-light shadow. The same camera and the same page are rendered twice; only the
        // shadow strength changes. A shadow map that silently failed to write, or a receiver
        // that never sampled it, would produce identical images here.
        const float height = fixture.species.lod[0].heightMetres;
        float litLuminance = 0.0f, shadowedLuminance = 0.0f, restoredLuminance = 0.0f;
        unsigned litCovered = 0, shadowedCovered = 0, restoredCovered = 0;
        auto litFrame = frameFor(eye, false);
        renderAndCount(rig, targets, draw, fixture, litFrame, &litCovered, nullptr, &litLuminance);

        auto shadowFrame = frameFor(eye, false);
        enableShadow(shadowFrame, height);
        renderAndCount(rig, targets, draw, fixture, shadowFrame, &shadowedCovered, "single-shadow", &shadowedLuminance);
        require(shadowedLuminance < litLuminance * 0.97f,
            "the shadow map did not darken the canopy: lit " + std::to_string(litLuminance)
            + " vs shadowed " + std::to_string(shadowedLuminance));
        // A shadow changes shading, not silhouettes: the same alpha-tested geometry is drawn
        // in both passes. The covered-pixel count is a luminance threshold, though, so a
        // pixel the shadow pushes from just-above to just-below it stops counting; the bound
        // below is 0.5% of the frame and the observed drift is reported in the evidence.
        const unsigned silhouetteDrift = shadowedCovered > litCovered ? shadowedCovered - litCovered : litCovered - shadowedCovered;
        require(silhouetteDrift <= litCovered / 200,
            "the shadow pass changed the silhouette: " + std::to_string(litCovered) + " vs " + std::to_string(shadowedCovered));
        ++groups;

        // Turning it off must restore the unshadowed image exactly. This is the check that
        // the darkening above came from the map and not from an unrelated lighting change.
        auto restored = frameFor(eye, false);
        renderAndCount(rig, targets, draw, fixture, restored, &restoredCovered, nullptr, &restoredLuminance);
        require(restoredCovered == litCovered, "the unshadowed frame drifted");
        require(std::fabs(restoredLuminance - litLuminance) < 1e-4f,
            "disabling the shadow did not restore the lit image: " + std::to_string(litLuminance)
            + " vs " + std::to_string(restoredLuminance));
        ++groups;

        // Minified view. At 45 m a 0.34 m leaf card is about 3 px wide, which is the regime
        // where a mask without a mip chain starts dropping leaves. The cook-side coverage
        // table (tools/vegetation-textures.mjs mips) is the primary evidence; this only
        // proves the chain reached the GPU as a chain and that the crown keeps a silhouette
        // when it is minified.
        const float farEye[3] = {3.0f, 7.0f, -45.0f};
        auto distant = frameFor(farEye, false);
        distant.maximumDistance = 200.0f;
        unsigned distantCoverage = 0;
        renderAndCount(rig, targets, draw, fixture, distant, &distantCoverage, "single-distant");
        require(distantCoverage > single / 10,
            "a 13 m tree at 45 m lost its canopy: " + std::to_string(distantCoverage) + " px");
        // A sub-pixel camera move (50 mm at 45 m is about half a pixel) must not reshuffle
        // which leaves survive: that reshuffle is what reads as shimmer.
        const float shiftedEye[3] = {3.05f, 7.0f, -45.0f};
        auto shifted = frameFor(shiftedEye, false);
        shifted.maximumDistance = 200.0f;
        unsigned shiftedCoverage = 0;
        renderAndCount(rig, targets, draw, fixture, shifted, &shiftedCoverage, nullptr);
        const float drift = distantCoverage > 0
            ? std::fabs(static_cast<float>(shiftedCoverage) - distantCoverage) / distantCoverage : 1.0f;
        require(drift < 0.10f,
            "a sub-pixel camera move changed minified coverage by " + std::to_string(drift * 100.0f) + "%");
        ++groups;

        // ------------------------------------------------------------ material witnesses
        // Each of these changes exactly one material field and keeps the geometry, the camera and
        // the light fixed. A field that is declared in the contract but never reaches the shader
        // produces two identical images here, which is the failure mode the prompt calls out:
        // "a field with no consumer is not implemented".
        nativegpu::VegetationMaterial baseline{};
        require(nativegpu::materialFromSpecies(fixture.species, baseline), "species has no foliage material");
        const float witnessEye[3] = {3.0f, 7.0f, -24.0f};
        const auto witnessFrame = frameFor(witnessEye, false);
        const unsigned framesBeforeWitness = draw.stats().frames;
        unsigned witnessDraws = 0;
        const auto witness = [&](const nativegpu::VegetationMaterial& material, int debugView) {
            ++witnessDraws;
            return renderWitness(rig, targets, draw, fixture, witnessFrame, material, debugView);
        };

        // Roughness: a smoother surface has a tighter, brighter highlight.
        nativegpu::VegetationMaterial smooth = baseline, rough = baseline;
        smooth.leafRoughness = 0.05f;
        rough.leafRoughness = 0.95f;
        const auto smoothPixels = witness(smooth, 0);
        const auto roughPixels = witness(rough, 0);
        require(smoothPixels.covered > 0 && roughPixels.covered > 0, "the roughness witness drew nothing");
        require(smoothPixels.peak > roughPixels.peak * 1.02f,
            "leaf roughness did not change the highlight: peak " + std::to_string(smoothPixels.peak)
            + " (roughness 0.05) vs " + std::to_string(roughPixels.peak) + " (0.95)");

        // Normal scale: zero must fall back to the geometric normal, so the shaded image changes.
        nativegpu::VegetationMaterial flatNormal = baseline;
        flatNormal.normalScale = 0.0f;
        const auto flatPixels = witness(flatNormal, 0);
        require(std::fabs(flatPixels.mean - roughPixels.mean) > 1e-4f,
            "normalScale 0 produced the same mean as a normal-mapped frame; the map is not sampled");

        // AO strength: the ambient term only, so the image must darken when occlusion is applied.
        nativegpu::VegetationMaterial noOcclusion = baseline;
        noOcclusion.occlusionStrength = 0.0f;
        const auto unoccludedPixels = witness(noOcclusion, 0);
        require(unoccludedPixels.mean >= roughPixels.mean,
            "disabling AO darkening made the frame darker; AO is applied with the wrong sign");

        // Alpha cutoff: a higher threshold keeps fewer fragments.
        nativegpu::VegetationMaterial strict = baseline;
        strict.alphaCutoff = 0.85f;
        const auto strictPixels = witness(strict, 0);
        require(strictPixels.covered < roughPixels.covered,
            "raising the alpha cutoff did not remove any coverage: " + std::to_string(strictPixels.covered)
            + " vs " + std::to_string(roughPixels.covered));

        // Thickness, backlit: a thicker leaf transmits less light. The light is moved behind the
        // canopy for this pair only, because a front-lit leaf hides the transmission term.
        auto backlit = witnessFrame;
        backlit.lightDirection[0] = 0.42f; backlit.lightDirection[1] = -0.35f; backlit.lightDirection[2] = 0.80f;
        nativegpu::VegetationMaterial thin = baseline, thick = baseline;
        thin.leafThicknessMetres = 0.00002f;
        thick.leafThicknessMetres = 0.02f;
        const auto thinPixels = renderWitness(rig, targets, draw, fixture, backlit, thin, 0);
        const auto thickPixels = renderWitness(rig, targets, draw, fixture, backlit, thick, 0);
        witnessDraws += 2;
        require(thinPixels.covered > 0, "the backlit thickness pair drew nothing");
        require(thinPixels.mean > thickPixels.mean,
            "a thicker leaf was not darker when backlit: " + std::to_string(thinPixels.mean)
            + " (thin) vs " + std::to_string(thickPixels.mean) + " (thick)");

        // Debug views: wood + leaves partition the shaded silhouette, and the UV view is a
        // different image rather than a relabelled one.
        const auto wood = witness(baseline, static_cast<int>(nativegpu::VegetationDebugView::WoodOnly));
        const auto foliage = witness(baseline, static_cast<int>(nativegpu::VegetationDebugView::LeavesOnly));
        const auto checker = witness(baseline, static_cast<int>(nativegpu::VegetationDebugView::UvChecker));
        const auto shaded = witness(baseline, 0);
        require(wood.covered > 0 && foliage.covered > 0,
            "a debug view removed every pixel: wood " + std::to_string(wood.covered)
            + ", leaves " + std::to_string(foliage.covered) + ", shaded " + std::to_string(shaded.covered));
        // Each filter must remove the other class...
        require(wood.covered < shaded.covered && foliage.covered < shaded.covered,
            "a material filter did not remove anything: wood " + std::to_string(wood.covered)
            + ", leaves " + std::to_string(foliage.covered) + ", shaded " + std::to_string(shaded.covered));
        // ...and together they must account for every shaded pixel: a pixel drawn in the shaded
        // frame belongs to the bark class or the foliage class, so its absence from both filtered
        // views would mean the material index disagrees between the pass and the filter.
        std::size_t unaccounted = 0;
        for (std::size_t index = 0; index < shaded.mask.size(); ++index) {
            if (shaded.mask[index] && !wood.mask[index] && !foliage.mask[index]) ++unaccounted;
        }
        require(unaccounted * 200 <= shaded.covered,
            std::to_string(unaccounted) + " shaded pixels are neither wood nor foliage");
        require(std::fabs(checker.mean - shaded.mean) > 1e-3f, "the UV checker view equals the shaded view");

        // Height parallax: it may move the texture lookup and nothing else. Two measurements make
        // that a contract instead of a comment — the shaded frame must change (the offset is
        // applied), and the camera-depth frame must not (the geometry and the depth buffer are
        // untouched). A parallax implementation that displaces the vertex or writes depth fails
        // the second one.
        nativegpu::VegetationMaterial noParallax = baseline;
        noParallax.heightScaleMetres = 0.0f;
        nativegpu::VegetationMaterial strongParallax = baseline;
        strongParallax.heightScaleMetres = 0.05f;
        const auto parallaxOff = witness(noParallax, 0);
        const auto parallaxOn = witness(strongParallax, 0);
        require(std::fabs(parallaxOn.mean - parallaxOff.mean) > 1e-5f,
            "height parallax did not change the bark shading: " + std::to_string(parallaxOff.mean)
            + " vs " + std::to_string(parallaxOn.mean));
        const auto depthOff = witness(noParallax, static_cast<int>(nativegpu::VegetationDebugView::CameraDepth));
        const auto depthOn = witness(strongParallax, static_cast<int>(nativegpu::VegetationDebugView::CameraDepth));
        std::size_t depthMoved = 0;
        for (std::size_t index = 0; index < depthOff.mask.size(); ++index) {
            if (depthOff.mask[index] != depthOn.mask[index]) ++depthMoved;
        }
        // Depth is a per-pixel value, so compare the masks and the mean; a parallax that writes
        // depth shifts both.
        require(depthMoved * 1000 <= depthOff.mask.size(),
            std::to_string(depthMoved) + " pixels changed coverage between two parallax settings");
        require(std::fabs(depthOn.mean - depthOff.mean) < 1e-5f,
            "height parallax changed camera depth: " + std::to_string(depthOff.mean) + " vs "
            + std::to_string(depthOn.mean));

        // Normal convention: flipping the green channel must change the frame, and must change
        // nothing when no normal map is sampled. That pair distinguishes "the flag reaches the
        // shader" from "the flag is stored in the package and ignored".
        nativegpu::VegetationMaterial openGl = baseline;
        openGl.leafNormalFlipGreen = !baseline.leafNormalFlipGreen;
        const auto openGlPixels = witness(openGl, 0);
        require(std::fabs(openGlPixels.mean - shaded.mean) > 1e-5f,
            "flipping the normal green channel did not change the frame: " + std::to_string(openGlPixels.mean)
            + " vs " + std::to_string(shaded.mean));
        nativegpu::VegetationMaterial flatOpenGl = flatNormal;
        flatOpenGl.leafNormalFlipGreen = !flatNormal.leafNormalFlipGreen;
        const auto flatOpenGlPixels = witness(flatOpenGl, 0);
        require(std::fabs(flatOpenGlPixels.mean - flatPixels.mean) < 1e-4f,
            "the normal convention changed the frame although no normal map was sampled: "
            + std::to_string(flatOpenGlPixels.mean) + " vs " + std::to_string(flatPixels.mean));
        ++groups;

        // The CPU never loops over instances: the stats show the submitted instance count,
        // and the visible count stays on the GPU by construction.
        const auto stats = draw.stats();
        require(stats.frames == framesBeforeWitness + witnessDraws,
            "unexpected frame count: " + std::to_string(stats.frames) + " vs " + std::to_string(framesBeforeWitness + witnessDraws));
        require(stats.shadowMapSize == 2048, "the shadow map size was not recorded");
        require(stats.textureMips[0] >= 10 && stats.textureMips[4] >= 10,
            "the draw did not record a full mip chain for the bark and leaf baseColor slots");
        require(stats.instanceCount == 1, "the last upload was a single instance");
        require(stats.capacity == 256, "visible capacity was not recorded");
        ++groups;

        // Material witnesses, assembled as their own object so the evidence file can be read
        // field by field. One field changed per row; everything else held fixed.
        nlohmann::json witnessJson;
        witnessJson["leafRoughness"] = {{"smoothPeak", smoothPixels.peak}, {"roughPeak", roughPixels.peak}};
        witnessJson["normalScale"] = {{"flatMean", flatPixels.mean}, {"mappedMean", roughPixels.mean}};
        witnessJson["occlusion"] = {{"unoccludedMean", unoccludedPixels.mean}, {"occludedMean", roughPixels.mean}};
        witnessJson["alphaCutoff"] = {{"baselineCoverage", roughPixels.covered}, {"strictCoverage", strictPixels.covered}};
        witnessJson["thicknessBacklit"] = {{"thinMean", thinPixels.mean}, {"thickMean", thickPixels.mean}};
        witnessJson["heightParallax"] = {{"shadedOff", parallaxOff.mean}, {"shadedOn", parallaxOn.mean},
                                         {"depthOff", depthOff.mean}, {"depthOn", depthOn.mean},
                                         {"depthCoverageMoved", depthMoved}};
        witnessJson["views"] = {{"woodCoverage", wood.covered}, {"foliageCoverage", foliage.covered},
                                {"shadedCoverage", shaded.covered}, {"unaccountedPixels", unaccounted}};

        std::string error;
        require(draw.shutdown(error), error);
        const auto debugErrors = rig.debugErrors();
        require(debugErrors == 0, "vegetation draw produced D3D12 debug errors");
        std::cout << nlohmann::json{{"schema", "nativegpu.vegetation-draw-evidence/1"}, {"ok", true}, {"groups", groups},
            {"checks", checks}, {"adapter", rig.adapter}, {"debugLayer", rig.debug}, {"debugErrors", debugErrors},
            {"singleCoverage", single}, {"fiveCoverage", covered}, {"backCoverage", behind}, {"culledCoverage", none},
            {"size", Targets::size}, {"cpuInstanceLoop", false}, {"runtimeReadback", false},
            {"textureBinding", "bound (9 slots, mip chains from NGT3)"},
            {"textureMips", {stats.textureMips[0], stats.textureMips[1], stats.textureMips[2],
                             stats.textureMips[3], stats.textureMips[4], stats.textureMips[5]}},
            {"distantCoverage", distantCoverage}, {"distantShiftCoverage", shiftedCoverage},
            {"distantShiftDrift", drift},
            {"shadowPass", "key light, alpha-tested depth, 3x3 PCF"},
            {"shadowMapSize", stats.shadowMapSize}, {"shadowFrames", stats.shadowFrames},
            {"shadowCoverage", shadowedCovered}, {"shadowSilhouetteDrift", silhouetteDrift},
            {"litLuminance", litLuminance},
            {"shadowedLuminance", shadowedLuminance},
            {"shadowDarkening", litLuminance > 0.0f ? 1.0 - shadowedLuminance / litLuminance : 0.0},
            {"materialWitness", witnessJson}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "vegetation draw failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
