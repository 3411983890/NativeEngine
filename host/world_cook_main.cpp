// World cook worker.
//
// A separate process for the same reason the viewport host is one: the compiler is a large
// chunk of C++ that must be able to run to completion (or crash) without taking the editor
// with it, and it must be runnable from a build machine with no Electron and no GPU.
//
// It is deliberately dumb. It reads one resolved compile request, writes chunk files and a
// partial manifest, and prints a JSON summary. It does not follow library references, it
// does not read layer files, and it does not decide where anything goes on disk beyond the
// directory it was told to use. The JS service owns all of that, which is why the path
// policy lives in exactly one language.
//
// Usage:
//   world_cook.exe --input <request.json> --out <dir> [--input-digest <64 hex>]
//                  [--max-chunks <n>] [--quiet]
//
// Output on stdout: one JSON object, `{"ok":true,...}` or `{"ok":false,"error":"..."}`.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../engine/native/assets/sha256.hpp"
#include "../engine/native/world/world_compiler.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using engine::world::ChunkData;
using engine::world::ChunkFileInfo;
using engine::world::ChunkSink;
using engine::world::CompileInput;
using engine::world::CompileStats;
using engine::world::WorldCompiler;

std::string readTextFile(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

bool writeFile(const fs::path& path, const void* data, std::size_t size) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(stream);
}

// Writes each chunk as it is produced. Holding 4,096 chunks in memory would defeat the
// residency bound the runtime is built to respect.
class DirectorySink : public ChunkSink {
public:
    DirectorySink(fs::path root, std::size_t maxChunks)
        : root_(std::move(root)), maxChunks_(maxChunks) {}

    bool write(const ChunkData& chunk, ChunkFileInfo& info, std::string& error) override {
        if (infos_.size() >= maxChunks_) {
            error = "the cook produced more chunks than the request allowed";
            return false;
        }
        std::vector<std::uint8_t> bytes;
        if (!engine::world::serializeChunk(chunk, bytes, error)) return false;

        char name[64];
        std::snprintf(name, sizeof(name), "%d_%d.ngrc", chunk.coord.x, chunk.coord.z);
        const fs::path path = root_ / "chunks" / name;
        std::error_code code;
        fs::create_directories(path.parent_path(), code);
        if (!writeFile(path, bytes.data(), bytes.size())) {
            error = "cannot write " + path.string();
            return false;
        }

        info.cx = chunk.coord.x;
        info.cz = chunk.coord.z;
        info.file = std::string("chunks/") + name;
        info.sha256 = engine::assets::sha256(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        info.bytes = bytes.size();
        info.lod = chunk.lod;
        info.instanceCount = static_cast<std::uint32_t>(chunk.instances.size());
        info.vertexCount = chunk.vertexCount;
        info.bounds = chunk.bounds;
        info.minHeight = chunk.minHeight;
        info.maxHeight = chunk.maxHeight;
        infos_.push_back(info);
        totalBytes_ += bytes.size();
        return true;
    }

    const std::vector<ChunkFileInfo>& infos() const { return infos_; }
    std::uint64_t totalBytes() const { return totalBytes_; }

private:
    fs::path root_;
    std::size_t maxChunks_ = 0;
    std::vector<ChunkFileInfo> infos_;
    std::uint64_t totalBytes_ = 0;
};

std::string escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buffer;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void fail(const std::string& message) {
    std::cout << "{\"ok\":false,\"error\":\"" << escape(message) << "\"}\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string inputPath;
    std::string outputDir;
    std::string inputDigest;
    std::size_t maxChunks = 262144;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                fail(std::string("missing value for ") + name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (argument == "--input") inputPath = next("--input");
        else if (argument == "--out") outputDir = next("--out");
        else if (argument == "--input-digest") inputDigest = next("--input-digest");
        else if (argument == "--max-chunks") maxChunks = static_cast<std::size_t>(std::stoul(next("--max-chunks")));
        else if (argument == "--quiet") quiet = true;
        else {
            fail("unknown argument " + argument);
            return 2;
        }
    }

    if (inputPath.empty() || outputDir.empty()) {
        fail("--input and --out are required");
        return 2;
    }
    if (maxChunks == 0 || maxChunks > 262144) {
        fail("--max-chunks must be in [1, 262144]");
        return 2;
    }

    const std::string json = readTextFile(inputPath);
    if (json.empty()) {
        fail("cannot read the compile request: " + inputPath);
        return 1;
    }

    CompileInput input;
    std::string error;
    if (!WorldCompiler::parseInput(json, input, error)) {
        fail(error);
        return 1;
    }

    std::error_code code;
    const fs::path root = fs::absolute(fs::u8path(outputDir), code);
    if (code) {
        fail("cannot resolve the output directory");
        return 1;
    }
    fs::create_directories(root / "chunks", code);
    if (code) {
        fail("cannot create the chunk directory");
        return 1;
    }

    DirectorySink sink(root, maxChunks);
    WorldCompiler compiler;
    CompileStats stats;
    const auto started = GetTickCount64();
    if (!compiler.compile(input, sink, stats, error)) {
        fail(error);
        return 1;
    }
    const auto elapsed = GetTickCount64() - started;

    // Partial manifest: everything except `manifestHash`, which the JS service computes
    // from the canonical form. Canonicalization lives in one place on purpose.
    const std::string partial = WorldCompiler::manifestJson(input, stats, sink.infos(),
                                                           inputDigest.empty() ? std::string(64, '0') : inputDigest,
                                                           std::string(64, '0'));
    if (!writeFile(root / "manifest.partial.json", partial.data(), partial.size())) {
        fail("cannot write the partial manifest");
        return 1;
    }

    if (!quiet) {
        std::cout << "{\"ok\":true"
                  << ",\"chunks\":" << stats.chunks
                  << ",\"instances\":" << stats.totalInstances
                  << ",\"buildings\":" << stats.totalBuildings
                  << ",\"vertices\":" << stats.vertices
                  << ",\"bytes\":" << sink.totalBytes()
                  << ",\"budgetScale\":" << stats.budgetScale
                  << ",\"cookMillis\":" << stats.cookMillis
                  << ",\"wallMillis\":" << elapsed
                  << ",\"chunkRange\":{\"minCx\":" << engine::world::chunkRangeForBounds(input.worldBounds(), input.chunkSizeMetres).minCx
                  << ",\"minCz\":" << engine::world::chunkRangeForBounds(input.worldBounds(), input.chunkSizeMetres).minCz
                  << ",\"maxCx\":" << engine::world::chunkRangeForBounds(input.worldBounds(), input.chunkSizeMetres).maxCx
                  << ",\"maxCz\":" << engine::world::chunkRangeForBounds(input.worldBounds(), input.chunkSizeMetres).maxCz
                  << "}"
                  << ",\"manifest\":\"" << escape((root / "manifest.partial.json").string()) << "\""
                  << ",\"warnings\":[";
        for (std::size_t i = 0; i < stats.warnings.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << "\"" << escape(stats.warnings[i]) << "\"";
        }
        std::cout << "],\"pathDiagnostics\":[";
        for (std::size_t i = 0; i < stats.pathDiagnostics.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << "{\"code\":\"" << escape(stats.pathDiagnostics[i].code)
                      << "\",\"message\":\"" << escape(stats.pathDiagnostics[i].message)
                      << "\",\"pathId\":\"" << escape(stats.pathDiagnostics[i].pathId) << "\"}";
        }
        std::cout << "],\"settlementDiagnostics\":[";
        for (std::size_t i = 0; i < stats.settlementDiagnostics.size(); ++i) {
            if (i) std::cout << ",";
            const auto& diagnostic = stats.settlementDiagnostics[i];
            std::cout << "{\"code\":\"" << escape(diagnostic.code)
                      << "\",\"message\":\"" << escape(diagnostic.message)
                      << "\",\"x\":" << diagnostic.x << ",\"z\":" << diagnostic.z << "}";
        }
        std::cout << "]}\n";
    }
    return 0;
}
