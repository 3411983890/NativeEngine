// Native viewport host: a child HWND that renders vegetation with the real engine path,
// parented into the editor window.
//
// Why a child window instead of a shared texture: the editor already runs inside Electron,
// and a WS_CHILD window owned by this process can present a DXGI swap chain directly into
// the parent's client area. There is no readback, no encoding and no copy through the
// browser compositor. The cost is that Win32 owns the rectangle: z-order, DPI, clipping and
// focus have to be driven explicitly, which is why this file exists at all.
//
// Contract with the host process (Electron main):
//   - it passes `--parent <hwnd>` (the BrowserWindow's native handle) and a pipe name;
//   - this process creates a WS_CHILD window inside that parent, connects to the pipe and
//     then reports `hwnd=<value>` and `ready` on stdout;
//   - the host sends `x=..;y=..;w=..;h=..;v=1` lines to move, resize, show or hide it.
//
// Standalone mode (`--parent 0`) opens an overlapped window instead, which is how the
// render path is verified without launching the editor.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include "nativegpu_shaders.h"
#include "scene_preview.hpp"
#include "world_bridge.hpp"
#include "../engine/native/assets/sha256.hpp"
#include "../engine/native/vegetation/texture.hpp"
#include "../renderer/png_writer.h"
#include "../renderer/vegetation_draw.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Failure : std::runtime_error {
    explicit Failure(const std::string& message) : std::runtime_error(message) {}
};

void check(HRESULT result, const char* what) {
    if (FAILED(result)) {
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer), "%s (hr=0x%08lx)", what, static_cast<unsigned long>(result));
        throw Failure(buffer);
    }
}

template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) { p->Release(); p = nullptr; }
    }
    T** out() { reset(); return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

// ---------------------------------------------------------------- matrix helpers
// Column-major, column vectors, left-handed, 0..1 depth 鈥?the same conventions the
// vegetation shader and renderer/matrix_math.h use. Duplicated here on purpose: the host
// links only the renderer pieces it draws with, not the whole engine.
void multiply(const float a[16], const float b[16], float out[16]) {
    for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row) {
        float sum = 0;
        for (int k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[column * 4 + k];
        out[column * 4 + row] = sum;
    }
}

void perspective(float fovY, float aspect, float nearPlane, float farPlane, float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    const float f = 1.0f / std::tan(fovY * 0.5f);
    out[0] = f / aspect;
    out[5] = f;
    out[10] = farPlane / (farPlane - nearPlane);
    out[11] = 1.0f;
    out[14] = -nearPlane * farPlane / (farPlane - nearPlane);
}

void cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

void normalize(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length < 1e-6f) { v[0] = 0; v[1] = 1; v[2] = 0; return; }
    v[0] /= length; v[1] /= length; v[2] /= length;
}

void lookAt(const float eye[3], const float target[3], const float up[3], float out[16]) {
    float z[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    normalize(z);
    float x[3];
    cross(up, z, x);
    normalize(x);
    float y[3];
    cross(z, x, y);
    std::memset(out, 0, sizeof(float) * 16);
    out[0] = x[0]; out[1] = y[0]; out[2] = z[0];
    out[4] = x[1]; out[5] = y[1]; out[6] = z[1];
    out[8] = x[2]; out[9] = y[2]; out[10] = z[2];
    out[12] = -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]);
    out[13] = -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]);
    out[14] = -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]);
    out[15] = 1.0f;
}

void orthographic(float width, float height, float nearPlane, float farPlane, float out[16]) {
    std::memset(out, 0, sizeof(float) * 16);
    out[0] = 2.0f / width;
    out[5] = 2.0f / height;
    out[10] = 1.0f / (farPlane - nearPlane);
    out[14] = -nearPlane / (farPlane - nearPlane);
    out[15] = 1.0f;
}

// Half -> float, used only by the screenshot path: the swap chain is FP16 linear and the PNG
// writer takes 8-bit RGBA. A table would be faster, but this runs once per capture.
float halfToFloat(unsigned short bits) {
    const unsigned sign = (bits >> 15) & 0x1u;
    const unsigned exponent = (bits >> 10) & 0x1fu;
    const unsigned mantissa = bits & 0x3ffu;
    float value;
    if (exponent == 0) {
        value = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 31) {
        value = mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    } else {
        value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    }
    return sign ? -value : value;
}

// Linear -> sRGB encode. The compositor does this for the window; the PNG has to do it
// itself or the saved evidence is darker than what the user saw.
unsigned char encodeSrgb(float linear) {
    if (!(linear > 0.0f)) return 0;
    if (linear > 1.0f) linear = 1.0f;
    const float encoded = linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return static_cast<unsigned char>(std::lround(std::min(1.0f, std::max(0.0f, encoded)) * 255.0f));
}

std::string readFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw Failure("cannot open " + path);
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- control channel
// The pipe thread writes, the render thread reads; the state is a plain struct so it can be
// copied out under the lock instead of held across Win32 calls.
//
// The protocol is deliberately one line per message:
//
//   rect    x=..;y=..;w=..;h=..;v=0|1
//   load    cmd=load;req=..;rev=..;species=..;lod=..;digest=..[;x=..;y=..;w=..;h=..]
//   view    cmd=view;req=..;rev=..;debug=..;lod=..;wind=..;shadow=..
//   open    cmd=open;req=..;rev=..;mode=..;scene=..[;play=edit|simulate|run]
//   play    cmd=play;req=..;rev=..;play=edit|simulate|run;pause=0|1[;step=1]
//   camera  cmd=camera;req=..;rev=..;view=perspective|top|front|side[;focus=1][;entity=..]
//   preview cmd=preview;req=..;rev=..;entity=..;px=..;py=..;pz=..;sx=..;sy=..;sz=..;active=0|1
//   previewclear cmd=previewclear;req=..;rev=..
//   capture cmd=capture;req=..;rev=..;path=..
//   status  cmd=status;req=..
//
// and every applied command answers on stdout with one `ack=1;...` line. The editor is not
// allowed to claim "the native viewport shows revision N" until that line names revision N,
// which is why the ack carries the revision and the package digest rather than just "ok".
struct ControlState {
    bool visible = true;
    int x = 0, y = 0, width = 0, height = 0;
    std::string command = "rect";
    unsigned long long requestId = 0;
    unsigned long long revision = 0;
    std::string species;
    std::string digest;
    std::string mode = "vegetation";
    std::string scene;
    std::string asset;
    std::string material;
    std::string clip;
    std::string effect;
    std::string library;
    std::string cooked;
    std::string debug;
    std::string path;
    std::string play;
    std::string view;
    std::string entity;
    float px = 0, py = 0, pz = 0;
    float sx = 1, sy = 1, sz = 1;
    int active = -1;
    int paused = -1;
    int step = 0;
    int focus = 0;
    int lod = -1;
    float wind = -1.0f;
    float shadow = -1.0f;
    float checkerCells = -1.0f;
};

struct Control {
    std::mutex mutex;
    // `state` is the cumulative rect/visibility state (each new line inherits the previous one);
    // `queue` holds every command in arrival order so none is dropped between frames.
    ControlState state;
    std::vector<ControlState> queue;
};

// Splits one `key=value;key=value` line. Values are strings: a species id and a debug view name
// are not integers, and the previous parser ran every value through `atoi`, which silently turned
// `species=oak_broadleaf` into 0.
bool parseControlLine(const std::string& line, Control& control) {
    // Bounded: a control line is a handful of short pairs. Anything longer is a protocol error,
    // and scanning an unbounded line for news is how a pipe becomes an input channel.
    if (line.empty() || line.size() > 1024) return false;
    std::lock_guard<std::mutex> guard(control.mutex);
    ControlState next = control.state;
    // Per-line fields start clean: a later line must not inherit the previous command or its
    // species, or "resize" would re-apply the last species every time the window moved.
    next.command = "rect";
    next.requestId = 0;
    next.species.clear();
    next.digest.clear();
    next.mode = "vegetation";
    next.scene.clear();
    next.asset.clear();
    next.material.clear();
    next.clip.clear();
    next.effect.clear();
    next.library.clear();
    next.cooked.clear();
    next.debug.clear();
    next.path.clear();
    next.play.clear();
    next.view.clear();
    next.entity.clear();
    next.px = next.py = next.pz = 0.0f;
    next.sx = next.sy = next.sz = 1.0f;
    next.active = -1;
    next.paused = -1;
    next.step = 0;
    next.focus = 0;
    next.lod = -1;
    next.wind = -1.0f;
    next.shadow = -1.0f;
    next.checkerCells = -1.0f;
    next.revision = control.state.revision;

    std::size_t cursor = 0;
    while (cursor < line.size()) {
        while (cursor < line.size() && (line[cursor] == ';' || line[cursor] == ' ' || line[cursor] == '\r')) ++cursor;
        if (cursor >= line.size()) break;
        const std::size_t equals = line.find('=', cursor);
        if (equals == std::string::npos) break;
        const std::string key = line.substr(cursor, equals - cursor);
        const std::size_t end = line.find(';', equals + 1);
        std::string value = line.substr(equals + 1, (end == std::string::npos ? line.size() : end) - equals - 1);
        if (!value.empty() && value.back() == '\r') value.pop_back();
        cursor = end == std::string::npos ? line.size() : end + 1;

        if (key == "x") next.x = std::atoi(value.c_str());
        else if (key == "y") next.y = std::atoi(value.c_str());
        else if (key == "w") next.width = std::atoi(value.c_str());
        else if (key == "h") next.height = std::atoi(value.c_str());
        else if (key == "v") next.visible = std::atoi(value.c_str()) != 0;
        else if (key == "cmd") next.command = value;
        else if (key == "req") next.requestId = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "rev") next.revision = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "species") next.species = value;
        else if (key == "digest") next.digest = value;
        else if (key == "mode") next.mode = value;
        else if (key == "scene") next.scene = value;
        else if (key == "asset") next.asset = value;
        else if (key == "material") next.material = value;
        else if (key == "clip") next.clip = value;
        else if (key == "effect") next.effect = value;
        else if (key == "library") next.library = value;
        else if (key == "cooked") next.cooked = value;
        else if (key == "debug") next.debug = value;
        else if (key == "path") next.path = value;
        else if (key == "play") next.play = value;
        else if (key == "view") next.view = value;
        else if (key == "entity") next.entity = value;
        else if (key == "px") next.px = static_cast<float>(std::atof(value.c_str()));
        else if (key == "py") next.py = static_cast<float>(std::atof(value.c_str()));
        else if (key == "pz") next.pz = static_cast<float>(std::atof(value.c_str()));
        else if (key == "sx") next.sx = static_cast<float>(std::atof(value.c_str()));
        else if (key == "sy") next.sy = static_cast<float>(std::atof(value.c_str()));
        else if (key == "sz") next.sz = static_cast<float>(std::atof(value.c_str()));
        else if (key == "active") next.active = std::atoi(value.c_str()) != 0 ? 1 : 0;
        else if (key == "pause") next.paused = std::atoi(value.c_str()) != 0 ? 1 : 0;
        else if (key == "step") next.step = std::atoi(value.c_str()) != 0 ? 1 : 0;
        else if (key == "focus") next.focus = std::atoi(value.c_str()) != 0 ? 1 : 0;
        else if (key == "lod") next.lod = std::atoi(value.c_str());
        else if (key == "wind") next.wind = static_cast<float>(std::atof(value.c_str()));
        else if (key == "shadow") next.shadow = static_cast<float>(std::atof(value.c_str()));
        else if (key == "cells") next.checkerCells = static_cast<float>(std::atof(value.c_str()));
        // Unknown keys are ignored on purpose: a newer editor talking to an older host must not
        // be able to wedge the pipe, and the ack says which revision was actually applied.
    }
    control.state = next;
    // Bound the queue: a client that writes thousands of lines between two frames must not make
    // the host allocate without limit. Older entries are dropped from the front, which keeps the
    // most recent intent (and every command still gets acknowledged once dequeued).
    if (control.queue.size() >= 128) control.queue.erase(control.queue.begin(), control.queue.begin() + 64);
    control.queue.push_back(next);
    return true;
}

// Reads newline-delimited control lines from the named pipe until the pipe closes. Runs on
// its own thread because the render loop must not block on ReadFile.
void controlLoop(HANDLE pipe, Control& control) {
    std::string buffer;
    char chunk[512];
    for (;;) {
        DWORD bytes = 0;
        if (!ReadFile(pipe, chunk, sizeof(chunk), &bytes, nullptr) || bytes == 0) return;
        buffer.append(chunk, bytes);
        for (;;) {
            const std::size_t newline = buffer.find('\n');
            if (newline == std::string::npos) break;
            parseControlLine(buffer.substr(0, newline), control);
            buffer.erase(0, newline + 1);
        }
    }
}

HANDLE connectPipe(const std::string& name) {
    const std::string path = "\\\\.\\pipe\\" + name;
    // The host creates the server right after spawning us, so a short retry window is
    // enough; failing loudly is better than silently running with no way to be moved.
    for (int attempt = 0; attempt < 60; ++attempt) {
        HANDLE pipe = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        Sleep(50);
    }
    throw Failure("cannot connect to control pipe " + path);
}

// ---------------------------------------------------------------- assets
struct LoadedAssets {
    engine::vegetation::Species species;
    nativegpu::VegetationMeshData mesh;
    std::vector<engine::vegetation::PackedInstance> instances;
    engine::vegetation::PageRecord page;
    nativegpu::VegetationTextureSet textures;
    nativegpu::VegetationMaterial material;
    std::string speciesId;
    // sha256 of the package bytes that were actually parsed. The editor compares this to the
    // digest it baked; matching ids with different bytes is how "the viewport shows the old
    // tree" happens without anyone lying.
    std::string digest;
    unsigned lodLevel = 0;
    unsigned triangleCount = 0;
    float bounds[3] = {1, 1, 0};
};

std::string libraryFileFor(const std::string& libraryJson, const std::string& id) {
    const auto library = nlohmann::json::parse(libraryJson);
    for (const auto& entry : library.at("species")) {
        if (entry.at("id").get<std::string>() == id) return entry.at("file").get<std::string>();
    }
    throw Failure("species " + id + " is not in the cooked library");
}

nativegpu::VegetationTexture toDrawTexture(const engine::vegetation::TextureImage& image) {
    nativegpu::VegetationTexture texture;
    texture.width = image.width;
    texture.height = image.height;
    texture.srgb = image.srgb;
    for (const auto& level : image.levels) {
        nativegpu::VegetationTextureLevel target;
        target.width = level.width;
        target.height = level.height;
        target.pixels = level.pixels;
        texture.levels.push_back(std::move(target));
    }
    return texture;
}

nativegpu::VegetationTextureSet loadTextures(const engine::vegetation::Species& species,
                                            const std::string& directory) {
    nativegpu::VegetationTextureSet set;
    const auto library = nlohmann::json::parse(readFile(directory + "/library.json"));
    const auto byRole = [&](const std::string& role) -> nlohmann::json {
        for (const auto& entry : library.at("roles")) {
            if (entry.at("role").get<std::string>() == role) return entry;
        }
        return nlohmann::json{};
    };
    // A slot that the species does not declare stays empty, and the shader's texture bitfield
    // says so. Silently substituting another role here is how a height map became "roughness".
    const auto assign = [&](nativegpu::VegetationTexture& target, const nlohmann::json& roles, const char* slot) {
        if (!roles.is_object() || !roles.contains(slot) || roles.at(slot).is_null()) return;
        const auto role = roles.at(slot).at("role").get<std::string>();
        const auto entry = byRole(role);
        if (entry.is_null() || !entry.contains("file")) throw Failure("texture role " + role + " is not cooked");
        const auto image = engine::vegetation::parseTexture(readFile(directory + "/" + entry.at("file").get<std::string>()));
        target = toDrawTexture(image);
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

// The vegetation draw path takes one page of one species. A species viewport is exactly
// that: N instances of the selected species on a ring, in one synthetic page. Drawing the
// real region would need per-instance LOD buckets and a multi-page constant ring, which is
// tracked separately in .developmentAgent/vegetation/VEGETATION-DEFECTS-AND-PLAN.md (A2/A3).
LoadedAssets loadAssets(const std::string& speciesId, const std::string& cookedDirectory,
                        const std::string& textureDirectory, unsigned instanceCount,
                        unsigned lodLevel = 0, const std::string& expectedDigest = {}) {
    LoadedAssets assets;
    const std::string file = libraryFileFor(readFile(cookedDirectory + "/library.json"), speciesId);
    // Read once: the digest and the parse have to describe the same bytes, and reading the file
    // twice would let a concurrent bake produce a package that authenticates against nothing.
    const std::string bytes = readFile(cookedDirectory + "/" + file);
    assets.digest = engine::assets::sha256(bytes);
    assets.speciesId = speciesId;
    assets.lodLevel = lodLevel;
    if (!expectedDigest.empty() && expectedDigest != assets.digest) {
        throw Failure("cooked package " + speciesId + " has digest " + assets.digest.substr(0, 16)
                      + ", the editor asked for " + expectedDigest.substr(0, 16));
    }
    assets.species = engine::vegetation::parseSpecies(bytes, assets.digest);
    if (lodLevel >= assets.species.lod.size()) {
        throw Failure("species " + speciesId + " has no lod " + std::to_string(lodLevel));
    }
    assets.textures = loadTextures(assets.species, textureDirectory);
    assets.material = nativegpu::VegetationMaterial{};
    if (!nativegpu::materialFromSpecies(assets.species, assets.material)) {
        // A species with no foliage material still draws (a bare trunk is legal), so this is not
        // an error; the material stays at its defaults and the ack reports it.
        assets.material = nativegpu::VegetationMaterial{};
    }

    const auto& mesh = assets.species.meshForLod(lodLevel);
    assets.mesh.positions = mesh.positions;
    assets.mesh.normals = mesh.normals;
    assets.mesh.uvs = mesh.uvs;
    assets.mesh.windWeights = mesh.windWeights;
    assets.mesh.colors = mesh.colors;
    assets.mesh.indices = mesh.indices;
    assets.triangleCount = static_cast<unsigned>(mesh.indices.size() / 3);
    unsigned first = 0, count = 0;
    bool any = false;
    for (const auto& primitive : assets.species.primitives) {
        if (primitive.lod != lodLevel) continue;
        nativegpu::VegetationPrimitive target;
        target.material = primitive.material;
        target.firstIndex = primitive.firstIndex;
        target.indexCount = primitive.indexCount;
        assets.mesh.primitives.push_back(target);
        first = any ? std::min(first, primitive.firstIndex) : primitive.firstIndex;
        count = any ? std::max(count, primitive.firstIndex + primitive.indexCount)
                    : primitive.firstIndex + primitive.indexCount;
        any = true;
    }
    assets.mesh.firstIndex = first;
    assets.mesh.indexCount = any ? count - first : 0;

    const auto& lod = assets.species.lod.at(lodLevel);
    assets.bounds[0] = lod.heightMetres;
    assets.bounds[1] = lod.radiusMetres;
    assets.bounds[2] = 0.0f;

    // Page frame: instance positions are quantized into [0, 1] across this box, so the box
    // has to cover every instance plus the tree radius. A tight box keeps the 16-bit
    // position error (0.48 mm at region scale) far below anything visible.
    const float height = std::max(0.2f, lod.heightMetres);
    const float radius = std::max(0.1f, lod.radiusMetres);
    const float ring = instanceCount > 1 ? radius * 2.6f * std::sqrt(static_cast<float>(instanceCount)) : 0.0f;
    const float extent = ring + radius * 1.35f;
    assets.page.minX = -extent;
    assets.page.maxX = extent;
    assets.page.minZ = -extent;
    assets.page.maxZ = extent;
    assets.page.baseY = 0.0f;
    assets.page.heightRange = height * 1.15f;
    assets.page.firstInstance = 0;
    assets.page.instanceCount = instanceCount;
    assets.page.pageIndex = 0;

    const auto quantize = [](float value, float lower, float span) {
        const float normalized = span > 1e-6f ? (value - lower) / span : 0.0f;
        const float clamped = std::min(1.0f, std::max(0.0f, normalized));
        return static_cast<unsigned short>(std::lround(clamped * 65535.0f));
    };
    for (unsigned index = 0; index < instanceCount; ++index) {
        engine::vegetation::PackedInstance instance;
        const float angle = static_cast<float>(index) / std::max(1u, instanceCount) * 6.2831853f;
        const float px = ring * std::cos(angle);
        const float pz = ring * std::sin(angle);
        instance.qx = quantize(px, assets.page.minX, assets.page.maxX - assets.page.minX);
        instance.qz = quantize(pz, assets.page.minZ, assets.page.maxZ - assets.page.minZ);
        instance.qy = 0;
        // Yaw is uniform over the full turn; the low byte of a 16-bit field is 0.0055 deg,
        // which is far below anything a leaf card can show.
        instance.yaw = static_cast<unsigned short>((index * 2654435761u) & 0xFFFFu);
        instance.tiltX = 0;
        instance.tiltZ = 0;
        instance.scale = 128;  // exp2(128/255*8-4) = 1.0
        instance.species = 0;
        instance.windPhase = static_cast<unsigned short>((index * 40503u) & 0xFFFFu);
        instance.lodBias = 0;
        instance.flags = 0;
        instance.palette = static_cast<unsigned short>(index * 8191u);
        instance.stableId = index;
        instance.aggregate = 0xFFFFFFFFu;
        instance.reserved = 0;
        assets.instances.push_back(instance);
    }
    return assets;
}

// ---------------------------------------------------------------- host
constexpr wchar_t kWindowClass[] = L"NativeGpuViewportHost";
// The vegetation shader writes linear radiance, so the display chain has to be linear too.
// Measured on this machine (tests/probe_swapchain.cpp):
//   flip_discard + B8G8R8A8_UNORM       ok, but treats the values as already sRGB -> dark
//   flip_discard + B8G8R8A8_UNORM_SRGB  rejected, 0x887A0001
//   discard       + anything            rejected, 0x887A0001
//   flip_discard + R16G16B16A16_FLOAT   ok, and accepts DXGI_COLOR_SPACE_RGB_FULL_G10
// So the viewport presents a half-float linear surface and lets the compositor convert.
constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_SWAP_EFFECT kSwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
constexpr UINT kRearmTimer = 1;
constexpr UINT_PTR kRaiseTimerId = 42;

struct Camera {
    float yaw = 0.62f, pitch = -0.18f, distance = 24.0f;
    float target[3] = {0, 6, 0};
    bool dragging = false;
    POINT last{};
};

enum class NavMode { None, Orbit, Pan, Look };

struct Host {
    HWND window = nullptr;
    HWND parent = nullptr;
    unsigned width = 0, height = 0;
    Control control;

    Com<ID3D12Device> device;
    Com<ID3D12CommandQueue> queue;
    Com<ID3D12CommandAllocator> allocator;
    Com<ID3D12GraphicsCommandList> list;
    Com<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    std::uint64_t fenceValue = 0;

    Com<IDXGISwapChain3> swapChain;
    static constexpr unsigned kBuffers = 3;
    Com<ID3D12Resource> backBuffers[kBuffers];
    unsigned bufferCount = kBuffers;
    bool presented[kBuffers] = {false, false, false};
    Com<ID3D12DescriptorHeap> rtvHeap;
    unsigned rtvStride = 0;
    Com<ID3D12Resource> depth;
    Com<ID3D12DescriptorHeap> dsvHeap;

    // GPU timing. Four timestamps per frame 鈥?list start, after the shadow pass, after the colour
    // pass, end of list 鈥?resolved into a ring of readback slots. The ring exists because a
    // timestamp can only be read after the fence that covers its frame, and blocking on that
    // every frame would make the measurement the dominant cost.
    static constexpr unsigned kTimingSlots = 8;
    Com<ID3D12QueryHeap> queryHeap;
    Com<ID3D12Resource> queryReadback;
    std::uint64_t queryFrequency = 0;
    unsigned timingSlot = 0;
    unsigned timingResolved = 0;
    std::vector<double> gpuTotalMs, gpuShadowMs, gpuColorMs;
    double presentMsSum = 0.0;
    unsigned presentFrames = 0;
    std::string timingJsonPath;

    std::unique_ptr<nativegpu::VegetationDraw> draw;
    LoadedAssets assets;
    Camera camera;
    // Editor navigation shares the mouse with the vegetation preview but never mutates
    // the game camera: scene mode forwards to the edit camera in host/main.cpp.
    NavMode nav = NavMode::None;
    POINT navLast{};
    bool navRightDown = false;

    // Where content comes from. Stored so a reload command can rebuild without the editor
    // re-sending paths it already passed on the command line.
    std::string cookedDirectory;
    std::string textureDirectory;
    std::filesystem::path projectRoot;
    std::string mode = "vegetation";
    bool sceneOpen = false;
    // World mode is independent of the scene preview: a world has no entities and no
    // revision, and threading it through the scene path would have meant lying about both.
    std::unique_ptr<host::WorldBridge> world;
    bool worldOpen = false;
    std::string worldDirectory;
    unsigned instanceCount = 1;

    // Applied state, published in every ack. `appliedRevision` only advances after the new
    // resources are uploaded *and* the next frame has been submitted, so the editor never
    // reports a version the GPU has not actually drawn.
    unsigned long long appliedRevision = 0;
    unsigned long long appliedRequest = 0;
    // Content requests that have been uploaded but not yet presented. A queue rather than a
    // single slot: two loads written back to back would otherwise overwrite each other and one
    // request would never be acknowledged, which the editor reads as a hung viewport.
    std::vector<ControlState> pendingContent;
    bool appliedPending = false;      // a reload succeeded but has not been drawn yet
    unsigned long long pendingRevision = 0;
    unsigned long long pendingRequest = 0;
    // View-only commands wait for the same frame as content commands, so every ack describes a
    // revision that was actually presented.
    std::vector<ControlState> pendingAcks;

    nativegpu::VegetationDebugView debugView = nativegpu::VegetationDebugView::Shaded;
    float debugCells = 8.0f;
    float windScale = 1.0f;

    std::string capturePath;
    bool captureRequested = false;
    unsigned long long captureRequest = 0;
    unsigned long long captureRevision = 0;
    // Negative means "use the wall clock". See Arguments::time.
    float pinnedSeconds = -1.0f;
    // Seconds added per frame when the clock is pinned, so a sequence can be captured at a fixed
    // frame rate instead of at whatever rate the machine happens to render.
    float timeStep = 1.0f / 60.0f;
    // Where to write the per-frame coverage/luminance series. Empty means "no statistics".
    std::string frameStatsPath;
    // Window for those statistics, in pixels; a negative width means the whole frame. A window on
    // a low fork measures that fork's silhouette under wind, and a window on the crown is the
    // control that proves the measurement is sensitive at all.
    int statsX = 0, statsY = 0, statsWidth = -1, statsHeight = -1;
    struct FrameStat { double seconds; unsigned covered; double meanLuminance; };
    std::vector<FrameStat> frameStats;

    float cameraPosition[3] = {0, 8, -24};
    float shadowStrength = 0.7f;
    bool quit = false;
    bool ready = false;
    std::uint64_t frames = 0;
};

// Declared here because renderFrame reads the previous frame's timestamps; the definitions live
// with the rest of the timing code, after the queue helpers they depend on.
void readTimings(Host& host);
void writeTimingReport(Host& host);
void writeFrameStats(Host& host);

std::uint64_t signalAndWait(Host& host) {    const std::uint64_t value = ++host.fenceValue;
    check(host.queue->Signal(host.fence.p, value), "queue signal");
    std::fprintf(stderr, "signal %llu completed=%llu\n", static_cast<unsigned long long>(value),
                 static_cast<unsigned long long>(host.fence->GetCompletedValue()));
    if (host.fence->GetCompletedValue() < value) {
        check(host.fence->SetEventOnCompletion(value, host.fenceEvent), "fence event");
        WaitForSingleObject(host.fenceEvent, INFINITE);
    }
    return value;
}

void transition(Host& host, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    host.list->ResourceBarrier(1, &barrier);
}

void createDepth(Host& host) {
    host.depth.reset();
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CreationNodeMask = properties.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = host.width;
    description.Height = host.height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1.0f;
    check(host.device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(host.depth.out())), "create viewport depth");
    host.device->CreateDepthStencilView(host.depth.p, nullptr, host.dsvHeap.p->GetCPUDescriptorHandleForHeapStart());
}

void createRenderTargets(Host& host) {
    const auto rtv = host.rtvHeap.p->GetCPUDescriptorHandleForHeapStart();
    for (unsigned index = 0; index < host.bufferCount; ++index) {
        check(host.swapChain->GetBuffer(index, IID_PPV_ARGS(host.backBuffers[index].out())), "get back buffer");
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = kSwapChainFormat;
        view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv;
        handle.ptr += static_cast<SIZE_T>(index) * host.rtvStride;
        host.device->CreateRenderTargetView(host.backBuffers[index].p, &view, handle);
        host.presented[index] = false;
    }
    createDepth(host);
}

void resize(Host& host, unsigned width, unsigned height) {
    width = std::max(16u, width);
    height = std::max(16u, height);
    if (host.sceneOpen) {
        if (width == host.width && height == host.height) return;
        std::string error;
        if (!host::scenePreviewResize(width, height, error)) throw Failure(error);
        host.width = width;
        host.height = height;
        return;
    }
    if (width == host.width && height == host.height && host.swapChain) return;
    signalAndWait(host);
    for (auto& buffer : host.backBuffers) buffer.reset();
    host.depth.reset();
    if (host.swapChain) {
        check(host.swapChain->ResizeBuffers(host.bufferCount, width, height, kSwapChainFormat, 0),
              "resize swap chain");
    }
    host.width = width;
    host.height = height;
    createRenderTargets(host);
    if (host.draw) {
        // Visible-list capacity scales with the rectangle, never below the instance count:
        // an undersized list silently drops plants (see A6 in the defect analysis).
        (void)host.draw;
    }
}

void createDevice(Host& host, bool debug) {
    if (debug) {
        Com<ID3D12Debug> control;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(control.out())))) {
            control->EnableDebugLayer();
            std::fprintf(stderr, "debug layer enabled\n");
        }
    }
    check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(host.device.out())), "create device");
    D3D12_COMMAND_QUEUE_DESC queueDescription{};
    queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(host.device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(host.queue.out())), "create queue");
    check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(host.allocator.out())),
          "create allocator");
    check(host.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, host.allocator.p, nullptr,
          IID_PPV_ARGS(host.list.out())), "create command list");
    check(host.list->Close(), "close initial command list");
    check(host.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(host.fence.out())), "create fence");
    host.fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!host.fenceEvent) throw Failure("create fence event");
    std::fprintf(stderr, "fence created completed=%llu\n",
                 static_cast<unsigned long long>(host.fence->GetCompletedValue()));
    std::fprintf(stderr, "device removed reason=0x%08lx\n", static_cast<unsigned long>(host.device->GetDeviceRemovedReason()));

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeap{};
    rtvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeap.NumDescriptors = Host::kBuffers;
    check(host.device->CreateDescriptorHeap(&rtvHeap, IID_PPV_ARGS(host.rtvHeap.out())), "create RTV heap");
    host.rtvStride = host.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeap{};
    dsvHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeap.NumDescriptors = 1;
    check(host.device->CreateDescriptorHeap(&dsvHeap, IID_PPV_ARGS(host.dsvHeap.out())), "create DSV heap");
}

void createSwapChain(Host& host, bool vsync) {
    Com<IDXGIFactory2> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.out())), "create DXGI factory");
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = host.width;
    description.Height = host.height;
    description.Format = kSwapChainFormat;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = Host::kBuffers;
    description.Scaling = DXGI_SCALING_STRETCH;
    description.SwapEffect = kSwapEffect;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    Com<IDXGISwapChain1> swap;
    check(factory->CreateSwapChainForHwnd(host.queue.p, host.window, &description, nullptr, nullptr, swap.out()),
          "create swap chain");
    check(swap.p->QueryInterface(IID_PPV_ARGS(host.swapChain.out())), "query swap chain3");
    DXGI_SWAP_CHAIN_DESC1 actual{};
    check(host.swapChain->GetDesc1(&actual), "query swap chain desc");
    host.bufferCount = std::min(Host::kBuffers, std::max(1u, actual.BufferCount));
    // scRGB: the compositor reads linear values and encodes for the display. Without this
    // the half-float surface is interpreted as sRGB data and comes out washed out.
    UINT support = 0;
    if (SUCCEEDED(host.swapChain->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support))
        && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0) {
        check(host.swapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709), "set scRGB colour space");
    } else {
        throw Failure("swap chain does not accept a linear scRGB colour space");
    }
    if (!vsync) host.swapChain->SetMaximumFrameLatency(2);
    createRenderTargets(host);
    (void)vsync;
}

void dumpInfoQueue(Host& host, const char* point) {
    Com<ID3D12InfoQueue> queue;
    if (FAILED(host.device.p->QueryInterface(IID_PPV_ARGS(queue.out())))) return;
    const UINT64 count = queue->GetNumStoredMessages();
    for (UINT64 index = 0; index < count; ++index) {
        SIZE_T length = 0;
        if (FAILED(queue->GetMessage(index, nullptr, &length)) || length == 0) continue;
        std::vector<char> bytes(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if (FAILED(queue->GetMessage(index, message, &length))) continue;
        std::fprintf(stderr, "d3d12[%s] %d: %.*s\n", point, static_cast<int>(message->Severity),
                     static_cast<int>(message->DescriptionByteLength), message->pDescription);
    }
    queue->ClearStoredMessages();
}

void trace(Host& host, const char* point) {
    std::fprintf(stderr, "trace %-22s removed=0x%08lx\n", point,
                 static_cast<unsigned long>(host.device->GetDeviceRemovedReason()));
}

void beginList(Host& host) {
    check(host.allocator->Reset(), "reset allocator");
    check(host.list->Reset(host.allocator.p, nullptr), "reset command list");
}

void submitList(Host& host) {
    check(host.list->Close(), "close command list");
    ID3D12CommandList* lists[] = {host.list.p};
    host.queue->ExecuteCommandLists(1, lists);
    signalAndWait(host);
}

// `setTextures` and `upload` must not share one command list: `upload` clears the staging
// array at its start, which destroys the texture upload buffers the list still references.
// Submitting in two batches is the contract, not a style choice.
//
// The assets and the draw object are parameters rather than reads of `host`, so a reload can
// prepare a complete replacement (mesh + textures + draw object) while the current one keeps
// rendering. Only after every step has succeeded does the caller swap them in; a package that
// fails to parse or upload cannot leave a half-uploaded viewport behind.
void uploadAssets(Host& host, LoadedAssets& assets, nativegpu::VegetationDraw& draw) {
    std::string error;
    beginList(host);
    if (!draw.setTextures(host.list.p, assets.textures, error)) throw Failure(error);
    submitList(host);
    beginList(host);
    if (!draw.upload(host.list.p, assets.mesh, assets.instances, assets.page, error)) {
        throw Failure(error);
    }
    submitList(host);
}

void buildFrame(Host& host, nativegpu::VegetationFrameInput& frame) {
    const auto& camera = host.camera;
    const float cy = std::cos(camera.yaw), sy = std::sin(camera.yaw);
    const float cp = std::cos(camera.pitch), sp = std::sin(camera.pitch);
    const float offset[3] = {sy * cp * camera.distance, -sp * camera.distance, -cy * cp * camera.distance};
    host.cameraPosition[0] = camera.target[0] - offset[0];
    host.cameraPosition[1] = camera.target[1] - offset[1];
    host.cameraPosition[2] = camera.target[2] - offset[2];

    const float aspect = host.width > 0 && host.height > 0
        ? static_cast<float>(host.width) / static_cast<float>(host.height) : 1.0f;
    const float nearPlane = std::max(0.05f, camera.distance * 0.01f);
    const float farPlane = camera.distance * 12.0f + 400.0f;
    float projection[16], view[16];
    perspective(0.9f, aspect, nearPlane, farPlane, projection);
    const float up[3] = {0, 1, 0};
    lookAt(host.cameraPosition, camera.target, up, view);
    multiply(projection, view, frame.viewProjection.m);

    // Key-light shadow: an orthographic box fitted to the species, positioned by walking
    // back along the light direction. The same matrix is used by the caster and the
    // receiver pass, which is the contract VegetationDraw documents.
    const float light[3] = {0.42f, 0.78f, 0.46f};
    frame.lightDirection[0] = light[0];
    frame.lightDirection[1] = light[1];
    frame.lightDirection[2] = light[2];
    const float height = std::max(0.5f, host.assets.bounds[0]);
    const float radius = std::max(0.3f, host.assets.bounds[1]);
    const float extent = std::max(height, radius * 2.0f) * 1.35f;
    const float distance = extent * 2.4f;
    const float centre[3] = {0.0f, height * 0.45f, 0.0f};
    const float lightEye[3] = {centre[0] - light[0] * distance, centre[1] - light[1] * distance,
                               centre[2] - light[2] * distance};
    float lightView[16], lightProjection[16];
    lookAt(lightEye, centre, up, lightView);
    orthographic(extent * 2.0f, extent * 2.0f, distance * 0.35f, distance * 1.9f, lightProjection);
    multiply(lightProjection, lightView, frame.lightViewProjection.m);

    frame.camera[0] = host.cameraPosition[0];
    frame.camera[1] = host.cameraPosition[1];
    frame.camera[2] = host.cameraPosition[2];
    frame.viewportHeight = static_cast<float>(host.height);
    frame.tanHalfFov = std::tan(0.45f);
    frame.alphaCutoff = 0.25f;
    frame.maximumDistance = camera.distance * 6.0f + 80.0f;
    frame.windAmplitude = (0.05f + height * 0.012f) * host.windScale;
    frame.windFrequency = 0.55f;
    frame.windSpeed = host.windScale;
    frame.lightIntensity = 1.9f;
    frame.ambient = 0.62f;
    frame.shadowStrength = host.shadowStrength;
    frame.shadowDepthBias = 0.0015f;
    frame.shadowNormalBiasMetres = std::max(0.02f, height * 0.0035f);
    frame.debugView = static_cast<int>(host.debugView);
    frame.debugCells = host.debugCells;
}

void renderFrame(Host& host, bool capture, const std::string& screenshot) {
    if (host.width == 0 || host.height == 0) return;
    const std::uint64_t next = host.fenceValue + 1;
    if (host.fence->GetCompletedValue() < host.fenceValue) {
        check(host.fence->SetEventOnCompletion(host.fenceValue, host.fenceEvent), "fence event");
        WaitForSingleObject(host.fenceEvent, INFINITE);
    }
    // The previous frame is now complete, so its resolved timestamps are readable. This is the
    // only place GPU time is read, and it never blocks: the fence wait above is for the frame
    // that is already in flight, not for the one being measured.
    readTimings(host);
    // The previous frame has completed, so any chunk the runtime retired last frame is no
    // longer referenced by an in-flight command list and its buffers can be freed.
    if (host.world) host.world->releaseRetired();
    beginList(host);
    host.list->EndQuery(host.queryHeap.p, D3D12_QUERY_TYPE_TIMESTAMP, 0);

    nativegpu::VegetationFrameInput frame{};
    // A pinned clock makes wind captures reproducible; the shader's whole wind model is a function
    // of this value, so two runs at the same `--time` produce the same canopy. `--time-step`
    // advances it per frame, which is what turns a pinned clock into a fixed-rate sequence instead
    // of a clock that follows the machine's frame rate.
    frame.seconds = host.pinnedSeconds >= 0.0f
        ? host.pinnedSeconds + static_cast<float>(host.frames) * host.timeStep
        : static_cast<float>(GetTickCount64()) * 0.001f;
    buildFrame(host, frame);

    const float material[4] = {0.55f, 0.0004f, 0.35f, 1.0f};
    (void)material;
    std::string error;
    // Shadow pass first: it binds a depth-only output and deliberately does not restore the
    // caller's render targets.
    if (!host.worldOpen && !host.draw->drawShadow(host.list.p, next, frame, host.assets.material, host.assets.bounds, error)) {
        std::fprintf(stderr, "drawShadow rejected next=%llu completed=%llu frames=%llu: %s\n",
                     static_cast<unsigned long long>(next),
                     static_cast<unsigned long long>(host.fence->GetCompletedValue()),
                     static_cast<unsigned long long>(host.frames), error.c_str());
        throw Failure(error);
    }
    // Split point between the caster pass and the colour pass, so the report can say which one
    // costs what instead of lumping both into one number.
    host.list->EndQuery(host.queryHeap.p, D3D12_QUERY_TYPE_TIMESTAMP, 1);

    const unsigned index = host.swapChain->GetCurrentBackBufferIndex();
    transition(host, host.backBuffers[index].p,
               host.presented[index] ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = host.rtvHeap.p->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * host.rtvStride;
    const auto dsv = host.dsvHeap.p->GetCPUDescriptorHandleForHeapStart();
    host.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    // Linear clear colour: 0.022 is roughly #23262a once encoded for the display.
    const float clear[4] = {0.022f, 0.025f, 0.029f, 1.0f};
    host.list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    host.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(host.width), static_cast<float>(host.height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(host.width), static_cast<LONG>(host.height)};
    host.list->RSSetViewports(1, &viewport);
    host.list->RSSetScissorRects(1, &scissor);

    if (host.worldOpen && host.world) {
        nativegpu::WorldDrawLighting lighting;
        for (int i = 0; i < 3; ++i) {
            lighting.keyDirection[i] = frame.lightDirection[i];
            lighting.keyColor[i] = frame.lightColor[i];
            lighting.camera[i] = frame.camera[i];
            lighting.fogColor[i] = i == 0 ? 0.62f : (i == 1 ? 0.7f : 0.78f);
        }
        lighting.keyIntensity = frame.lightIntensity;
        lighting.ambient = frame.ambient;
        lighting.seconds = frame.seconds;
        // The camera is above the terrain looking at it; fog is thin so the far chunks fade
        // rather than disappear at the streaming boundary.
        lighting.fogDensity = 0.0004f;
        if (!host.world->frame(host.list.p, frame.viewProjection.m, lighting, error)) {
            std::fprintf(stderr, "world frame rejected: %s\n", error.c_str());
            throw Failure(error);
        }
    } else if (!host.draw->draw(host.list.p, next, frame, host.assets.material, host.assets.bounds, error)) {
        std::fprintf(stderr, "draw rejected next=%llu completed=%llu frames=%llu: %s\n",
                     static_cast<unsigned long long>(next),
                     static_cast<unsigned long long>(host.fence->GetCompletedValue()),
                     static_cast<unsigned long long>(host.frames), error.c_str());
        throw Failure(error);
    }
    // End of the colour pass. Anything after this (a capture copy, the present transition) is
    // measured into the frame total but not into gpuColourMs, so the report does not blame
    // shading for the cost of reading back a screenshot.
    host.list->EndQuery(host.queryHeap.p, D3D12_QUERY_TYPE_TIMESTAMP, 2);

    // Capture before Present. A bitblt swap chain leaves the back buffer undefined after
    // Present, so reading it afterwards returns whatever the compositor last blitted.
    Com<ID3D12Resource> readback;
    // Four half floats per pixel: R16G16B16A16_FLOAT.
    const UINT64 rowPitch = static_cast<UINT64>(host.width) * 8ull;
    if (capture) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = rowPitch * host.height;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(host.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.out())), "create readback");
        transition(host, host.backBuffers[index].p, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{}, to{};
        from.pResource = host.backBuffers[index].p;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback.p;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint.Format = kSwapChainFormat;
        to.PlacedFootprint.Footprint.Width = host.width;
        to.PlacedFootprint.Footprint.Height = host.height;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);
        host.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(host, host.backBuffers[index].p, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_PRESENT);
    } else {
        transition(host, host.backBuffers[index].p, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PRESENT);
    }
    // Timestamp the end of the whole command list, then resolve all four queries into this
    // frame's slot. Both calls must be recorded before the list is closed; the readback happens
    // on a later frame, after the fence that protects it.
    host.list->EndQuery(host.queryHeap.p, D3D12_QUERY_TYPE_TIMESTAMP, 3);
    host.list->ResolveQueryData(host.queryHeap.p, D3D12_QUERY_TYPE_TIMESTAMP, 0, 4,
                                host.queryReadback.p, static_cast<UINT64>(host.timingSlot) * 4ull * sizeof(std::uint64_t));
    check(host.list->Close(), "close frame list");
    ID3D12CommandList* lists[] = {host.list.p};
    host.queue->ExecuteCommandLists(1, lists);
    host.fenceValue = next;
    check(host.queue->Signal(host.fence.p, next), "signal frame fence");
    const auto presentStart = std::chrono::steady_clock::now();
    check(host.swapChain->Present(1, 0), "present");
    const auto presentEnd = std::chrono::steady_clock::now();
    host.presentMsSum += std::chrono::duration<double, std::milli>(presentEnd - presentStart).count();
    ++host.presentFrames;
    host.timingSlot = (host.timingSlot + 1) % Host::kTimingSlots;
    ++host.timingResolved;
    host.presented[index] = true;
    ++host.frames;

    if (capture) {
        if (host.fence->GetCompletedValue() < next) {
            check(host.fence->SetEventOnCompletion(next, host.fenceEvent), "fence event");
            WaitForSingleObject(host.fenceEvent, INFINITE);
        }
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(rowPitch * host.height)};
        check(readback->Map(0, &range, &mapped), "map readback");
        // FP16 linear -> 8-bit sRGB, with the sRGB transfer applied here rather than trusted
        // to the compositor.
        std::vector<unsigned char> rgba(static_cast<std::size_t>(host.width) * host.height * 4);
        const auto* sourceBytes = static_cast<const unsigned char*>(mapped);
        for (unsigned y = 0; y < host.height; ++y) {
            const auto* row = reinterpret_cast<const unsigned short*>(sourceBytes + static_cast<std::size_t>(y) * rowPitch);
            auto* target = rgba.data() + static_cast<std::size_t>(y) * host.width * 4;
            for (unsigned x = 0; x < host.width; ++x) {
                const unsigned short r = row[x * 4 + 0], g = row[x * 4 + 1], b = row[x * 4 + 2];
                target[x * 4 + 0] = encodeSrgb(halfToFloat(r));
                target[x * 4 + 1] = encodeSrgb(halfToFloat(g));
                target[x * 4 + 2] = encodeSrgb(halfToFloat(b));
                target[x * 4 + 3] = 255;
            }
        }
        const D3D12_RANGE written{0, 0};
        readback->Unmap(0, &written);
        // Per-frame statistics for the wind sequence: coverage and mean luminance of the frame,
        // computed from the same readback the screenshot uses. This is what makes "the canopy
        // moves without popping" measurable over hundreds of frames without decoding PNGs.
        if (!host.frameStatsPath.empty()) {
            unsigned covered = 0;
            double sum = 0.0;
            // "Covered" means "differs from the background", and the background is whatever the
            // clear colour became after the sRGB encode. It is read from the top-left pixel: the
            // clear is uniform and nothing is drawn at the corner, whereas a fixed threshold
            // either counted the whole frame (it did, on the first attempt) or none of it.
            const int backgroundR = rgba[0], backgroundG = rgba[1], backgroundB = rgba[2];
            // Optional window: a fork near the base barely moves under wind, so its silhouette must
            // stay put; the same statistic over the crown is the sensitivity control.
            const int windowX0 = std::max(0, host.statsX);
            const int windowY0 = std::max(0, host.statsY);
            const int windowX1 = host.statsWidth > 0
                ? std::min(static_cast<int>(host.width), windowX0 + host.statsWidth) : static_cast<int>(host.width);
            const int windowY1 = host.statsHeight > 0
                ? std::min(static_cast<int>(host.height), windowY0 + host.statsHeight) : static_cast<int>(host.height);
            for (int y = windowY0; y < windowY1; ++y) {
                for (int x = windowX0; x < windowX1; ++x) {
                    const std::size_t index = (static_cast<std::size_t>(y) * host.width + x) * 4;
                    const int r = rgba[index], g = rgba[index + 1], b = rgba[index + 2];
                    if (std::abs(r - backgroundR) <= 6 && std::abs(g - backgroundG) <= 6
                        && std::abs(b - backgroundB) <= 6) continue;
                    ++covered;
                    sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
                }
            }
            const double seconds = host.pinnedSeconds >= 0.0f
                ? host.pinnedSeconds + static_cast<float>(host.frames) * host.timeStep
                : 0.0;
            host.frameStats.push_back({ seconds, covered, covered ? sum / covered : 0.0 });
        } else {
            std::string pngError;
            if (!nativegpu::writePng(screenshot, host.width, host.height, rgba.data(),
                                     static_cast<std::size_t>(host.width) * 4, pngError)) {
                throw Failure(pngError);
            }
            std::printf("captured %s %ux%u\n", screenshot.c_str(), host.width, host.height);
            std::fflush(stdout);
        }
    }
}

// Every applied command answers with exactly one line. `want=` is the revision the editor asked
// for and `rev=` is the revision this process has actually drawn; when they differ the editor
// shows "stale" instead of pretending the viewport updated.
void sendAck(Host& host, const ControlState& request, const char* state, const std::string& error) {
    const auto sceneStats = host.sceneOpen ? host::scenePreviewStats() : host::ScenePreviewStats{};
    const std::string digest = host.sceneOpen
        ? (sceneStats.digest.empty() ? std::string("none") : sceneStats.digest.substr(0, 16))
        : (host.assets.digest.empty() ? std::string("none") : host.assets.digest.substr(0, 16));
    const auto frames = host.sceneOpen ? sceneStats.frames : host.frames;
    std::printf("ack=1;req=%llu;want=%llu;rev=%llu;state=%s;mode=%s;species=%s;lod=%u;digest=%s;tri=%u;debug=%s;wind=%.3f;frames=%llu;world=%llu;resident=%llu;layers=%u;play=%s;paused=%d;ortho=%d;camyaw=%.4f;campitch=%.4f;camdist=%.3f;overlay=%llu;wchunks=%llu;winst=%llu;wbuild=%llu;wtri=%llu;wmanifest=%s;err=%s\n",
                request.requestId, request.revision, host.appliedRevision, state, host.mode.c_str(),
                host.assets.speciesId.empty() ? "none" : host.assets.speciesId.c_str(),
                host.assets.lodLevel, digest.c_str(), host.assets.triangleCount,
                nativegpu::vegetationDebugViewName(host.debugView).c_str(),
                static_cast<double>(host.windScale),
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(sceneStats.worldInstances),
                static_cast<unsigned long long>(sceneStats.residentInstances), sceneStats.vegetationLayers,
                host.sceneOpen ? sceneStats.play.c_str() : "vegetation",
                host.sceneOpen ? (sceneStats.paused ? 1 : 0) : 0,
                host.sceneOpen ? (sceneStats.cameraOrthographic ? 1 : 0) : 0,
                static_cast<double>(sceneStats.cameraYaw),
                static_cast<double>(sceneStats.cameraPitch),
                static_cast<double>(sceneStats.cameraDistance),
                static_cast<unsigned long long>(sceneStats.overlayRevision),
                static_cast<unsigned long long>(host.worldOpen && host.world ? host.world->stats().chunksResident : 0),
                static_cast<unsigned long long>(host.worldOpen && host.world ? host.world->stats().instancesDrawn : 0),
                static_cast<unsigned long long>(host.worldOpen && host.world ? host.world->stats().buildingsResident : 0),
                static_cast<unsigned long long>(host.worldOpen && host.world ? host.world->stats().triangles : 0),
                host.worldOpen && host.world && host.world->stats().manifestHash.size() >= 16
                    ? host.world->stats().manifestHash.substr(0, 16).c_str() : "none",
                error.c_str());
    std::fflush(stdout);
}

// Loads a species/LOD and uploads it, then swaps it in. Everything fallible happens before the
// live resources are touched: a package that does not parse, a digest that does not match or an
// upload that fails leaves the previous content on screen and reports the failure.
void reloadContent(Host& host, const ControlState& request) {
    const std::string species = request.species.empty() ? host.assets.speciesId : request.species;
    const unsigned lod = request.lod >= 0 ? static_cast<unsigned>(request.lod) : host.assets.lodLevel;
    if (species.empty()) {
        sendAck(host, request, "error", "no species selected");
        return;
    }
    const bool speciesChanged = species != host.assets.speciesId;
    const float previousHeight = host.assets.bounds[0];
    try {
        LoadedAssets next = loadAssets(species, host.cookedDirectory, host.textureDirectory,
                                       host.instanceCount, lod, request.digest);
        const unsigned capacity = std::max(256u, host.instanceCount * 2 + 32);
        auto draw = std::make_unique<nativegpu::VegetationDraw>(
            host.device.p, host.fence.p, kSwapChainFormat, DXGI_FORMAT_D32_FLOAT, capacity);
        // Drain the queue first: the previous frame may still reference the heaps this object
        // is about to retire.
        signalAndWait(host);
        uploadAssets(host, next, *draw);
        if (host.draw) {
            std::string ignored;
            host.draw->shutdown(ignored);
        }
        if (speciesChanged && previousHeight > 0.05f && next.bounds[0] > 0.05f) {
            // Keep the orbit the user set up. Only the scale follows the new species, so
            // switching oak -> pine does not snap the camera back to a default view.
            host.camera.distance *= next.bounds[0] / previousHeight;
            host.camera.target[1] = next.bounds[0] * 0.46f;
        }
        host.assets = std::move(next);
        host.draw = std::move(draw);
        host.pendingRevision = request.revision;
        host.pendingRequest = request.requestId;
        host.pendingContent.push_back(request);
        host.appliedPending = true;
    } catch (const std::exception& error) {
        sendAck(host, request, "error", error.what());
    }
}

std::filesystem::path projectPath(const Host& host, const std::string& relative) {
    if (relative.empty()) throw Failure("project-relative path is required");
    const auto root = std::filesystem::weakly_canonical(host.projectRoot);
    const auto candidate = (root / std::filesystem::u8path(relative)).lexically_normal();
    const auto resolved = std::filesystem::weakly_canonical(candidate);
    const auto mismatch = std::mismatch(root.begin(), root.end(), resolved.begin(), resolved.end());
    if (mismatch.first != root.end()) throw Failure("viewport path escapes the project: " + relative);
    return candidate;
}

std::filesystem::path runtimePath(const Host& host, const std::string& value) {
    const auto requested = std::filesystem::u8path(value);
    if (!requested.is_absolute()) return projectPath(host, value);
    const auto resolved = std::filesystem::weakly_canonical(requested);
    if (!std::filesystem::is_directory(resolved)) throw Failure("runtime asset path is not a directory: " + value);
    return resolved;
}

void releaseVegetationSwapChain(Host& host) {
    if (host.swapChain) signalAndWait(host);
    for (auto& buffer : host.backBuffers) buffer.reset();
    host.depth.reset();
    host.swapChain.reset();
}

void openContent(Host& host, const ControlState& request) {
    if (request.mode == "vegetation") {
        if (host.sceneOpen) {
            host::scenePreviewClose();
            host.sceneOpen = false;
        }
        if (!host.swapChain) createSwapChain(host, true);
        host.mode = "vegetation";
        reloadContent(host, request);
        return;
    }
    if (!host.sceneOpen) releaseVegetationSwapChain(host);
    else host::scenePreviewClose();
    host.sceneOpen = false;
    try {
        host::ScenePreviewOptions options;
        options.mode = request.mode;
        options.projectRoot = host.projectRoot;
        options.scene = projectPath(host, request.scene);
        options.textures = projectPath(host, "assets/textures");
        const bool sourceEffect = request.library.rfind("assets/effects/", 0) == 0;
        options.effectLibrary = projectPath(host, request.library.empty() || sourceEffect ? "build/effects/library.json" : request.library);
        options.asset = request.asset;
        options.material = request.material;
        options.clip = request.clip;
        options.effect = request.effect;
        options.play = request.play.empty() ? "edit" : request.play;
        if (!request.asset.empty()) options.scene = projectPath(host, "assets/imported/" + request.asset + "/preview.scene.json");
        std::string error;
        if (!host::scenePreviewOpen(host.window, host.width, host.height, options, error)) {
            sendAck(host, request, "error", error);
            return;
        }
        host.sceneOpen = true;
        host.mode = request.mode;
        host.pendingRevision = request.revision;
        host.pendingRequest = request.requestId;
        host.pendingContent.push_back(request);
        host.appliedPending = true;
    } catch (const std::exception& error) {
        sendAck(host, request, "error", error.what());
    }
}

// ---------------------------------------------------------------- GPU timing
// A timestamp query is the only measurement of GPU work that is not a CPU wait in disguise. The
// numbers here are used for the performance report, so the readback is asynchronous: the frame
// that just completed writes its four timestamps into a slot, and the *next* frame reads it after
// the existing fence wait.
void createTimingQueries(Host& host) {
    D3D12_QUERY_HEAP_DESC description{};
    description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    description.Count = 4;
    check(host.device->CreateQueryHeap(&description, IID_PPV_ARGS(host.queryHeap.out())), "create timestamp query heap");
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = static_cast<UINT64>(Host::kTimingSlots) * 4ull * sizeof(std::uint64_t);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(host.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(host.queryReadback.out())), "create timestamp readback");
    check(host.queue->GetTimestampFrequency(&host.queryFrequency), "query timestamp frequency");
    if (host.queryFrequency == 0) host.queryFrequency = 1;
}

// Reads the slot written by the previous frame. Called only after the fence for that frame has
// signalled, which renderFrame guarantees.
void readTimings(Host& host) {
    if (!host.queryReadback || host.timingResolved == 0) return;
    const std::size_t slot = (host.timingSlot + Host::kTimingSlots - 1) % Host::kTimingSlots;
    void* mapped = nullptr;
    // `nullptr` means "the CPU may read the whole resource": the runtime uses it to scope the
    // cache invalidation. Declaring only the slot's byte range left the earlier slots stale in
    // the CPU cache, which read back as the same frame over and over.
    if (FAILED(host.queryReadback->Map(0, nullptr, &mapped))) return;
    // The mapped pointer is the start of the whole ring; this frame's four timestamps live at the
    // slot's offset. Reading index 0 every time is how the first frame's sample gets counted as
    // every frame.
    const auto* stamps = static_cast<const std::uint64_t*>(mapped) + slot * 4;
    const double perTick = 1000.0 / static_cast<double>(host.queryFrequency);
    const double shadow = static_cast<double>(stamps[1] - stamps[0]) * perTick;
    const double color = static_cast<double>(stamps[3] - stamps[1]) * perTick;
    const double total = static_cast<double>(stamps[3] - stamps[0]) * perTick;
    const D3D12_RANGE written{0, 0};
    host.queryReadback->Unmap(0, &written);
    // A resolved pair of identical timestamps means the queue did not execute between them; that
    // is a valid reading (an empty pass), not a reason to drop the sample.
    if (total >= 0.0 && total < 1000.0) {
        host.gpuTotalMs.push_back(total);
        host.gpuShadowMs.push_back(shadow);
        host.gpuColorMs.push_back(color);
    }
}

struct TimingStats {
    std::size_t samples = 0;
    double min = 0, median = 0, p95 = 0, max = 0, mean = 0;
};

TimingStats summarizeTiming(std::vector<double> values) {
    TimingStats stats;
    if (values.empty()) return stats;
    std::sort(values.begin(), values.end());
    stats.samples = values.size();
    stats.min = values.front();
    stats.max = values.back();
    stats.median = values[(values.size() - 1) / 2];
    stats.p95 = values[std::min(values.size() - 1, static_cast<std::size_t>(0.95 * (values.size() - 1)))];
    double sum = 0;
    for (double value : values) sum += value;
    stats.mean = sum / static_cast<double>(values.size());
    return stats;
}

void writeTimingReport(Host& host) {
    if (host.timingJsonPath.empty()) return;
    const auto total = summarizeTiming(host.gpuTotalMs);
    const auto shadow = summarizeTiming(host.gpuShadowMs);
    const auto color = summarizeTiming(host.gpuColorMs);
    std::FILE* file = std::fopen(host.timingJsonPath.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "[host] cannot write timing report %s\n", host.timingJsonPath.c_str());
        return;
    }
    // A zero total means the resolve never landed. Writing 0.000 ms would be read as "free
    // frame"; the honest report is null plus a note, so a consumer cannot mistake it for a
    // measurement.
    const bool measured = total.samples > 0 && total.max > 0.0;
    if (!measured) {
        std::fprintf(file,
            "{\n  \"schema\": \"nativegpu.vegetation-host-timing/1\",\n"
            "  \"species\": \"%s\",\n  \"lod\": %u,\n  \"size\": [%u, %u],\n"
            "  \"timestampFrequency\": %llu,\n  \"samples\": %zu,\n"
            "  \"gpuFrameMs\": null,\n  \"gpuShadowMs\": null,\n  \"gpuColourMs\": null,\n"
            "  \"presentCpuMs\": %.4f,\n  \"triangles\": %u,\n"
            "  \"unavailable\": \"timestamp queries resolved to zero; no GPU time is reported\"\n}\n",
            host.assets.speciesId.c_str(), host.assets.lodLevel, host.width, host.height,
            static_cast<unsigned long long>(host.queryFrequency), total.samples,
            host.presentFrames ? host.presentMsSum / static_cast<double>(host.presentFrames) : 0.0,
            host.assets.triangleCount);
        std::fclose(file);
        std::fprintf(stderr, "[host] timing report %s: no GPU time available\n", host.timingJsonPath.c_str());
        return;
    }
    std::fprintf(file,
        "{\n  \"schema\": \"nativegpu.vegetation-host-timing/1\",\n"
        "  \"species\": \"%s\",\n  \"lod\": %u,\n  \"size\": [%u, %u],\n"
        "  \"timestampFrequency\": %llu,\n  \"samples\": %zu,\n"
        "  \"gpuFrameMs\": {\"min\": %.4f, \"median\": %.4f, \"p95\": %.4f, \"max\": %.4f, \"mean\": %.4f},\n"
        "  \"gpuShadowMs\": {\"min\": %.4f, \"median\": %.4f, \"p95\": %.4f, \"max\": %.4f, \"mean\": %.4f},\n"
        "  \"gpuColourMs\": {\"min\": %.4f, \"median\": %.4f, \"p95\": %.4f, \"max\": %.4f, \"mean\": %.4f},\n"
        "  \"presentCpuMs\": %.4f,\n"
        "  \"triangles\": %u,\n"
        "  \"note\": \"gpuFrameMs is the command list start-to-end timestamp difference; presentCpuMs is "
        "the CPU time spent inside Present and is not GPU time. Shadow and colour are split at the "
        "timestamp between the caster pass and the colour pass.\"\n}\n",
        host.assets.speciesId.c_str(), host.assets.lodLevel, host.width, host.height,
        static_cast<unsigned long long>(host.queryFrequency), total.samples,
        total.min, total.median, total.p95, total.max, total.mean,
        shadow.min, shadow.median, shadow.p95, shadow.max, shadow.mean,
        color.min, color.median, color.p95, color.max, color.mean,
        host.presentFrames ? host.presentMsSum / static_cast<double>(host.presentFrames) : 0.0,
        host.assets.triangleCount);
    std::fclose(file);
    std::printf("timing gpuMedian=%.3f gpuP95=%.3f samples=%zu\n", total.median, total.p95, total.samples);
    std::fflush(stdout);
}

// Writes the per-frame coverage/luminance series. A wind sequence is only evidence if the numbers
// are finite and move smoothly: a torn junction or a NaN shows up as a spike or a gap here.
void writeFrameStats(Host& host) {
    if (host.frameStatsPath.empty()) return;
    std::FILE* file = std::fopen(host.frameStatsPath.c_str(), "wb");
    if (!file) {
        std::fprintf(stderr, "[host] cannot write frame stats %s\n", host.frameStatsPath.c_str());
        return;
    }
    std::fprintf(file,
        "{\n  \"schema\": \"nativegpu.vegetation-wind-sequence/1\",\n"
        "  \"species\": \"%s\",\n  \"lod\": %u,\n  \"size\": [%u, %u],\n"
        "  \"timeStepSeconds\": %.6f,\n  \"frames\": %zu,\n  \"samples\": [\n",
        host.assets.speciesId.c_str(), host.assets.lodLevel, host.width, host.height,
        static_cast<double>(host.timeStep), host.frameStats.size());
    for (std::size_t index = 0; index < host.frameStats.size(); ++index) {
        const auto& stat = host.frameStats[index];
        std::fprintf(file, "    {\"frame\": %zu, \"seconds\": %.6f, \"covered\": %u, \"meanLuminance\": %.6f}%s\n",
                     index, stat.seconds, stat.covered, stat.meanLuminance,
                     index + 1 == host.frameStats.size() ? "" : ",");
    }
    std::fprintf(file, "  ]\n}\n");
    std::fclose(file);
    std::printf("frame-stats frames=%zu\n", host.frameStats.size());
    std::fflush(stdout);
}

// Applies one command. Split out from the drain loop so that every line the editor wrote is
// applied in order: a single "latest state" slot silently dropped a request when two lines
// arrived inside one frame, and a dropped request is indistinguishable from a hung viewport.
void applyOne(Host& host, const ControlState& snapshot) {
    if (!snapshot.visible) {
        if (IsWindowVisible(host.window)) ShowWindow(host.window, SW_HIDE);
    } else if (snapshot.width > 0) {
        SetWindowPos(host.window, HWND_TOP, snapshot.x, snapshot.y, snapshot.width, snapshot.height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        const auto clientWidth = static_cast<unsigned>(std::max(0, snapshot.width));
        const auto clientHeight = static_cast<unsigned>(std::max(0, snapshot.height));
        if (clientWidth != host.width || clientHeight != host.height) resize(host, clientWidth, clientHeight);
    }

    // Settings first: they are cheap, they cannot fail, and a debug view must apply to whatever
    // content is already on screen even if the following reload fails.
    if (snapshot.wind >= 0.0f) host.windScale = std::min(2.0f, std::max(0.0f, snapshot.wind));
    if (snapshot.shadow >= 0.0f) host.shadowStrength = std::min(1.0f, std::max(0.0f, snapshot.shadow));
    if (snapshot.checkerCells > 0.0f) host.debugCells = std::min(64.0f, std::max(1.0f, snapshot.checkerCells));
    if (!snapshot.debug.empty()) {
        nativegpu::VegetationDebugView view{};
        if (nativegpu::parseVegetationDebugView(snapshot.debug, view)) host.debugView = view;
        else { sendAck(host, snapshot, "error", "unknown debug view '" + snapshot.debug + "'"); return; }
    }

    if (snapshot.command == "rect") return;
    if (snapshot.command == "status" || snapshot.command == "ping") { sendAck(host, snapshot, "ok", {}); return; }
    if (snapshot.command == "capture") {
        if (snapshot.path.empty()) { sendAck(host, snapshot, "error", "capture needs a path"); return; }
        host.capturePath = snapshot.path;
        host.captureRequested = true;
        host.captureRequest = snapshot.requestId;
        host.captureRevision = snapshot.revision;
        return;
    }
    if (snapshot.command == "open") {
        std::fprintf(stderr, "[host] open mode=%s scene=%s material=%s clip=%s effect=%s library=%s\n",
                     snapshot.mode.c_str(), snapshot.scene.c_str(), snapshot.material.c_str(),
                     snapshot.clip.c_str(), snapshot.effect.c_str(), snapshot.library.c_str());
        openContent(host, snapshot);
        if (!snapshot.debug.empty()) {
            std::string debugError;
            if (!host::scenePreviewSetLayerDebug(snapshot.debug, debugError)) {
                sendAck(host, snapshot, "error", debugError);
                return;
            }
        }
        return;
    }
    if (snapshot.command == "world") {
        // Opening a world is a resource operation, not a scene edit: it loads a manifest,
        // validates it and lets the runtime stream from the camera. Nothing is written.
        if (snapshot.cooked.empty()) { sendAck(host, snapshot, "error", "world needs a cooked directory"); return; }
        try {
            const std::filesystem::path directory = host.projectRoot / std::filesystem::u8path(snapshot.cooked);
            const std::string canonical = std::filesystem::weakly_canonical(directory).string();
            const std::string root = std::filesystem::weakly_canonical(host.projectRoot).string();
            if (canonical.rfind(root, 0) != 0) {
                sendAck(host, snapshot, "error", "cooked world directory escapes the project");
                return;
            }
            auto bridge = std::make_unique<host::WorldBridge>();
            std::string error;
            if (!bridge->open(host.device.p, canonical, kSwapChainFormat, DXGI_FORMAT_D32_FLOAT, error)) {
                sendAck(host, snapshot, "error", error);
                return;
            }
            host.world = std::move(bridge);
            host.worldOpen = true;
            host.worldDirectory = snapshot.cooked;
            host.mode = "world";
            host.pendingRevision = snapshot.revision;
            host.pendingRequest = snapshot.requestId;
            host.pendingContent.clear();
            host.appliedPending = true;
            sendAck(host, snapshot, "ok", {});
        } catch (const std::exception& error) {
            sendAck(host, snapshot, "error", error.what());
        }
        return;
    }
    if (snapshot.command == "worldclose") {
        host.world.reset();
        host.worldOpen = false;
        host.worldDirectory.clear();
        host.mode = "vegetation";
        sendAck(host, snapshot, "ok", {});
        return;
    }
    if (snapshot.command == "play") {
        if (!host.sceneOpen) { sendAck(host, snapshot, "error", "play requires an open scene"); return; }
        std::string error;
        const std::string play = snapshot.play.empty() ? "edit" : snapshot.play;
        if (!host::scenePreviewSetPlay(play, snapshot.paused == 1, error)) {
            sendAck(host, snapshot, "error", error);
            return;
        }
        if (snapshot.step && !host::scenePreviewStep(1.0f / 60.0f, error)) {
            sendAck(host, snapshot, "error", error);
            return;
        }
        host.pendingAcks.push_back(snapshot);
        return;
    }
    if (snapshot.command == "camera") {
        if (!host.sceneOpen) { sendAck(host, snapshot, "error", "camera requires an open scene"); return; }
        std::string error;
        if (!snapshot.view.empty() && !host::scenePreviewSetView(snapshot.view, error)) {
            sendAck(host, snapshot, "error", error);
            return;
        }
        if (snapshot.focus && !host::scenePreviewFocus(snapshot.entity, error)) {
            sendAck(host, snapshot, "error", error);
            return;
        }
        host.pendingAcks.push_back(snapshot);
        return;
    }
    if (snapshot.command == "preview") {
        if (!host.sceneOpen) { sendAck(host, snapshot, "error", "preview requires an open scene"); return; }
        const float position[3] = {snapshot.px, snapshot.py, snapshot.pz};
        const float scale[3] = {snapshot.sx, snapshot.sy, snapshot.sz};
        std::string error;
        if (!host::scenePreviewSetEntityOverride(snapshot.entity, position, scale,
                                                 snapshot.active != 0, error)) {
            sendAck(host, snapshot, "error", error);
            return;
        }
        host.pendingAcks.push_back(snapshot);
        return;
    }
    if (snapshot.command == "previewclear") {
        if (!host.sceneOpen) { sendAck(host, snapshot, "error", "previewclear requires an open scene"); return; }
        host::scenePreviewClearOverrides();
        host.pendingAcks.push_back(snapshot);
        return;
    }
    if (snapshot.command != "load" && snapshot.command != "view") {
        sendAck(host, snapshot, "error", "unknown command '" + snapshot.command + "'");
        return;
    }
    std::fprintf(stderr, "[host] command=%s req=%llu species=%s lod=%d digest=%.16s\n",
                 snapshot.command.c_str(), snapshot.requestId, snapshot.species.c_str(),
                 snapshot.lod, snapshot.digest.c_str());
    std::fflush(stderr);
    // A LOD change is a different mesh, so it takes the reload path; a pure view change is
    // acknowledged after the next submitted frame alongside it.
    const bool lodChanged = snapshot.lod >= 0 && static_cast<unsigned>(snapshot.lod) != host.assets.lodLevel;
    if (snapshot.command == "load" || lodChanged) reloadContent(host, snapshot);
    else host.pendingAcks.push_back(snapshot);
}

// Right-button fly: W/A/S/D move in the camera plane and Q/E move vertically. The
// keys are polled every frame rather than stored from WM_KEYDOWN so a dropped key-up
// cannot leave the camera drifting after focus moved elsewhere.
void applySceneFly(Host& host) {
    if (!host.sceneOpen || !host.navRightDown) return;
    const auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    const float forward = (down('W') ? 1.0f : 0.0f) - (down('S') ? 1.0f : 0.0f);
    const float right = (down('D') ? 1.0f : 0.0f) - (down('A') ? 1.0f : 0.0f);
    const float up = (down('E') ? 1.0f : 0.0f) - (down('Q') ? 1.0f : 0.0f);
    if (forward == 0.0f && right == 0.0f && up == 0.0f) return;
    const bool fast = down(VK_SHIFT);
    host::scenePreviewCameraMove(forward, right, up, 1.0f / 60.0f, fast ? 30.0f : 9.0f);
}

// Drains every queued command. The queue exists so that two commands written inside one frame
// are both applied and both acknowledged, in arrival order, instead of the second silently
// replacing the first.
void applyControl(Host& host) {    std::vector<ControlState> commands;
    {
        std::lock_guard<std::mutex> guard(host.control.mutex);
        if (host.control.queue.empty()) return;
        commands.swap(host.control.queue);
    }
    for (const auto& command : commands) applyOne(host, command);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Host* host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(window, message, wParam, lParam);
        }
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_LBUTTONDOWN: {
            if (!host) break;
            SetFocus(window);
            const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            if (host->sceneOpen) {
                // Plain left click is reserved for selection. Orbit needs Alt so that a
                // click on an object can never be mistaken for a camera drag.
                if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
                    host->nav = NavMode::Orbit;
                    host->navLast = {x, y};
                    SetCapture(window);
                }
                return 0;
            }
            host->camera.dragging = true;
            host->camera.last.x = x;
            host->camera.last.y = y;
            SetCapture(window);
            return 0;
        }
        case WM_MBUTTONDOWN: {
            if (!host || !host->sceneOpen) break;
            SetFocus(window);
            host->nav = NavMode::Pan;
            host->navLast = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            SetCapture(window);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            if (!host || !host->sceneOpen) break;
            SetFocus(window);
            host->nav = NavMode::Look;
            host->navRightDown = true;
            host->navLast = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            SetCapture(window);
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
        case WM_RBUTTONUP:
            if (host) {
                if (message == WM_RBUTTONUP) host->navRightDown = false;
                if (host->sceneOpen) host->nav = NavMode::None;
                else host->camera.dragging = false;
            }
            ReleaseCapture();
            return 0;
        case WM_MOUSEMOVE: {
            if (!host) break;
            const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            if (host->sceneOpen) {
                if (host->nav == NavMode::None) break;
                const float dx = static_cast<float>(x - host->navLast.x);
                const float dy = static_cast<float>(y - host->navLast.y);
                if (host->nav == NavMode::Pan) host::scenePreviewCameraPan(dx, dy);
                else host::scenePreviewCameraOrbit(dx, dy);
                host->navLast = {x, y};
                return 0;
            }
            if (!host->camera.dragging) break;
            host->camera.yaw -= static_cast<float>(x - host->camera.last.x) * 0.008f;
            host->camera.pitch = std::min(1.35f, std::max(-0.35f,
                host->camera.pitch - static_cast<float>(y - host->camera.last.y) * 0.008f));
            host->camera.last.x = x;
            host->camera.last.y = y;
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (!host) break;
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (host->sceneOpen) {
                host::scenePreviewCameraDolly(static_cast<float>(delta) / 120.0f);
                return 0;
            }
            host->camera.distance = std::min(400.0f, std::max(1.0f,
                host->camera.distance * std::pow(1.12f, -static_cast<float>(delta) / 120.0f)));
            return 0;
        }
        case WM_KEYDOWN: {
            if (!host) break;
            if (host->sceneOpen) {
                if (wParam == VK_F1) break;
                // F focuses the selection; until selection exists it frames the scene.
                if (wParam == 'F' || wParam == VK_HOME) {
                    std::string error;
                    if (!host::scenePreviewFocus("", error)) std::fprintf(stderr, "[viewport] focus: %s\n", error.c_str());
                }
                if (wParam == VK_ESCAPE) {
                    host->nav = NavMode::None;
                    host->navRightDown = false;
                    ReleaseCapture();
                }
                return 0;
            }
            if (wParam == VK_ESCAPE && !host->parent) host->quit = true;
            return 0;
        }
        case WM_KILLFOCUS:
        case WM_CAPTURECHANGED:
            if (host) {
                // Losing focus or capture must not leave the camera in a drag that keeps
                // moving after the user has switched windows.
                host->nav = NavMode::None;
                host->navRightDown = false;
                host->camera.dragging = false;
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_TIMER:
            // The editor's Chromium child windows are siblings: a repaint on their side can
            // put them in front of this window. Re-asserting HWND_TOP on a slow timer keeps
            // the viewport visible without stealing activation.
            if (host) SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        case WM_DESTROY:
            if (host) host->quit = true;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

struct Arguments {
    unsigned long long parent = 0;
    std::string pipe;
    std::string project;
    std::string species = "oak_broadleaf";
    std::string cooked = "build/vegetation/cooked";
    std::string textures = "build/vegetation/textures";
    std::string screenshot;
    // Standalone world mode: a project-relative cooked world directory. Empty keeps the
    // vegetation preview, which is what the editor's default viewport opens.
    std::string world;
    // Optional expected package digest. When present the host refuses to draw bytes that do not
    // match, which is what stops a stale package from being shown under a new revision number.
    std::string digest;
    // Initial debug view for standalone captures (the editor sends the same name over the pipe).
    std::string view = "shaded";
    // Where to write the GPU timing report at exit. Empty means "do not measure".
    std::string timingJson;
    // Pin the wind clock, in seconds. Negative keeps the live clock. A frozen clock is what makes
    // two captures comparable: the canopy moves on `seconds`, so an unpinned screenshot differs
    // between runs for a reason that has nothing to do with the code.
    float time = -1.0f;
    float timeStep = 1.0f / 60.0f;
    // Per-frame coverage/luminance series destination (wind continuity evidence).
    std::string frameStats;
    // Optional "x,y,w,h" window for those statistics; a negative width means the whole frame.
    int statsX = 0, statsY = 0, statsWidth = -1, statsHeight = -1;
    unsigned instances = 1;
    unsigned lod = 0;
    unsigned frames = 0;
    int x = 40, y = 40;
    unsigned width = 960, height = 540;
    bool vsync = true;
    bool debug = false;
    float shadow = 0.7f;
    // Fixed-camera overrides for evidence captures. Zero means "frame the species".
    float distance = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float targetY = -1.0f;
};

Arguments parseArguments(int argc, char** argv) {
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string key = argv[index];
        const auto value = [&]() -> std::string {
            if (index + 1 >= argc) throw Failure("missing value for " + key);
            return argv[++index];
        };
        if (key == "--parent") arguments.parent = std::strtoull(value().c_str(), nullptr, 0);
        else if (key == "--pipe") arguments.pipe = value();
        else if (key == "--project") arguments.project = value();
        else if (key == "--species") arguments.species = value();
        else if (key == "--cooked") arguments.cooked = value();
        else if (key == "--textures") arguments.textures = value();
        else if (key == "--screenshot") arguments.screenshot = value();
        else if (key == "--world") arguments.world = value();
        else if (key == "--digest") arguments.digest = value();
        else if (key == "--lod") arguments.lod = static_cast<unsigned>(std::atoi(value().c_str()));
        else if (key == "--instances") arguments.instances = static_cast<unsigned>(std::atoi(value().c_str()));
        else if (key == "--frames") arguments.frames = static_cast<unsigned>(std::atoi(value().c_str()));
        else if (key == "--x") arguments.x = std::atoi(value().c_str());
        else if (key == "--y") arguments.y = std::atoi(value().c_str());
        else if (key == "--width") arguments.width = static_cast<unsigned>(std::atoi(value().c_str()));
        else if (key == "--height") arguments.height = static_cast<unsigned>(std::atoi(value().c_str()));
        else if (key == "--novsync") arguments.vsync = false;
        else if (key == "--debug") arguments.debug = true;
        else if (key == "--view") arguments.view = value();
        else if (key == "--timing-json") arguments.timingJson = value();
        else if (key == "--time") arguments.time = static_cast<float>(std::atof(value().c_str()));
        else if (key == "--time-step") arguments.timeStep = static_cast<float>(std::atof(value().c_str()));
        else if (key == "--frame-stats") arguments.frameStats = value();
        else if (key == "--stats-window") {
            // "x,y,w,h"; commas are turned into spaces so one istringstream reads all four.
            std::string window = value();
            for (char& character : window) if (character == ',') character = ' ';
            std::istringstream stream(window);
            stream >> arguments.statsX >> arguments.statsY >> arguments.statsWidth >> arguments.statsHeight;
        }
        else if (key == "--shadow") arguments.shadow = static_cast<float>(std::atof(value().c_str()));
        else if (key == "--distance") arguments.distance = static_cast<float>(std::atof(value().c_str()));
        else if (key == "--yaw") arguments.yaw = static_cast<float>(std::atof(value().c_str()));
        else if (key == "--pitch") arguments.pitch = static_cast<float>(std::atof(value().c_str()));
        else if (key == "--target-y") arguments.targetY = static_cast<float>(std::atof(value().c_str()));
        else throw Failure("unknown argument " + key);
    }
    return arguments;
}

int run(Host& host, const Arguments& arguments) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw Failure("register window class");
    }

    host.parent = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(arguments.parent));
    const DWORD style = host.parent
        ? (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN)
        : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    host.window = CreateWindowExW(host.parent ? WS_EX_NOPARENTNOTIFY : 0, kWindowClass, L"NativeGPU viewport",
                                  style, arguments.x, arguments.y,
                                  static_cast<int>(arguments.width), static_cast<int>(arguments.height),
                                  host.parent, nullptr, windowClass.hInstance, &host);
    if (!host.window) throw Failure("create viewport window");
    if (host.parent) {
        // Without WS_CLIPCHILDREN on the parent, Chromium paints over the child; without
        // HWND_TOP the child is behind the render widget.
        const LONG_PTR parentStyle = GetWindowLongPtrW(host.parent, GWL_STYLE);
        SetWindowLongPtrW(host.parent, GWL_STYLE, parentStyle | WS_CLIPCHILDREN);
        SetWindowPos(host.window, HWND_TOP, arguments.x, arguments.y,
                     static_cast<int>(arguments.width), static_cast<int>(arguments.height),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetTimer(host.window, kRaiseTimerId, 500, nullptr);
    }
    host.width = std::max(16u, arguments.width);
    host.height = std::max(16u, arguments.height);
    host.shadowStrength = std::min(1.0f, std::max(0.0f, arguments.shadow));
    if (!nativegpu::parseVegetationDebugView(arguments.view, host.debugView)) {
        throw Failure("unknown --view '" + arguments.view + "'");
    }

    createDevice(host, arguments.debug);
    trace(host, "device+queue");
    createSwapChain(host, arguments.vsync);
    trace(host, "swap chain");
    host.timingJsonPath = arguments.timingJson;
    host.pinnedSeconds = arguments.time;
    host.timeStep = arguments.timeStep > 0.0f ? arguments.timeStep : 1.0f / 60.0f;
    host.frameStatsPath = arguments.frameStats;
    host.statsX = arguments.statsX;
    host.statsY = arguments.statsY;
    host.statsWidth = arguments.statsWidth;
    host.statsHeight = arguments.statsHeight;
    host.projectRoot = std::filesystem::absolute(arguments.project.empty()
        ? std::filesystem::current_path() : std::filesystem::u8path(arguments.project));
    host.cookedDirectory = runtimePath(host, arguments.cooked).u8string();
    host.textureDirectory = runtimePath(host, arguments.textures).u8string();
    host.instanceCount = std::max(1u, arguments.instances);
    // World mode does not need a vegetation package. Requiring one would make a published
    // world depend on an unrelated asset library, which is exactly the coupling the world
    // path exists to avoid.
    const bool worldOnly = !arguments.world.empty();
    if (worldOnly) {
        host.assets.digest = "world";
        host.assets.speciesId = "world";
        host.assets.bounds[0] = 24.0f;
        host.assets.bounds[1] = 24.0f;
        host.assets.bounds[2] = 24.0f;
    } else {
        host.assets = loadAssets(arguments.species, host.cookedDirectory, host.textureDirectory,
                                 host.instanceCount, arguments.lod, arguments.digest);
    }

    // Visible list capacity: instances plus slack for the shadow pass reusing the list.
    const unsigned capacity = std::max(256u, arguments.instances * 2 + 32);
    host.draw = std::make_unique<nativegpu::VegetationDraw>(
        host.device.p, host.fence.p, kSwapChainFormat, DXGI_FORMAT_D32_FLOAT, capacity);
    trace(host, "vegetation draw ctor");
    createTimingQueries(host);
    if (!worldOnly) uploadAssets(host, host.assets, *host.draw);

    // Frame the species: distance from height, target at 45% height, slight three-quarter view.
    const float height = std::max(0.3f, host.assets.bounds[0]);
    host.camera.target[0] = 0.0f;
    host.camera.target[1] = height * 0.46f;
    host.camera.target[2] = 0.0f;
    host.camera.distance = height * 1.55f;
    host.camera.pitch = -0.12f;
    // Evidence captures pin the camera so two builds are comparable pixel for pixel.
    if (arguments.distance > 0.0f) host.camera.distance = arguments.distance;
    if (arguments.yaw != 0.0f) host.camera.yaw = arguments.yaw;
    if (arguments.pitch != 0.0f) host.camera.pitch = arguments.pitch;
    if (arguments.targetY >= 0.0f) host.camera.target[1] = arguments.targetY;

    // Standalone world mode. The editor reaches the same state over the pipe with
    // `cmd=world`; this path exists so a build machine can capture the world without an
    // Electron window.
    if (!arguments.world.empty()) {
        const std::filesystem::path directory = runtimePath(host, arguments.world);
        std::string worldError;
        auto bridge = std::make_unique<host::WorldBridge>();
        if (!bridge->open(host.device.p, directory, kSwapChainFormat, DXGI_FORMAT_D32_FLOAT, worldError)) {
            throw Failure(worldError);
        }
        host.world = std::move(bridge);
        host.worldOpen = true;
        host.worldDirectory = arguments.world;
        host.mode = "world";
        // Frame the world from above and to the south so a capture shows terrain, the river
        // and the settlement in one image.
        const auto stats = host.world->stats();
        host.camera.target[0] = 0.0f;
        host.camera.target[1] = 12.0f;
        host.camera.target[2] = 0.0f;
        host.camera.distance = arguments.distance > 0.0f ? arguments.distance : 190.0f;
        // Positive pitch raises the camera above the target: buildFrame subtracts
        // -sin(pitch)*distance from the target height, so a negative pitch puts the eye
        // below the terrain and the capture shows the inside of a hill.
        host.camera.pitch = arguments.pitch != 0.0f ? arguments.pitch : 0.62f;
        host.camera.yaw = arguments.yaw != 0.0f ? arguments.yaw : 0.35f;
        (void)stats;
        std::printf("world-open id=%s manifest=%s\n", host.world->stats().worldId.c_str(),
                    host.world->stats().manifestHash.substr(0, 16).c_str());
        std::fflush(stdout);
    }

    if (!arguments.pipe.empty()) {
        HANDLE pipe = connectPipe(arguments.pipe);
        std::printf("pipe-connected\n");
        std::printf("hwnd=%llu\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(host.window)));
        std::fflush(stdout);
        std::thread(controlLoop, pipe, std::ref(host.control)).detach();
        // A detached thread is acceptable here: the process exits with the window, and the
        // read blocks are released by CloseHandle at teardown.
    }
    std::printf("ready species=%s tri=%zu instances=%u size=%ux%u\n", arguments.species.c_str(),
                host.assets.mesh.indices.size() / 3, arguments.instances, host.width, host.height);
    // The initial content is already on the GPU, so it is applied revision 0 with no request.
    ControlState initial;
    initial.command = "load";
    initial.requestId = 0;
    initial.revision = 0;
    host.appliedRevision = 0;
    sendAck(host, initial, "ok", {});
    std::fflush(stdout);

    const bool capture = !arguments.screenshot.empty();
    std::string screenshot = arguments.screenshot;
    unsigned capturedFrames = 0;
    while (!host.quit) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { host.quit = true; break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (host.quit) break;
        applyControl(host);
        // A hidden window still applies commands and still owns uploaded GPU resources; only
        // presentation is impossible. Skipping the ack here too would make "load a species while
        // the viewport is collapsed" hang forever waiting for an acknowledgement that the content
        // is in place, which is what the editor's revision display depends on.
        const bool visible = IsWindowVisible(host.window) != FALSE;
        if (!visible) {
            if (host.appliedPending && !host.sceneOpen) {
                host.appliedRevision = host.pendingRevision;
                host.appliedRequest = host.pendingRequest;
                host.appliedPending = false;
                for (const auto& request : host.pendingContent) sendAck(host, request, "ok", {});
                host.pendingContent.clear();
            }
            for (const auto& request : host.pendingAcks) sendAck(host, request, "ok", {});
            host.pendingAcks.clear();
            Sleep(16);
            continue;
        }
        if (!host.sceneOpen && !host.swapChain) {
            Sleep(16);
            continue;
        }
        // A capture request from the editor replaces the command-line screenshot path for this
        // frame. The ack for it is emitted after the PNG is on disk, below.
        const bool editorCapture = host.captureRequested;
        if (editorCapture) screenshot = host.capturePath;
        const bool shoot = (capture && (arguments.frames == 0 || host.frames + 1 >= arguments.frames))
            || editorCapture
            // Frame statistics need every frame, not only the last one.
            || !host.frameStatsPath.empty();
        if (host.sceneOpen) {
            applySceneFly(host);
            std::string error;
            const bool ok = editorCapture ? host::scenePreviewCapture(screenshot, error)
                                          : host::scenePreviewFrame(1.0f / 60.0f, error);
            if (!ok) throw Failure(error);
            ++host.frames;
            if (shoot) ++capturedFrames;
        } else {
            renderFrame(host, shoot, screenshot);
            if (shoot) ++capturedFrames;
        }
        if (editorCapture) {
            ControlState request;
            request.command = "capture";
            request.requestId = host.captureRequest;
            request.revision = host.captureRevision;
            host.captureRequested = false;
            sendAck(host, request, "ok", {});
        }
        // A reload only becomes "applied" once a frame with it has been submitted, so the editor
        // cannot report a revision the user has not seen. Every queued content request is
        // answered exactly once, and all of them are told the revision that is now on screen.
        if (host.appliedPending) {
            host.appliedRevision = host.pendingRevision;
            host.appliedRequest = host.pendingRequest;
            host.appliedPending = false;
            for (const auto& request : host.pendingAcks) sendAck(host, request, "ok", {});
            host.pendingAcks.clear();
            for (const auto& request : host.pendingContent) sendAck(host, request, "ok", {});
            host.pendingContent.clear();
        } else if (!host.pendingAcks.empty()) {
            for (const auto& request : host.pendingAcks) sendAck(host, request, "ok", {});
            host.pendingAcks.clear();
        }
        if (capture && shoot) break;
        if (arguments.frames && host.frames >= arguments.frames) break;
    }
    if (host.sceneOpen) host::scenePreviewClose();
    if (host.draw) {
        std::string error;
        host.draw->shutdown(error);
    }
    if (arguments.debug) dumpInfoQueue(host, "exit");
    writeTimingReport(host);
    writeFrameStats(host);
    if (host.fenceEvent) CloseHandle(host.fenceEvent);
    std::printf("exit frames=%llu captured=%u\n", static_cast<unsigned long long>(host.frames), capturedFrames);
    if (host.worldOpen && host.world) {
        const auto world = host.world->stats();
        std::printf("world stats: chunks=%llu cpuReady=%llu drawn=%llu instances=%llu buildings=%llu tris=%llu bytes=%llu errors=%llu\n",
                    static_cast<unsigned long long>(world.chunksResident),
                    static_cast<unsigned long long>(world.chunksCpuReady),
                    static_cast<unsigned long long>(world.chunksDrawn),
                    static_cast<unsigned long long>(world.instancesDrawn),
                    static_cast<unsigned long long>(world.buildingsResident),
                    static_cast<unsigned long long>(world.triangles),
                    static_cast<unsigned long long>(world.bytesResident),
                    static_cast<unsigned long long>(world.loadErrors));
        if (!world.lastError.empty()) std::printf("world error: %s\n", world.lastError.c_str());
        std::fflush(stdout);
    }
    std::fflush(stdout);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Host host;
    try {
        const Arguments arguments = parseArguments(argc, argv);
        return run(host, arguments);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "viewport host failed: %s\n", error.what());
        std::fflush(stderr);
        return 1;
    }
}
