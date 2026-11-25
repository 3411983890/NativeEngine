// GPU strand-draw evidence for a cooked hairstyle.
//
// The claims this harness checks are the ones `renderer/hair_draw.hpp` makes: a real
// D3D12 frame draws six vertices per segment through the vertex and pixel shaders, the
// coverage-corrected ribbon covers a plausible silhouette from three views, the debug
// channels expose different quantities, deep-opacity self-shadowing darkens the volume,
// backlit hair transmits by albedo, and an edited or truncated pack is refused before it
// is uploaded.
//
// Not checked here: order-independent transparency (the pass blends in upload order),
// GPU guide simulation, hardware flicker and timing. `HairDrawStats::transparency` names
// the first; the evidence JSON reports the other three as unsupported.

#include "../renderer/hair_draw.hpp"
#include "../renderer/png_writer.h"
#include "../third_party/json.hpp"
#include "d3d12_test_rig.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using gpu_test::Rig;
using nativegpu::HairDraw;
using nativegpu::HairSimulation;
using nativegpu::identityMatrix;
using nativegpu::Mat4;
using nativegpu::HairFrameInput;
using nativegpu::HairLighting;
using nativegpu::HairMaterial;
using engine::hair::GpuPack;

namespace {
unsigned checks = 0, groups = 0;
std::uint64_t drawnVertices = 0, shadedPixels = 0, simulationTicks = 0;
float simulationMaxStretch = 0, simulationMaxPenetration = 0, simulationMaxRootDrift = 0;
float simulationCalmSpeed = 0, simulationWindSpeed = 0;
std::size_t simulationWindChanged = 0;
std::size_t sortingChangedPixels = 0, headTurnChangedPixels = 0;
nlohmann::json gReport = nlohmann::json::object();
constexpr unsigned kHeaderBytes = 320;

void require(bool ok, const std::string& why) { ++checks; if (!ok) throw std::runtime_error(why); }

std::string readText(const std::string& file) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + file);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}
std::vector<std::uint8_t> readBytes(const std::string& file) {
    const auto text = readText(file);
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

Mat4 matrixFrom(const nlohmann::json& values) {
    require(values.is_array() && values.size() == 16, "a fixture matrix must have sixteen floats");
    Mat4 matrix{};
    for (int index = 0; index < 16; ++index) matrix.m[index] = values.at(index).get<float>();
    return matrix;
}
std::array<float, 3> vectorFrom(const nlohmann::json& values) {
    require(values.is_array() && values.size() == 3, "a fixture vector must have three floats");
    return {values.at(0).get<float>(), values.at(1).get<float>(), values.at(2).get<float>()};
}

struct View {
    std::string name;
    Mat4 viewProjection;
    std::array<float, 3> position;
};
struct Fixture {
    unsigned width = 256, height = 256;
    float tanHalfFovY = 0.29f, nearMetres = 0.01f;
    std::vector<View> views;
    HairMaterial material;
    HairLighting keyLight, rimLight;
    nlohmann::json pack;
    const View& view(const std::string& name) const {
        for (const auto& entry : views) if (entry.name == name) return entry;
        throw std::runtime_error("fixture has no view " + name);
    }
};

Fixture loadFixture(const std::string& path) {
    const auto document = nlohmann::json::parse(readText(path));
    require(document.value("schema", std::string()) == "nativegpu.hair-draw-fixture/1", "fixture schema");
    Fixture fixture;
    fixture.width = document.at("width").get<unsigned>();
    fixture.height = document.at("height").get<unsigned>();
    fixture.tanHalfFovY = document.at("tanHalfFovY").get<float>();
    fixture.nearMetres = document.at("nearMetres").get<float>();
    for (const auto& entry : document.at("views")) {
        View view;
        view.name = entry.at("name").get<std::string>();
        view.viewProjection = matrixFrom(entry.at("viewProjection"));
        view.position = vectorFrom(entry.at("position"));
        fixture.views.push_back(view);
    }
    require(!fixture.views.empty(), "fixture has no views");
    const auto& material = document.at("material");
    const auto albedo = vectorFrom(material.at("albedo"));
    std::copy(albedo.begin(), albedo.end(), fixture.material.albedo);
    fixture.material.roughness = material.at("roughness").get<float>();
    fixture.material.radialRoughness = material.value("radialRoughness", 0.25f);
    fixture.material.shiftRadians = material.value("shiftRadians", 0.12f);
    const auto tint = material.contains("rootToTipTint")
        ? vectorFrom(material.at("rootToTipTint")) : std::array<float, 3>{1, 1, 1};
    std::copy(tint.begin(), tint.end(), fixture.material.rootToTipTint);

    const auto lightOf = [](const nlohmann::json& entry) {
        HairLighting light;
        const auto direction = vectorFrom(entry.at("direction"));
        const auto colour = vectorFrom(entry.at("colour"));
        std::copy(direction.begin(), direction.end(), light.direction);
        std::copy(colour.begin(), colour.end(), light.colour);
        light.intensity = entry.at("intensity").get<float>();
        return light;
    };
    fixture.keyLight = lightOf(document.at("keyLight"));
    fixture.rimLight = lightOf(document.at("rimLight"));
    const auto ambient = vectorFrom(document.at("ambient"));
    for (auto* light : {&fixture.keyLight, &fixture.rimLight})
        std::copy(ambient.begin(), ambient.end(), light->ambient);
    fixture.keyLight.shadowStrength = document.value("shadowStrength", 0.9f);
    fixture.rimLight.shadowStrength = 0.9f;
    fixture.keyLight.shadowSigmaPerMetre = document.value("shadowSigmaPerMetre", 2400.0f);
    fixture.rimLight.shadowSigmaPerMetre = fixture.keyLight.shadowSigmaPerMetre;
    fixture.keyLight.shadowDensityScale = document.value("shadowDensityScale", 1.0f);
    fixture.rimLight.shadowDensityScale = fixture.keyLight.shadowDensityScale;
    fixture.pack = document.at("pack");
    return fixture;
}

Mat4 rotationAboutY(float radians, const float pivot[3]) {
    const float cosine = std::cos(radians), sine = std::sin(radians);
    Mat4 result{};
    result.m[0] = cosine; result.m[2] = -sine;
    result.m[5] = 1.0f;
    result.m[8] = sine; result.m[10] = cosine;
    result.m[15] = 1.0f;
    for (int row = 0; row < 3; ++row)
        result.m[12 + row] = pivot[row] - (result.m[row] * pivot[0]
            + result.m[4 + row] * pivot[1] + result.m[8 + row] * pivot[2]);
    return result;
}

HairFrameInput frameOf(const Fixture& fixture, const View& view, int debugView = 0) {
    HairFrameInput frame;
    frame.viewProjection = view.viewProjection;
    std::copy(view.position.begin(), view.position.end(), frame.camera);
    frame.viewportWidth = static_cast<float>(fixture.width);
    frame.viewportHeight = static_cast<float>(fixture.height);
    frame.tanHalfFovY = fixture.tanHalfFovY;
    frame.nearMetres = fixture.nearMetres;
    frame.debugView = debugView;
    return frame;
}

using Pixel = std::array<float, 4>;

struct Targets {
    static constexpr unsigned size = 256;
    gpu_test::Com<ID3D12Resource> color, depth, readback, queryReadback;
    gpu_test::Com<ID3D12DescriptorHeap> rtv, dsv;
    gpu_test::Com<ID3D12QueryHeap> queries;
    struct Counters { std::uint64_t ia = 0, vs = 0, ps = 0; } counters;

    explicit Targets(Rig& rig) {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        gpu_test::hr(rig.device.p->CreateDescriptorHeap(&heap, IID_PPV_ARGS(rtv.out())), "Create hair RTV heap");
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        gpu_test::hr(rig.device.p->CreateDescriptorHeap(&heap, IID_PPV_ARGS(dsv.out())), "Create hair DSV heap");
        D3D12_HEAP_PROPERTIES properties{};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = description.Height = size;
        description.DepthOrArraySize = description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = description.Format;
        gpu_test::hr(rig.device.p->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(color.out())), "Create hair colour target");
        rig.device.p->CreateRenderTargetView(color.p, nullptr, rtv.p->GetCPUDescriptorHandleForHeapStart());
        description.Format = DXGI_FORMAT_D32_FLOAT;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        clear.Format = description.Format;
        clear.DepthStencil.Depth = 1;
        gpu_test::hr(rig.device.p->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(depth.out())), "Create hair depth target");
        rig.device.p->CreateDepthStencilView(depth.p, nullptr, dsv.p->GetCPUDescriptorHandleForHeapStart());
        gpu_test::makeBuffer(rig.device.p, readback, size * 16ull * size, D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_QUERY_HEAP_DESC query{};
        query.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
        query.Count = 1;
        gpu_test::hr(rig.device.p->CreateQueryHeap(&query, IID_PPV_ARGS(queries.out())), "Create hair query heap");
        gpu_test::makeBuffer(rig.device.p, queryReadback, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
            D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    }

    void bind(Rig& rig) {
        auto target = rtv.p->GetCPUDescriptorHandleForHeapStart();
        auto depthHandle = dsv.p->GetCPUDescriptorHandleForHeapStart();
        float clear[4] = {0, 0, 0, 0};
        rig.commands.p->ClearRenderTargetView(target, clear, 0, nullptr);
        rig.commands.p->ClearDepthStencilView(depthHandle, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        rig.commands.p->OMSetRenderTargets(1, &target, FALSE, &depthHandle);
        D3D12_VIEWPORT viewport{0, 0, static_cast<float>(size), static_cast<float>(size), 0, 1};
        D3D12_RECT scissor{0, 0, size, size};
        rig.commands.p->RSSetViewports(1, &viewport);
        rig.commands.p->RSSetScissorRects(1, &scissor);
    }

    void resolveCounters(Rig& rig) {
        rig.commands.p->ResolveQueryData(queries.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, queryReadback.p, 0);
        rig.submit();
        void* data = nullptr;
        D3D12_RANGE range{0, sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
        gpu_test::hr(queryReadback.p->Map(0, &range, &data), "Map hair pipeline counters");
        D3D12_QUERY_DATA_PIPELINE_STATISTICS values{};
        std::memcpy(&values, data, sizeof(values));
        D3D12_RANGE none{0, 0};
        queryReadback.p->Unmap(0, &none);
        counters = {values.IAVertices, values.VSInvocations, values.PSInvocations};
    }

    std::vector<Pixel> image(Rig& rig) {
        constexpr unsigned pitch = size * 16;
        rig.begin();
        gpu_test::transition(rig.commands.p, color.p, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = color.p;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback.p;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, size, size, 1, pitch};
        rig.commands.p->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        gpu_test::transition(rig.commands.p, color.p, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        rig.submit();
        std::vector<Pixel> pixels(size * size);
        void* data = nullptr;
        D3D12_RANGE range{0, pitch * size};
        gpu_test::hr(readback.p->Map(0, &range, &data), "Map hair pixels");
        std::memcpy(pixels.data(), data, pitch * size);
        D3D12_RANGE none{0, 0};
        readback.p->Unmap(0, &none);
        bool allFinite = true;
        for (const auto& pixel : pixels)
            for (float value : pixel) allFinite = allFinite && std::isfinite(value);
        require(allFinite, "a hair pixel is not finite");
        return pixels;
    }

    void save(Rig& rig, const std::filesystem::path& directory, const std::string& name,
              const std::vector<Pixel>& pixels, bool coverageOnly = false) {
        (void)rig;
        std::vector<unsigned char> rgba(pixels.size() * 4);
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            for (unsigned channel = 0; channel < 3; ++channel) {
                const float linear = coverageOnly ? pixels[index][3] : pixels[index][channel];
                const float exposed = 1 - std::exp(-std::max(0.0f, linear) * 1.7f);
                const float encoded = exposed <= 0.0031308f ? 12.92f * exposed
                    : 1.055f * std::pow(exposed, 1 / 2.4f) - 0.055f;
                rgba[index * 4 + channel] = static_cast<unsigned char>(std::clamp(encoded, 0.0f, 1.0f) * 255 + 0.5f);
            }
            rgba[index * 4 + 3] = 255;
        }
        std::string error;
        require(nativegpu::writePng((directory / (name + ".png")).u8string(), size, size, rgba.data(), size * 4, error),
            error.empty() ? "PNG write failed" : error);
    }
};

struct Metrics {
    std::uint64_t covered = 0;
    double coverageFraction = 0, meanLuminance = 0, meanAlpha = 0;
};
Metrics measure(const std::vector<Pixel>& pixels) {
    Metrics metrics;
    double luminance = 0, alpha = 0;
    for (const auto& pixel : pixels) {
        if (pixel[3] <= 0.002f) continue;
        ++metrics.covered;
        luminance += 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2];
        alpha += pixel[3];
    }
    metrics.coverageFraction = static_cast<double>(metrics.covered) / pixels.size();
    if (metrics.covered) {
        metrics.meanLuminance = luminance / metrics.covered;
        metrics.meanAlpha = alpha / metrics.covered;
    }
    return metrics;
}

std::size_t differingPixels(const std::vector<Pixel>& a, const std::vector<Pixel>& b, float epsilon) {
    std::size_t different = 0;
    for (std::size_t index = 0; index < a.size(); ++index)
        for (unsigned channel = 0; channel < 4; ++channel)
            if (std::fabs(a[index][channel] - b[index][channel]) > epsilon) { ++different; break; }
    return different;
}

void upload(Rig& rig, HairDraw& draw, const GpuPack& pack) {
    rig.begin();
    std::string error;
    require(draw.upload(rig.commands.p, rig.ticket(), pack, error), "hair upload failed: " + error);
    rig.submit();
}

struct Render {
    Metrics metrics;
    std::vector<Pixel> pixels;
    nativegpu::HairDrawStats stats;
    Targets::Counters counters;
};

Render renderFrame(Rig& rig, Targets& targets, HairDraw& draw, const Fixture& fixture, const View& view,
                   const HairMaterial& material, const HairLighting& lighting, int debugView = 0,
                   bool sortStrands = true, const Mat4& headTransform = identityMatrix()) {
    rig.begin();
    targets.bind(rig);
    rig.commands.p->BeginQuery(targets.queries.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    std::string error;
    HairFrameInput frame = frameOf(fixture, view, debugView);
    frame.sortStrands = sortStrands;
    frame.headTransform = headTransform;
    require(draw.draw(rig.commands.p, rig.ticket(), frame, material, lighting, error),
        "hair draw failed: " + error);
    rig.commands.p->EndQuery(targets.queries.p, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
    targets.resolveCounters(rig);
    Render result;
    result.pixels = targets.image(rig);
    result.metrics = measure(result.pixels);
    result.stats = draw.stats();
    result.counters = targets.counters;
    drawnVertices += result.counters.ia;
    shadedPixels += result.counters.ps;
    return result;
}

void recomputeHash(std::vector<std::uint8_t>& bytes) {
    const auto read = [&bytes](std::uint64_t offset) {
        std::uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, 4);
        return value;
    };
    std::string payload;
    for (int index = 0; index < 5; ++index) {
        const std::uint64_t offset = read(88 + index * 4), size = read(108 + index * 4);
        require(offset + size <= bytes.size(), "corrupt fixture section leaves the file");
        payload.append(reinterpret_cast<const char*>(bytes.data() + offset), static_cast<std::size_t>(size));
    }
    const auto hash = engine::assets::sha256(payload);
    std::memcpy(bytes.data() + 152, hash.data(), 64);
}

void expectReject(std::vector<std::uint8_t> bytes, const std::string& expected, const std::string& label) {
    const auto path = std::filesystem::temp_directory_path() / ("nativegpu-hair-reject-" + label + ".hgp");
    {
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::string message;
    try {
        engine::hair::loadGpuPack(path.u8string());
    } catch (const engine::hair::Error& error) {
        message = error.code + ": " + error.what();
    }
    std::filesystem::remove(path);
    require(!message.empty(), label + " was accepted");
    require(message.find(expected) != std::string::npos,
        label + " failed for the wrong reason: " + message + " (expected " + expected + ")");
    ++checks;
}

void readerEvidence(const GpuPack& pack, const Fixture& fixture, const std::string& packPath) {
    const auto& expected = fixture.pack;
    require(pack.strandCount == expected.at("strands").get<std::uint32_t>(), "pack strand count disagrees with the fixture");
    require(pack.pointCount == expected.at("points").get<std::uint32_t>(), "pack point count disagrees with the fixture");
    require(pack.drawVertices == expected.at("drawVertices").get<std::uint32_t>(), "pack draw count disagrees with the fixture");
    require(pack.hairHash == expected.at("hairHash").get<std::string>(), "pack lost the cooked hairstyle hash");
    require(pack.segmentsPerStrand >= 1 && pack.pointCount == pack.strandCount * (pack.segmentsPerStrand + 1),
        "pack point table is inconsistent");
    ++groups;

    const auto original = readBytes(packPath);
    require(original.size() >= kHeaderBytes, "fixture pack is empty");
    auto patch = [&](std::size_t offset, std::uint32_t value, bool rehash) {
        auto copy = original;
        std::memcpy(copy.data() + offset, &value, 4);
        if (rehash) recomputeHash(copy);
        return copy;
    };
    expectReject(patch(0, 0x21444748, false), "magic", "magic");
    expectReject(patch(4, 9, false), "version", "version");
    expectReject(patch(40, 24, false), "strides", "stride");
    expectReject(patch(108, 12, false), "section size", "section-size");
    expectReject(patch(56, 0x7fc00000, false), "bounds", "bounds");
    {
        auto copy = original;
        const std::uint32_t firstSection = [&copy] {
            std::uint32_t value = 0;
            std::memcpy(&value, copy.data() + 88, 4);
            return value;
        }();
        copy[firstSection] ^= 0xff;
        expectReject(copy, "declared hash", "payload");
        const std::uint32_t lastOffset = [&copy] {
            std::uint32_t offset = 0, size = 0;
            std::memcpy(&offset, copy.data() + 88 + 4 * 4, 4);
            std::memcpy(&size, copy.data() + 108 + 4 * 4, 4);
            return offset + size;
        }();
        auto truncated = original;
        truncated.resize(lastOffset - 1);
        expectReject(truncated, "leaves the file", "truncated");
    }
    {
        // Duplicate stable id: patch the second strand's id, then re-authenticate the payload
        // so the id check itself is what rejects the file.
        auto copy = original;
        std::uint32_t firstStrandSection = 0;
        std::memcpy(&firstStrandSection, copy.data() + 88 + 4, 4);
        std::uint32_t id = 0;
        std::memcpy(&id, copy.data() + firstStrandSection + 12, 4);
        std::memcpy(copy.data() + firstStrandSection + 32 + 12, &id, 4);
        std::uint32_t attributeSection = 0;
        std::memcpy(&attributeSection, copy.data() + 88 + 4 * 4, 4);
        std::memcpy(copy.data() + attributeSection + 16 + 12, &id, 4);
        recomputeHash(copy);
        expectReject(copy, "stable id", "duplicate-id");
    }
    {
        auto copy = original;
        std::uint32_t strandSection = 0;
        std::memcpy(&strandSection, copy.data() + 88 + 4, 4);
        const std::uint32_t badGuide = 0xffffffffu;
        std::memcpy(copy.data() + strandSection + 16, &badGuide, 4);
        recomputeHash(copy);
        expectReject(copy, "guide", "guide-index");
    }
    {
        auto copy = original;
        std::uint32_t pointSection = 0;
        std::memcpy(&pointSection, copy.data() + 88, 4);
        const std::uint32_t notFinite = 0x7fc00000u;
        std::memcpy(copy.data() + pointSection, &notFinite, 4);
        recomputeHash(copy);
        expectReject(copy, "finite", "non-finite");
    }
    ++groups;
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::filesystem::path packPath = "build/hair-draw/hair-pack.hgp";
        std::filesystem::path fixturePath = "build/hair-draw/fixture.json";
        std::filesystem::path output = "build/hair-draw";
        std::string adapter = "AMD";
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--pack" && index + 1 < argc) packPath = std::filesystem::u8path(argv[++index]);
            else if (argument == "--fixture" && index + 1 < argc) fixturePath = std::filesystem::u8path(argv[++index]);
            else if (argument == "--out" && index + 1 < argc) output = std::filesystem::u8path(argv[++index]);
            else if (argument == "--adapter" && index + 1 < argc) adapter = argv[++index];
            else throw std::runtime_error("Usage: native_hair_draw_tests [--pack p] [--fixture f] [--out d] [--adapter name]");
        }
        if (adapter == "any") adapter.clear();
        std::filesystem::create_directories(output);

        Fixture fixture = loadFixture(fixturePath.u8string());
        require(fixture.width == Targets::size && fixture.height == Targets::size,
            "the fixture resolution must match the evidence target");
        GpuPack pack = engine::hair::loadGpuPack(packPath.u8string());
        readerEvidence(pack, fixture, packPath.u8string());

        Rig rig(adapter);
        Targets targets(rig);
        HairDraw draw(rig.device.p, rig.fence.p, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_D32_FLOAT);
        upload(rig, draw, pack);

        const auto frontView = fixture.view("front");
        const auto backView = fixture.view("back");
        const auto sideView = fixture.view("three_quarter_left");
        const auto front = renderFrame(rig, targets, draw, fixture, frontView, fixture.material, fixture.keyLight);
        const auto back = renderFrame(rig, targets, draw, fixture, backView, fixture.material, fixture.keyLight);
        const auto side = renderFrame(rig, targets, draw, fixture, sideView, fixture.material, fixture.keyLight);
        const auto closeup = renderFrame(rig, targets, draw, fixture, fixture.view("hairline_closeup"),
            fixture.material, fixture.keyLight);
        ++groups;

        require(front.counters.ia == pack.drawVertices, "the draw did not submit six vertices per segment");
        require(front.counters.vs > 0 && front.counters.ps > 0, "the strand draw recorded no vertex or pixel work");
        require(front.metrics.coverageFraction > 0.03 && front.metrics.coverageFraction < 0.8,
            "the front view covers an implausible fraction of the frame");
        require(front.metrics.covered > 2000, "the front view drew almost nothing");
        require(back.metrics.coverageFraction > 0.03 && side.metrics.coverageFraction > 0.03,
            "back or three-quarter coverage collapsed");
        require(closeup.metrics.coverageFraction > 0.05, "the hairline close-up covered nothing");
        require(differingPixels(front.pixels, back.pixels, 1e-4f) > 500, "front and back are the same image");
        require(differingPixels(front.pixels, side.pixels, 1e-4f) > 500, "front and three-quarter are the same image");
        targets.save(rig, output, "front", front.pixels);
        targets.save(rig, output, "back", back.pixels);
        targets.save(rig, output, "three-quarter", side.pixels);
        targets.save(rig, output, "closeup", closeup.pixels);
        // Rigid head attachment: rotate the hairstyle about the pack centre as a head pose
        // would. The silhouette must move; a pass that ignored headTransform would return
        // the same image and this is the control that catches it.
        const float pivot[3] = {
            (pack.boundsMin[0] + pack.boundsMax[0]) * 0.5f,
            (pack.boundsMin[1] + pack.boundsMax[1]) * 0.5f,
            (pack.boundsMin[2] + pack.boundsMax[2]) * 0.5f,
        };
        const Mat4 headTurn = rotationAboutY(25.0f * 3.14159265358979323846f / 180.0f, pivot);
        const auto turned = renderFrame(rig, targets, draw, fixture, frontView, fixture.material,
            fixture.keyLight, 0, true, headTurn);
        headTurnChangedPixels = differingPixels(front.pixels, turned.pixels, 1e-4f);
        require(headTurnChangedPixels > 500, "a 25 degree head turn did not move the hairstyle");
        targets.save(rig, output, "head-turn", turned.pixels);
        targets.save(rig, output, "front-coverage", front.pixels, true);
        gReport = {{"draw", {
            {"coverageFraction", front.metrics.coverageFraction},
            {"frontLuminance", front.metrics.meanLuminance},
            {"iaVertices", front.counters.ia}, {"vsInvocations", front.counters.vs},
            {"psInvocations", front.counters.ps},
            {"shadowCoveredTexels", front.stats.shadowCoveredTexels},
            {"shadowMaxOpticalDepthMetres", front.stats.shadowMaxOpticalDepthMetres},
        }}};

        const auto repeat = renderFrame(rig, targets, draw, fixture, frontView, fixture.material, fixture.keyLight);
        require(differingPixels(front.pixels, repeat.pixels, 0.0f) == 0, "the same draw is not deterministic");
        require(repeat.counters.ia == front.counters.ia && repeat.counters.ps == front.counters.ps,
            "a repeated draw recorded different work");
        // Depth sorting is not cosmetic: with alpha blending the same geometry composites
        // differently when the order is upload order instead of far-to-near.
        require(front.stats.depthSorted && front.stats.sortedStrands >= pack.strandCount,
            "the draw did not report a depth-sorted order");
        const auto unsorted = renderFrame(rig, targets, draw, fixture, frontView, fixture.material,
            fixture.keyLight, 0, false);
        require(!unsorted.stats.depthSorted, "the unsorted control also reported a sorted order");
        sortingChangedPixels = differingPixels(front.pixels, unsorted.pixels, 1e-4f);
        require(sortingChangedPixels > 100, "depth sorting did not change the composite");
        targets.save(rig, output, "unsorted-front", unsorted.pixels);
        ++groups;

        const auto coverage = renderFrame(rig, targets, draw, fixture, frontView, fixture.material, fixture.keyLight, 1);
        const auto tangent = renderFrame(rig, targets, draw, fixture, frontView, fixture.material, fixture.keyLight, 2);
        const auto rootTip = renderFrame(rig, targets, draw, fixture, frontView, fixture.material, fixture.keyLight, 3);
        const auto ids = renderFrame(rig, targets, draw, fixture, frontView, fixture.material, fixture.keyLight, 4);
        const auto shadowView = renderFrame(rig, targets, draw, fixture, frontView, fixture.material, fixture.keyLight, 7);
        require(coverage.metrics.covered > 2000, "the coverage channel is empty");
        require(tangent.metrics.coverageFraction > 0, "the tangent channel is empty");
        require(differingPixels(front.pixels, tangent.pixels, 1e-4f) > 500, "the tangent channel duplicates the shaded frame");
        require(differingPixels(tangent.pixels, rootTip.pixels, 1e-4f) > 500, "tangent and root-to-tip agree");
        require(differingPixels(ids.pixels, front.pixels, 1e-4f) > 500, "the strand-id channel duplicates the shaded frame");
        require(shadowView.metrics.covered > 2000, "the shadow channel is empty");
        targets.save(rig, output, "debug-coverage", coverage.pixels, true);
        targets.save(rig, output, "debug-tangent", tangent.pixels);
        targets.save(rig, output, "debug-roottip", rootTip.pixels);
        targets.save(rig, output, "debug-shadow", shadowView.pixels);
        ++groups;

        auto unshadowedLight = fixture.keyLight;
        unshadowedLight.shadowStrength = 0;
        const auto unshadowed = renderFrame(rig, targets, draw, fixture, sideView, fixture.material, unshadowedLight);
        require(front.stats.shadowCoveredTexels > 0, "the deep-opacity grid received no strands");
        require(front.stats.shadowMaxOpticalDepthMetres > 0, "the deep-opacity grid accumulated no optical depth");
        require(front.stats.shadowBuilds >= 1, "the grid was never built");
        require(unshadowed.metrics.meanLuminance > 0 && side.metrics.meanLuminance < unshadowed.metrics.meanLuminance * 0.98,
            "deep-opacity self-shadowing barely changed the same view");
        targets.save(rig, output, "shadow-off", unshadowed.pixels);
        targets.save(rig, output, "shadow-on", side.pixels);
        ++groups;

        auto lightHair = fixture.material;
        lightHair.albedo[0] = 0.32f; lightHair.albedo[1] = 0.22f; lightHair.albedo[2] = 0.12f;
        auto darkHair = fixture.material;
        darkHair.albedo[0] = 0.02f; darkHair.albedo[1] = 0.014f; darkHair.albedo[2] = 0.01f;
        const auto brightBacklit = renderFrame(rig, targets, draw, fixture, frontView, lightHair, fixture.rimLight);
        const auto darkBacklit = renderFrame(rig, targets, draw, fixture, frontView, darkHair, fixture.rimLight);
        require(brightBacklit.metrics.meanLuminance > darkBacklit.metrics.meanLuminance * 1.15,
            "blond hair does not transmit visibly more than black hair under backlight");
        targets.save(rig, output, "backlight-light", brightBacklit.pixels);
        targets.save(rig, output, "backlight-dark", darkBacklit.pixels);

        auto tinted = fixture.material;
        tinted.rootToTipTint[0] = 0.4f; tinted.rootToTipTint[1] = 0.3f; tinted.rootToTipTint[2] = 0.2f;
        const auto tintedRender = renderFrame(rig, targets, draw, fixture, frontView, tinted, fixture.keyLight);
        require(tintedRender.metrics.meanLuminance < front.metrics.meanLuminance * 0.98,
            "the root-to-tip tint did not darken the tips");
        targets.save(rig, output, "tint-on", tintedRender.pixels);
        ++groups;

        {
            // Guide simulation: the solver moves guides at a fixed tick, the draw consumes
            // interpolated strands, and a reset must reproduce the first solve.
            std::string error;
            HairSimulation calmSettings;
            calmSettings.enabled = true;
            require(draw.setSimulation(calmSettings, error), "simulation setup failed: " + error);
            const auto runFrames = [&](int count) {
                Render last;
                for (int frameIndex = 0; frameIndex < count; ++frameIndex)
                    last = renderFrame(rig, targets, draw, fixture, frontView, fixture.material, fixture.keyLight);
                return last;
            };
            const auto calm = runFrames(61);
            require(calm.stats.simulating, "the draw did not report simulation");
            require(calm.stats.simulationTicks >= 61, "the guide solver did not advance every frame");
            require(calm.stats.simulationMaxStretch <= 1.05f,
                "the guide solver stretched a segment past 5%");
            require(calm.stats.simulationMaxPenetrationMetres <= 1e-3f,
                "guides ended inside the head sphere");
            require(calm.stats.simulationMaxRootDriftMetres == 0.0f, "a pinned root moved during the solve");
            require(calm.stats.simulationStaleUploads == 0, "a simulated frame could not upload its points");

            draw.resetSimulation();
            const auto repeated = runFrames(61);
            require(differingPixels(calm.pixels, repeated.pixels, 0.0f) == 0,
                "reset did not reproduce the same solved frame");

            HairSimulation windSettings = calmSettings;
            windSettings.wind[0] = 5.0f;
            windSettings.wind[2] = 2.0f;
            require(draw.setSimulation(windSettings, error), "wind setup failed: " + error);
            const auto wind = runFrames(61);
            require(differingPixels(wind.pixels, calm.pixels, 1e-4f) > 500,
                "wind did not move the rendered strands");
            require(wind.stats.simulationMaxSpeedMetresPerSecond
                > calm.stats.simulationMaxSpeedMetresPerSecond,
                "wind did not increase the strand speed");
            targets.save(rig, output, "simulation-calm", calm.pixels);
            targets.save(rig, output, "simulation-wind", wind.pixels);
            simulationTicks = calm.stats.simulationTicks;
            simulationMaxStretch = calm.stats.simulationMaxStretch;
            simulationMaxPenetration = calm.stats.simulationMaxPenetrationMetres;
            simulationMaxRootDrift = calm.stats.simulationMaxRootDriftMetres;
            simulationCalmSpeed = calm.stats.simulationMaxSpeedMetresPerSecond;
            simulationWindSpeed = wind.stats.simulationMaxSpeedMetresPerSecond;
            simulationWindChanged = differingPixels(wind.pixels, calm.pixels, 1e-4f);
            gReport["simulation"] = {
                {"ticks", simulationTicks},
                {"maxStretch", simulationMaxStretch},
                {"maxPenetrationMetres", simulationMaxPenetration},
                {"maxRootDriftMetres", simulationMaxRootDrift},
                {"calmSpeed", simulationCalmSpeed},
                {"windSpeed", simulationWindSpeed},
                {"windChangedPixels", simulationWindChanged},
            };
            ++groups;
        }

        {
            std::string error;
            require(!draw.draw(rig.commands.p, rig.submitted, frameOf(fixture, frontView), fixture.material,
                fixture.keyLight, error) && !error.empty(), "a completed fence was accepted");
            auto badLight = fixture.keyLight;
            badLight.direction[0] = badLight.direction[1] = badLight.direction[2] = 0;
            require(!draw.draw(rig.commands.p, rig.ticket(), frameOf(fixture, frontView), fixture.material,
                badLight, error), "a zero light direction was accepted");
            auto badMaterial = fixture.material;
            badMaterial.roughness = 0;
            require(!draw.draw(rig.commands.p, rig.ticket(), frameOf(fixture, frontView), badMaterial,
                fixture.keyLight, error), "an invalid roughness was accepted");
            auto badFrame = frameOf(fixture, frontView);
            badFrame.viewProjection.m[0] = std::numeric_limits<float>::quiet_NaN();
            require(!draw.draw(rig.commands.p, rig.ticket(), badFrame, fixture.material, fixture.keyLight, error),
                "a non-finite view projection was accepted");
            auto badDebug = frameOf(fixture, frontView, 9);
            require(!draw.draw(rig.commands.p, rig.ticket(), badDebug, fixture.material, fixture.keyLight, error),
                "an unknown debug channel was accepted");
            HairDraw stale(rig.device.p, rig.fence.p, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_D32_FLOAT);
            require(!stale.draw(rig.commands.p, rig.ticket(), frameOf(fixture, frontView), fixture.material,
                fixture.keyLight, error), "a draw without an upload was accepted");
            require(stale.shutdown(error), "an unused draw did not shut down");
            ++groups;
        }

        {
            rig.begin();
            targets.bind(rig);
            std::string error;
            require(draw.draw(rig.commands.p, rig.ticket(), frameOf(fixture, frontView), fixture.material,
                fixture.keyLight, error), "the in-flight draw failed: " + error);
            rig.submit(false);
            require(!draw.shutdown(error) && !error.empty(), "an in-flight draw was shut down");
            rig.wait();
        }
        const auto finalStats = draw.stats();
        require(finalStats.frames >= 12, "the evidence did not draw every planned frame");
        require(finalStats.transparency == std::string("depth-sorted-alpha"),
            "the transparency mode reported by the draw changed");
        std::string error;
        require(draw.shutdown(error), "hair draw shutdown failed: " + error);
        require(!rig.debugErrors(), "the D3D12 debug layer reported an error");
        ++groups;

        gReport = {
            {"pack", {
                {"strands", pack.strandCount}, {"points", pack.pointCount},
                {"segmentsPerStrand", pack.segmentsPerStrand}, {"guides", pack.guideCount},
                {"drawVertices", pack.drawVertices}, {"uploadBytes", finalStats.uploadBytes},
                {"hash", pack.hash}, {"hairHash", pack.hairHash},
            }},
            {"draw", {
                {"backend", finalStats.backend}, {"transparency", finalStats.transparency},
                {"frames", finalStats.frames}, {"drawnVertices", drawnVertices}, {"shadedPixels", shadedPixels},
                {"coverageFraction", front.metrics.coverageFraction},
                {"frontLuminance", front.metrics.meanLuminance},
                {"depthSorted", front.stats.depthSorted},
                {"sortedStrands", front.stats.sortedStrands},
                {"orderStaleUploads", finalStats.orderStaleUploads},
                {"sortingChangedPixels", sortingChangedPixels},
                {"headTurnChangedPixels", headTurnChangedPixels},
                {"shadowedLuminance", side.metrics.meanLuminance},
                {"unshadowedLuminance", unshadowed.metrics.meanLuminance},
                {"shadow", {
                    {"gridSize", finalStats.shadowGridSize}, {"layers", finalStats.shadowLayers},
                    {"builds", finalStats.shadowBuilds}, {"coveredTexels", finalStats.shadowCoveredTexels},
                    {"crossings", finalStats.shadowCrossings}, {"droppedLayers", finalStats.shadowDroppedLayers},
                    {"maxOpticalDepthMetres", finalStats.shadowMaxOpticalDepthMetres},
                }},
            }},
            {"simulation", {
                {"backend", "cpu-guide"}, {"ticks", simulationTicks},
                {"maxStretch", simulationMaxStretch}, {"maxPenetrationMetres", simulationMaxPenetration},
                {"maxRootDriftMetres", simulationMaxRootDrift}, {"calmSpeed", simulationCalmSpeed},
                {"windSpeed", simulationWindSpeed}, {"windChangedPixels", simulationWindChanged},
            }},
            {"images", {"front", "back", "three-quarter", "front-coverage", "debug-coverage", "debug-tangent",
                        "debug-roottip", "debug-shadow", "shadow-on", "shadow-off", "backlight-light",
                        "backlight-dark", "tint-on", "simulation-calm", "simulation-wind", "closeup",
                        "unsorted-front", "head-turn"}},
            {"capabilities", {
                {"strandDraw", true}, {"coverageAlpha", true}, {"fibreShading", true},
                {"deepOpacitySelfShadow", true}, {"backlightTransmission", true}, {"debugChannels", true},
                {"guideSimulation", true}, {"gpuGuideSolve", false},
                {"depthSortedTransparency", true}, {"orderIndependentTransparency", false},
                {"sceneShadowCast", false},
            }},
        };
        std::cout << nlohmann::json({
            {"schema", "nativegpu.hair-draw-evidence/1"}, {"ok", true},
            {"groups", groups}, {"checks", checks}, {"adapter", rig.adapter},
            {"evidence", gReport},
        }).dump() << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hair draw evidence failed after " << checks << " checks: " << error.what() << std::endl;
        if (!gReport.empty()) std::cerr << "measured: " << gReport.dump() << std::endl;
        return 1;
    }
}
