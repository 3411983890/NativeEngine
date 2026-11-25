#include "world_draw.hpp"
#include "nativegpu_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

namespace nativegpu {
namespace {

constexpr float kPi = 3.14159265358979323846f;

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** out() { return &p; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
};

void check(HRESULT hr, const char* label) {
    if (FAILED(hr)) throw std::runtime_error(std::string("NG-E-WORLD-DRAW: ") + label);
}

// 36-byte vertex: position, normal, colour. The shader reads exactly these three.
struct WorldVertex {
    float position[3];
    float normal[3];
    float color[3];
};
static_assert(sizeof(WorldVertex) == 36, "WorldVertex stride is a shader contract");

// 80-byte instance record matching `InstanceRecord` in shaders/world.hlsl.
struct WorldInstanceRecord {
    // Column-major: col0..col3 are the four columns of the model matrix.
    float col0[4];
    float col1[4];
    float col2[4];
    float col3[4];
    float color[4];
};
static_assert(sizeof(WorldInstanceRecord) == 80, "instance stride is a shader contract");

struct FrameConstants {
    float viewProjection[16];
    float cameraTime[4];
    float keyDirectionIntensity[4];
    float keyColorAmbient[4];
    float fogColorDensity[4];
    float params[4];
    float chunkOrigin[4];
};
static_assert(sizeof(FrameConstants) == 160, "frame constant ABI");

// Column-major 4x4 from position + yaw + uniform scale, matching the engine's
// Translation * Yaw * Scale convention.
void composeInstance(float out[16], const float position[3], float yaw, float scale) {
    const float c = std::cos(yaw) * scale;
    const float s = std::sin(yaw) * scale;
    std::memset(out, 0, sizeof(float) * 16);
    // Yaw about +Y: +Z rotates toward +X.
    out[0] = c;  out[1] = 0.0f; out[2] = -s; out[3] = 0.0f;
    out[4] = 0.0f; out[5] = scale; out[6] = 0.0f; out[7] = 0.0f;
    out[8] = s;  out[9] = 0.0f; out[10] = c;  out[11] = 0.0f;
    out[12] = position[0]; out[13] = position[1]; out[14] = position[2]; out[15] = 1.0f;
}

void pushTriangle(std::vector<WorldVertex>& vertices, std::vector<std::uint32_t>& indices,
                  const WorldVertex& a, const WorldVertex& b, const WorldVertex& c) {
    const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
}

WorldVertex vertexAt(float x, float y, float z, const float normal[3], const float color[3]) {
    WorldVertex vertex{};
    vertex.position[0] = x; vertex.position[1] = y; vertex.position[2] = z;
    std::memcpy(vertex.normal, normal, sizeof(float) * 3);
    std::memcpy(vertex.color, color, sizeof(float) * 3);
    return vertex;
}

// ---------------------------------------------------------------- procedural meshes
//
// Every component is a stand-in built from its declared size. The shapes are chosen so a
// pine reads as a pine and a courtyard house reads as a block: the point is that the world
// is legible before any art exists, not that these are final models.

void buildBox(std::vector<WorldVertex>& vertices, std::vector<std::uint32_t>& indices,
              const float size[3], const float color[3]) {
    // Pivot at the base centre: a building placed on the terrain sits on it, not half in it.
    const float hx = size[0] * 0.5f;
    const float h = size[1];
    const float hz = size[2] * 0.5f;
    struct Face {
        float normal[3];
        float corner[4][3];
    };
    const Face faces[6] = {
        {{1, 0, 0},  {{hx, 0, -hz}, {hx, 0, hz}, {hx, h, hz}, {hx, h, -hz}}},
        {{-1, 0, 0}, {{-hx, 0, hz}, {-hx, 0, -hz}, {-hx, h, -hz}, {-hx, h, hz}}},
        {{0, 0, 1},  {{-hx, 0, hz}, {hx, 0, hz}, {hx, h, hz}, {-hx, h, hz}}},
        {{0, 0, -1}, {{hx, 0, -hz}, {-hx, 0, -hz}, {-hx, h, -hz}, {hx, h, -hz}}},
        {{0, 1, 0},  {{-hx, h, -hz}, {hx, h, -hz}, {hx, h, hz}, {-hx, h, hz}}},
        {{0, -1, 0}, {{-hx, 0, hz}, {hx, 0, hz}, {hx, 0, -hz}, {-hx, 0, -hz}}},
    };
    for (const auto& face : faces) {
        const WorldVertex a = vertexAt(face.corner[0][0], face.corner[0][1], face.corner[0][2], face.normal, color);
        const WorldVertex b = vertexAt(face.corner[1][0], face.corner[1][1], face.corner[1][2], face.normal, color);
        const WorldVertex c = vertexAt(face.corner[2][0], face.corner[2][1], face.corner[2][2], face.normal, color);
        const WorldVertex d = vertexAt(face.corner[3][0], face.corner[3][1], face.corner[3][2], face.normal, color);
        pushTriangle(vertices, indices, a, b, c);
        pushTriangle(vertices, indices, a, c, d);
    }
}

void buildCone(std::vector<WorldVertex>& vertices, std::vector<std::uint32_t>& indices,
               const float size[3], const float color[3], int sides, float taper) {
    const int segments = std::max(3, std::min(24, sides));
    const float radius = size[0] * 0.5f;
    const float topRadius = radius * std::max(0.0f, std::min(1.0f, taper));
    const float height = size[1];
    for (int i = 0; i < segments; ++i) {
        const float a0 = static_cast<float>(i) / segments * 2.0f * kPi;
        const float a1 = static_cast<float>(i + 1) / segments * 2.0f * kPi;
        const float x0 = std::cos(a0), z0 = std::sin(a0);
        const float x1 = std::cos(a1), z1 = std::sin(a1);
        // Side quad: two triangles with an outward normal.
        const float nx0 = x0, nz0 = z0, nx1 = x1, nz1 = z1;
        const float n0[3] = {nx0, 0.35f, nz0};
        const float n1[3] = {nx1, 0.35f, nz1};
        const WorldVertex b0 = vertexAt(x0 * radius, 0.0f, z0 * radius, n0, color);
        const WorldVertex b1 = vertexAt(x1 * radius, 0.0f, z1 * radius, n1, color);
        const WorldVertex t0 = vertexAt(x0 * topRadius, height, z0 * topRadius, n0, color);
        const WorldVertex t1 = vertexAt(x1 * topRadius, height, z1 * topRadius, n1, color);
        pushTriangle(vertices, indices, b0, t0, t1);
        pushTriangle(vertices, indices, b0, t1, b1);
    }
}

void buildSphere(std::vector<WorldVertex>& vertices, std::vector<std::uint32_t>& indices,
                 const float size[3], const float color[3], int segments, float taper) {
    const int rings = std::max(3, std::min(16, segments));
    const int slices = std::max(4, std::min(24, segments * 2));
    const float rx = size[0] * 0.5f;
    const float ry = size[1] * 0.5f * std::max(0.3f, std::min(1.0f, taper <= 0.0f ? 1.0f : taper));
    const float rz = size[2] * 0.5f;
    // A tree crown sits on its trunk, so the sphere is lifted so its bottom touches y = 0.
    const float centreY = size[1] - ry;
    for (int ring = 0; ring < rings; ++ring) {
        const float v0 = static_cast<float>(ring) / rings;
        const float v1 = static_cast<float>(ring + 1) / rings;
        const float phi0 = v0 * kPi;
        const float phi1 = v1 * kPi;
        for (int slice = 0; slice < slices; ++slice) {
            const float u0 = static_cast<float>(slice) / slices * 2.0f * kPi;
            const float u1 = static_cast<float>(slice + 1) / slices * 2.0f * kPi;
            const auto point = [&](float phi, float theta, float normal[3]) {
                const float nx = std::sin(phi) * std::cos(theta);
                const float ny = std::cos(phi);
                const float nz = std::sin(phi) * std::sin(theta);
                normal[0] = nx; normal[1] = ny; normal[2] = nz;
                return vertexAt(nx * rx, centreY + ny * ry, nz * rz, normal, color);
            };
            float n[3];
            const WorldVertex p00 = point(phi0, u0, n);
            const WorldVertex p10 = point(phi1, u0, n);
            const WorldVertex p11 = point(phi1, u1, n);
            const WorldVertex p01 = point(phi0, u1, n);
            pushTriangle(vertices, indices, p00, p10, p11);
            pushTriangle(vertices, indices, p00, p11, p01);
        }
    }
}

void buildCross(std::vector<WorldVertex>& vertices, std::vector<std::uint32_t>& indices,
                const float size[3], const float color[3], int planes) {
    // Ground cover is two or three intersecting quads: cheap, and it reads as volume from
    // any direction without a billboard that turns with the camera.
    const int count = std::max(2, std::min(4, planes));
    const float halfWidth = size[0] * 0.5f;
    const float height = size[1];
    for (int plane = 0; plane < count; ++plane) {
        const float angle = static_cast<float>(plane) / count * kPi;
        const float cx = std::cos(angle) * halfWidth;
        const float cz = std::sin(angle) * halfWidth;
        const float normal[3] = {0.0f, 1.0f, 0.0f};
        const WorldVertex a = vertexAt(-cx, 0.0f, -cz, normal, color);
        const WorldVertex b = vertexAt(cx, 0.0f, cz, normal, color);
        const WorldVertex c = vertexAt(cx, height, cz, normal, color);
        const WorldVertex d = vertexAt(-cx, height, -cz, normal, color);
        pushTriangle(vertices, indices, a, b, c);
        pushTriangle(vertices, indices, a, c, d);
    }
}

struct ComponentMesh {
    Com<ID3D12Resource> vertices;
    Com<ID3D12Resource> indices;
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    D3D12_INDEX_BUFFER_VIEW indexView{};
    std::uint32_t indexCount = 0;
    float color[3] = {1, 1, 1};
};

} // namespace

struct WorldDraw::Impl {
    Com<ID3D12Device> device;
    Com<ID3D12RootSignature> root;
    Com<ID3D12PipelineState> pipeline;
    std::vector<ComponentMesh> components;
    WorldDrawStats stats;

    struct ChunkBuffers {
        Com<ID3D12Resource> terrainVertices;
        Com<ID3D12Resource> terrainIndices;
        Com<ID3D12Resource> instances;
        // Two constant buffers, not one. The command list is recorded now and executed
        // later, so rewriting a shared buffer between the terrain draw and the instance
        // draw makes both reads see the last write: the terrain then reads garbage model
        // matrices out of the instance buffer and covers the screen with a stray triangle.
        Com<ID3D12Resource> constants;
        Com<ID3D12Resource> constantsInstanced;
        Com<ID3D12DescriptorHeap> instanceHeap;
        D3D12_VERTEX_BUFFER_VIEW terrainVertexView{};
        D3D12_INDEX_BUFFER_VIEW terrainIndexView{};
        void* mappedConstants = nullptr;
        void* mappedConstantsInstanced = nullptr;
        std::uint32_t terrainIndexCount = 0;
        std::uint32_t instanceCount = 0;
        float centre[3] = {0, 0, 0};
        engine::world::Bounds2 bounds{};
        // Instances grouped by component so one draw call covers a whole species.
        std::vector<std::pair<std::uint16_t, std::pair<std::uint32_t, std::uint32_t>>> ranges;
    };
    std::map<std::uint64_t, ChunkBuffers> chunks;

    Impl(ID3D12Device* d, DXGI_FORMAT color, DXGI_FORMAT depth) {
        device.p = d;
        d->AddRef();

        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].Descriptor.ShaderRegister = 0;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = 0;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &range;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 2;
        signature.pParameters = parameters;
        signature.NumStaticSamplers = 0;
        signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        Com<ID3DBlob> blob, diagnostics;
        check(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, blob.out(), diagnostics.out()),
              "serialize root signature");
        check(d->CreateRootSignature(0, blob.p->GetBufferPointer(), blob.p->GetBufferSize(),
                                     IID_PPV_ARGS(root.out())), "create root signature");

        D3D12_INPUT_ELEMENT_DESC layout[3]{};
        layout[0] = {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
        layout[1] = {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
        layout[2] = {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
        pipelineDesc.pRootSignature = root.p;
        pipelineDesc.VS = {shaders::kWorldTerrainVS.data, shaders::kWorldTerrainVS.size};
        pipelineDesc.PS = {shaders::kWorldTerrainPS.data, shaders::kWorldTerrainPS.size};
        pipelineDesc.InputLayout = {layout, 3};
        pipelineDesc.SampleMask = UINT_MAX;
        pipelineDesc.SampleDesc.Count = 1;
        pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        // World geometry is drawn single-sided for the terrain and double-sided for the
        // grass cross; culling nothing keeps both cases in one pipeline at the cost of a
        // few back faces.
        pipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipelineDesc.RasterizerState.DepthClipEnable = TRUE;
        pipelineDesc.DepthStencilState.DepthEnable = TRUE;
        pipelineDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pipelineDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pipelineDesc.DSVFormat = depth;
        pipelineDesc.NumRenderTargets = 1;
        pipelineDesc.RTVFormats[0] = color;
        pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipelineDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        check(d->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(pipeline.out())), "create pipeline");
    }

    // One descriptor heap per chunk, created once at upload. Building a heap per draw call
    // would allocate a COM object per chunk per frame.
    void buildInstanceHeap(ChunkBuffers& chunk) {
        chunk.instanceHeap.reset();
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 1;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device.p->CreateDescriptorHeap(&desc, IID_PPV_ARGS(chunk.instanceHeap.out())), "create instance heap");
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_UNKNOWN;
        view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Buffer.FirstElement = 0;
        view.Buffer.NumElements = std::max<std::uint32_t>(1, chunk.instanceCount);
        view.Buffer.StructureByteStride = sizeof(WorldInstanceRecord);
        device.p->CreateShaderResourceView(chunk.instances.p, &view,
                                           chunk.instanceHeap.p->GetCPUDescriptorHandleForHeapStart());
    }

    void uploadBuffer(Com<ID3D12Resource>& resource, std::size_t size, const void* data, void** mapped) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = std::max<std::size_t>(size, 1);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device.p->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(resource.out())), "create upload buffer");
        void* pointer = nullptr;
        D3D12_RANGE readRange{0, 0};
        check(resource.p->Map(0, &readRange, &pointer), "map upload buffer");
        if (data && size) std::memcpy(pointer, data, size);
        if (mapped) *mapped = pointer;
        stats.uploadBytes += size;
    }
};

WorldDraw::WorldDraw(ID3D12Device* device, DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat)
    : impl_(std::make_unique<Impl>(device, colorFormat, depthFormat)) {}

WorldDraw::~WorldDraw() = default;

bool WorldDraw::setComponents(ID3D12GraphicsCommandList* list, const std::vector<WorldComponentDesc>& components,
                              std::string& error) {
    (void)list;
    try {
        auto& state = *impl_;
        state.components.clear();
        state.components.resize(components.size());
        for (std::size_t index = 0; index < components.size(); ++index) {
            const auto& desc = components[index];
            std::vector<WorldVertex> vertices;
            std::vector<std::uint32_t> indices;
            const float* size = desc.sizeMetres;
            if (desc.shape == "cone") buildCone(vertices, indices, size, desc.albedo, 8, 0.15f);
            else if (desc.shape == "sphere") buildSphere(vertices, indices, size, desc.albedo, 6, 0.9f);
            else if (desc.shape == "cross") buildCross(vertices, indices, size, desc.albedo, 3);
            else if (desc.shape == "prism") buildCone(vertices, indices, size, desc.albedo, 3, 0.85f);
            else buildBox(vertices, indices, size, desc.albedo);
            if (vertices.empty() || indices.empty()) {
                error = "NG-E-WORLD-DRAW: component '" + desc.id + "' produced no geometry";
                return false;
            }
            auto& mesh = state.components[index];
            state.uploadBuffer(mesh.vertices, vertices.size() * sizeof(WorldVertex), vertices.data(), nullptr);
            state.uploadBuffer(mesh.indices, indices.size() * sizeof(std::uint32_t), indices.data(), nullptr);
            mesh.vertexView = {mesh.vertices.p->GetGPUVirtualAddress(),
                               static_cast<UINT>(vertices.size() * sizeof(WorldVertex)), sizeof(WorldVertex)};
            mesh.indexView = {mesh.indices.p->GetGPUVirtualAddress(), static_cast<UINT>(indices.size()),
                              DXGI_FORMAT_R32_UINT};
            mesh.indexCount = static_cast<std::uint32_t>(indices.size());
            mesh.color[0] = desc.albedo[0];
            mesh.color[1] = desc.albedo[1];
            mesh.color[2] = desc.albedo[2];
        }
        state.stats.components = static_cast<std::uint32_t>(state.components.size());
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

bool WorldDraw::uploadChunk(ID3D12GraphicsCommandList* list, const engine::world::ChunkData& chunk,
                            std::string& error) {
    (void)list;
    try {
        if (!chunk.valid()) {
            error = "NG-E-WORLD-DRAW: chunk data is inconsistent";
            return false;
        }
        auto& state = *impl_;
        const std::uint64_t key = engine::world::chunkKeyOf(chunk.coord);

        // Build terrain geometry first, in local memory: if the instance upload fails the
        // previous chunk content is still intact.
        const std::uint32_t n = chunk.vertexCount;
        const float step = chunk.chunkSizeMetres / static_cast<float>(n - 1);
        std::vector<WorldVertex> vertices(static_cast<std::size_t>(n) * n);
        for (std::uint32_t iz = 0; iz < n; ++iz) {
            for (std::uint32_t ix = 0; ix < n; ++ix) {
                const std::uint32_t index = chunk.vertexIndex(ix, iz);
                std::int16_t encoded[2] = {chunk.normals[index * 2], chunk.normals[index * 2 + 1]};
                float normal[3];
                engine::world::decodeNormalOctahedral(encoded, normal);
                // Material weights become the vertex colour: grass, rock, dirt, sand.
                const float grass = chunk.materialWeights[index * 4 + 0] / 255.0f;
                const float rock = chunk.materialWeights[index * 4 + 1] / 255.0f;
                const float dirt = chunk.materialWeights[index * 4 + 2] / 255.0f;
                const float sand = chunk.materialWeights[index * 4 + 3] / 255.0f;
                const float total = std::max(0.0001f, grass + rock + dirt + sand);
                const float color[3] = {
                    (grass * 0.31f + rock * 0.44f + dirt * 0.36f + sand * 0.62f) / total,
                    (grass * 0.42f + rock * 0.43f + dirt * 0.30f + sand * 0.58f) / total,
                    (grass * 0.19f + rock * 0.40f + dirt * 0.21f + sand * 0.48f) / total,
                };
                auto& vertex = vertices[index];
                vertex.position[0] = static_cast<float>(ix) * step;
                vertex.position[1] = chunk.heights[index];
                vertex.position[2] = static_cast<float>(iz) * step;
                std::memcpy(vertex.normal, normal, sizeof(float) * 3);
                std::memcpy(vertex.color, color, sizeof(float) * 3);
            }
        }
        std::vector<std::uint32_t> indices;
        indices.reserve(static_cast<std::size_t>(n - 1) * (n - 1) * 6);
        for (std::uint32_t iz = 0; iz + 1 < n; ++iz) {
            for (std::uint32_t ix = 0; ix + 1 < n; ++ix) {
                const std::uint32_t a = chunk.vertexIndex(ix, iz);
                const std::uint32_t b = chunk.vertexIndex(ix + 1, iz);
                const std::uint32_t c = chunk.vertexIndex(ix + 1, iz + 1);
                const std::uint32_t d = chunk.vertexIndex(ix, iz + 1);
                indices.push_back(a); indices.push_back(d); indices.push_back(c);
                indices.push_back(a); indices.push_back(c); indices.push_back(b);
            }
        }

        std::vector<WorldInstanceRecord> records;
        records.reserve(chunk.instances.size());
        for (const auto& instance : chunk.instances) {
            WorldInstanceRecord record{};
            float model[16];
            // Chunk-local coordinates: the shader receives a model matrix relative to the
            // chunk origin and the chunk's own transform is folded in below, which keeps
            // the float magnitudes small in an 8 km world.
            const float local[3] = {
                instance.position[0] - chunk.bounds.minX,
                instance.position[1],
                instance.position[2] - chunk.bounds.minZ,
            };
            composeInstance(model, local, instance.yaw, instance.scale);
            std::memcpy(record.col0, model + 0, sizeof(float) * 4);
            std::memcpy(record.col1, model + 4, sizeof(float) * 4);
            std::memcpy(record.col2, model + 8, sizeof(float) * 4);
            std::memcpy(record.col3, model + 12, sizeof(float) * 4);
            const float white[4] = {1, 1, 1, 1};
            std::memcpy(record.color, white, sizeof(white));
            records.push_back(record);
        }

        auto& target = state.chunks[key];
        target.terrainVertices.reset();
        target.terrainIndices.reset();
        target.instances.reset();
        target.constants.reset();
        target.constantsInstanced.reset();
        target.instanceHeap.reset();
        state.uploadBuffer(target.terrainVertices, vertices.size() * sizeof(WorldVertex), vertices.data(), nullptr);
        state.uploadBuffer(target.terrainIndices, indices.size() * sizeof(std::uint32_t), indices.data(), nullptr);
        state.uploadBuffer(target.instances, std::max<std::size_t>(1, records.size() * sizeof(WorldInstanceRecord)),
                           records.empty() ? nullptr : records.data(), nullptr);
        state.uploadBuffer(target.constants, sizeof(FrameConstants), nullptr, &target.mappedConstants);
        state.uploadBuffer(target.constantsInstanced, sizeof(FrameConstants), nullptr, &target.mappedConstantsInstanced);
        target.terrainVertexView = {target.terrainVertices.p->GetGPUVirtualAddress(),
                                    static_cast<UINT>(vertices.size() * sizeof(WorldVertex)), sizeof(WorldVertex)};
        target.terrainIndexView = {target.terrainIndices.p->GetGPUVirtualAddress(),
                                   static_cast<UINT>(indices.size()), DXGI_FORMAT_R32_UINT};
        target.terrainIndexCount = static_cast<std::uint32_t>(indices.size());
        target.instanceCount = static_cast<std::uint32_t>(records.size());
        target.bounds = chunk.bounds;
        target.centre[0] = chunk.bounds.centreX();
        target.centre[1] = (chunk.minHeight + chunk.maxHeight) * 0.5f;
        target.centre[2] = chunk.bounds.centreZ();

        // Group instance indices by component. A forest becomes one draw per species
        // instead of one per tree, which is the difference between 40k draws and 4.
        std::map<std::uint16_t, std::vector<std::uint32_t>> byComponent;
        for (std::size_t index = 0; index < chunk.instances.size(); ++index) {
            byComponent[chunk.instances[index].component].push_back(static_cast<std::uint32_t>(index));
        }
        target.ranges.clear();
        // Reorder the instance buffer so each component's records are contiguous: a
        // DrawIndexedInstanced can only address one contiguous instance range.
        std::vector<WorldInstanceRecord> ordered;
        ordered.reserve(records.size());
        for (const auto& entry : byComponent) {
            const std::uint32_t first = static_cast<std::uint32_t>(ordered.size());
            for (std::uint32_t index : entry.second) ordered.push_back(records[index]);
            target.ranges.push_back({entry.first, {first, static_cast<std::uint32_t>(entry.second.size())}});
        }
        if (!ordered.empty()) {
            void* mapped = nullptr;
            D3D12_RANGE readRange{0, 0};
            check(target.instances.p->Map(0, &readRange, &mapped), "map instance buffer");
            std::memcpy(mapped, ordered.data(), ordered.size() * sizeof(WorldInstanceRecord));
            target.instances.p->Unmap(0, nullptr);
        }
        state.buildInstanceHeap(target);
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

void WorldDraw::releaseChunk(std::int32_t cx, std::int32_t cz) {
    const std::uint64_t key = engine::world::chunkKeyOf({cx, cz});
    auto it = impl_->chunks.find(key);
    if (it == impl_->chunks.end()) return;
    // The caller has already waited on the frame fence for this chunk; releasing the COM
    // references here is what returns the upload heap memory.
    impl_->chunks.erase(it);
    impl_->stats.releasedChunks += 1;
}

bool WorldDraw::draw(ID3D12GraphicsCommandList* list, const float viewProjection[16],
                     const WorldDrawLighting& lighting, float drawDistance, std::string& error) {
    try {
        auto& state = *impl_;
        if (state.chunks.empty()) {
            state.stats.frames += 1;
            return true;
        }
        list->SetGraphicsRootSignature(state.root.p);
        list->SetPipelineState(state.pipeline.p);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        state.stats.frames += 1;
        state.stats.chunksResident = state.chunks.size();
        state.stats.chunksDrawn = 0;
        state.stats.instancesDrawn = 0;
        state.stats.triangles = 0;
        state.stats.terrainVertices = 0;

        for (auto& entry : state.chunks) {
            auto& chunk = entry.second;
            const float dx = chunk.centre[0] - lighting.camera[0];
            const float dz = chunk.centre[2] - lighting.camera[2];
            if (drawDistance > 0.0f && std::sqrt(dx * dx + dz * dz) > drawDistance) continue;

            FrameConstants constants{};
            std::memcpy(constants.viewProjection, viewProjection, sizeof(float) * 16);
            constants.cameraTime[0] = lighting.camera[0];
            constants.cameraTime[1] = lighting.camera[1];
            constants.cameraTime[2] = lighting.camera[2];
            constants.cameraTime[3] = lighting.seconds;
            std::memcpy(constants.keyDirectionIntensity, lighting.keyDirection, sizeof(float) * 3);
            constants.keyDirectionIntensity[3] = lighting.keyIntensity;
            std::memcpy(constants.keyColorAmbient, lighting.keyColor, sizeof(float) * 3);
            constants.keyColorAmbient[3] = lighting.ambient;
            std::memcpy(constants.fogColorDensity, lighting.fogColor, sizeof(float) * 3);
            constants.fogColorDensity[3] = lighting.fogDensity;
            constants.params[0] = 0.0f;
            constants.params[2] = static_cast<float>(debugMode_);
            // Terrain vertices are chunk-local; the origin is added in the vertex shader so
            // the same constants buffer can be reused for the instanced pass.
            constants.chunkOrigin[0] = chunk.bounds.minX;
            constants.chunkOrigin[1] = 0.0f;
            constants.chunkOrigin[2] = chunk.bounds.minZ;
            std::memcpy(chunk.mappedConstants, &constants, sizeof(constants));

            list->SetGraphicsRootConstantBufferView(0, chunk.constants.p->GetGPUVirtualAddress());
            ID3D12DescriptorHeap* heaps[]{chunk.instanceHeap.p};
            list->SetDescriptorHeaps(1, heaps);
            list->SetGraphicsRootDescriptorTable(1, chunk.instanceHeap.p->GetGPUDescriptorHandleForHeapStart());

            list->IASetVertexBuffers(0, 1, &chunk.terrainVertexView);
            list->IASetIndexBuffer(&chunk.terrainIndexView);
            list->DrawIndexedInstanced(chunk.terrainIndexCount, 1, 0, 0, 0);
            state.stats.triangles += chunk.terrainIndexCount / 3;
            state.stats.terrainVertices += chunk.terrainVertexView.SizeInBytes / sizeof(WorldVertex);

            if (chunk.instanceCount == 0) {
                state.stats.chunksDrawn += 1;
                continue;
            }
            constants.params[0] = 1.0f;
            std::memcpy(chunk.mappedConstantsInstanced, &constants, sizeof(constants));
            list->SetGraphicsRootConstantBufferView(0, chunk.constantsInstanced.p->GetGPUVirtualAddress());
            for (const auto& range : chunk.ranges) {
                if (range.first >= state.components.size()) continue;
                const auto& mesh = state.components[range.first];
                list->IASetVertexBuffers(0, 1, &mesh.vertexView);
                list->IASetIndexBuffer(&mesh.indexView);
                list->DrawIndexedInstanced(mesh.indexCount, range.second.second, 0, 0, range.second.first);
                state.stats.triangles += static_cast<std::uint64_t>(mesh.indexCount / 3) * range.second.second;
                state.stats.instancesDrawn += range.second.second;
            }
            state.stats.chunksDrawn += 1;
        }
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

WorldDrawStats WorldDraw::stats() const { return impl_->stats; }

} // namespace nativegpu
