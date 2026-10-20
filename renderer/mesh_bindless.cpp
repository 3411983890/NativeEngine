// Mesh shader sample and bindless material slot management.
//
// Mesh path: one amplification shader group per rigid instance culls the six cube-face
// meshlets, then DispatchMesh launches one mesh-shader group per visible meshlet. The
// pipeline is a D3D12 stream state object (AS + MS + PS), which the legacy
// D3D12_GRAPHICS_PIPELINE_STATE_DESC cannot describe. The vertex/instance path stays the
// fallback and is used whenever the mesh pipeline is unavailable or disabled.
//
// Bindless: a bounded, generation-checked material table. Slot 0 is the default
// "missing texture" slot. Slots can be acquired, released and reused; a stale generation
// resolves to the default slot and is reported instead of sampling the wrong texture.

#include "d3d12_renderer_internal.h"

#include "nativegpu_shaders.h"

#include <cstring>
#include <sstream>

namespace nativegpu {
namespace {

// One meshlet per cube face: 4 vertices and 2 triangles, matching CubeVertices/Indices.
constexpr unsigned FaceMeshletVertexCount = 4;
constexpr unsigned FaceMeshletTriangleCount = 2;
constexpr unsigned FaceCount = 6;

struct GpuMeshlet {
    unsigned vertexOffset;
    unsigned vertexCount;
    unsigned triangleOffset;
    unsigned triangleCount;
    float center[4];
    float normal[4];
};
static_assert(sizeof(GpuMeshlet) == 48, "Meshlet layout changed");

struct MeshCameraConstants {
    float eye[4];
    float params[4];
};

// A pipeline state stream needs every element 8-byte aligned and 8-byte sized.
template<class T> struct alignas(8) StreamElement {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T value;
};

struct PipelineStream {
    std::vector<unsigned char> bytes;
    template<class T> void add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const T& value) {
        const size_t aligned = (bytes.size() + 7u) & ~static_cast<size_t>(7u);
        bytes.resize(aligned, 0);
        StreamElement<T> element{type, value};
        const auto* raw = reinterpret_cast<const unsigned char*>(&element);
        bytes.insert(bytes.end(), raw, raw + sizeof(StreamElement<T>));
    }
};

D3D12_BLEND_DESC opaqueBlend() {
    D3D12_BLEND_DESC blend{};
    auto& target = blend.RenderTarget[0];
    target.SrcBlend = D3D12_BLEND_ONE;
    target.DestBlend = D3D12_BLEND_ZERO;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D12_BLEND_ONE;
    target.DestBlendAlpha = D3D12_BLEND_ZERO;
    target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    return blend;
}

D3D12_RASTERIZER_DESC solidRaster() {
    D3D12_RASTERIZER_DESC raster{};
    raster.FillMode = D3D12_FILL_MODE_SOLID;
    raster.CullMode = D3D12_CULL_MODE_BACK;
    raster.FrontCounterClockwise = FALSE;
    raster.DepthClipEnable = TRUE;
    raster.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    raster.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    raster.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    return raster;
}

D3D12_DEPTH_STENCIL_DESC depthEnabled() {
    D3D12_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depth.StencilEnable = FALSE;
    depth.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    depth.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    depth.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    depth.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    depth.BackFace = depth.FrontFace;
    return depth;
}

} // namespace

using detail::MaterialTable;
using detail::MaterialDefaultSlot;

// --- material table ---------------------------------------------------------

MaterialTable::MaterialTable() {
    for (unsigned index = 0; index < detail::MaterialSlotCount; ++index) {
        slots[index].generation = 1;
        slots[index].inUse = false;
        slots[index].textureId = index;
    }
    // Slot 0 is the default "missing texture" slot and is always resident: it samples the
    // dedicated magenta checker so a missing material is obvious on screen.
    slots[MaterialDefaultSlot].inUse = true;
    slots[MaterialDefaultSlot].textureId = 3;
    // Slots 1..3 are the three built-in materials (checker, brick, metal) and stay
    // resident. Slots 4..7 are free for acquisition, release and reuse.
    for (unsigned index = 1; index <= 3; ++index) {
        slots[index].inUse = true;
        slots[index].textureId = index - 1;
    }
}

unsigned MaterialTable::acquire(unsigned textureId, unsigned& outGeneration) {
    ++acquired;
    unsigned chosen = detail::MaterialSlotCount;
    for (unsigned index = 1; index < detail::MaterialSlotCount; ++index) {
        if (!slots[index].inUse) { chosen = index; break; }
    }
    if (chosen == detail::MaterialSlotCount) {
        // No free slot: reuse the oldest generation so the table stays bounded.
        ++reused;
        unsigned oldest = 1;
        for (unsigned index = 2; index < detail::MaterialSlotCount; ++index) {
            if (slots[index].generation < slots[oldest].generation) oldest = index;
        }
        chosen = oldest;
        ++slots[chosen].generation;
    }
    slots[chosen].inUse = true;
    slots[chosen].textureId = textureId % detail::MaterialSlotCount;
    outGeneration = slots[chosen].generation;
    if (slots[chosen].generation > generationHighWater) {
        generationHighWater = slots[chosen].generation;
    }
    return chosen;
}

bool MaterialTable::release(unsigned slot) {
    if (slot == MaterialDefaultSlot || slot >= detail::MaterialSlotCount) return false;
    if (!slots[slot].inUse) return false;
    slots[slot].inUse = false;
    ++slots[slot].generation; // invalidates any in-flight reference
    ++released;
    return true;
}

bool MaterialTable::resolve(unsigned slot, unsigned generation, unsigned& outTexture,
                            unsigned& outGeneration) {
    if (slot == MaterialDefaultSlot) {
        outTexture = slots[MaterialDefaultSlot].textureId;
        outGeneration = slots[MaterialDefaultSlot].generation;
        return true;
    }
    if (slot >= detail::MaterialSlotCount || !slots[slot].inUse) {
        ++missingFallbacks;
        outTexture = slots[MaterialDefaultSlot].textureId;
        outGeneration = slots[MaterialDefaultSlot].generation;
        return false;
    }
    if (generation != 0 && generation != slots[slot].generation) {
        ++staleRejections;
        outTexture = slots[MaterialDefaultSlot].textureId;
        outGeneration = slots[MaterialDefaultSlot].generation;
        return false;
    }
    outTexture = slots[slot].textureId;
    outGeneration = slots[slot].generation;
    return true;
}

bool Renderer::Impl::selfTestMaterialTable(std::string& error) {
    error.clear();
    unsigned generationA = 0;
    const unsigned slotA = materials.acquire(1, generationA);
    unsigned generationB = 0;
    const unsigned slotB = materials.acquire(2, generationB);
    if (slotA == slotB) { error = "acquire returned the same slot twice"; return false; }
    unsigned texture = 0;
    unsigned generation = 0;
    if (!materials.resolve(slotA, generationA, texture, generation)) {
        error = "freshly acquired slot did not resolve";
        return false;
    }
    if (materials.resolve(slotA, generationA + 7, texture, generation)) {
        error = "stale generation resolved instead of falling back";
        return false;
    }
    addDiagnostic("NG-E-MAT-STALE", "self-test: stale generation rejected and fell back");
    if (!materials.release(slotA)) { error = "release of an acquired slot failed"; return false; }
    if (materials.resolve(slotA, generationA, texture, generation)) {
        error = "released slot still resolves";
        return false;
    }
    unsigned generationC = 0;
    const unsigned slotC = materials.acquire(3, generationC);
    if (slotC != slotA) { error = "released slot was not reused"; return false; }
    if (generationC == generationA) { error = "reused slot did not bump its generation"; return false; }
    if (!materials.resolve(slotC, generationC, texture, generation)) {
        error = "reused slot does not resolve with the new generation";
        return false;
    }
    materials.release(slotB);
    materials.release(slotC);
    return true;
}

std::string Renderer::Impl::bindlessJson() const {
    std::ostringstream out;
    out << "{"
        << "\"mode\":\"bounded-table\""
        << ",\"slots\":" << detail::MaterialSlotCount
        << ",\"defaultSlot\":" << MaterialDefaultSlot
        << ",\"acquired\":" << materials.acquired
        << ",\"released\":" << materials.released
        << ",\"reused\":" << materials.reused
        << ",\"missingFallbacks\":" << materials.missingFallbacks
        << ",\"staleRejections\":" << materials.staleRejections
        << ",\"generationHighWater\":" << materials.generationHighWater
        << ",\"slotState\":[";
    for (unsigned index = 0; index < detail::MaterialSlotCount; ++index) {
        if (index) out << ',';
        out << "{\"slot\":" << index
            << ",\"inUse\":" << (materials.slots[index].inUse ? "true" : "false")
            << ",\"generation\":" << materials.slots[index].generation
            << ",\"texture\":" << materials.slots[index].textureId << "}";
    }
    out << "]"
        << ",\"sm66HeapIndexing\":{\"supported\":"
        << ((shaderModel >= 0x66 && bindingTier >= 3) ? "true" : "false")
        << ",\"implemented\":false"
        << ",\"reason\":\"ResourceDescriptorHeap indexing needs a ps_6_6 pipeline and an "
           "unbounded range; the bounded table with generations is used instead\"}"
        << ",\"notes\":\"Bounded table with slot generations, default slot and reuse. Not "
           "SM 6.6 descriptor heap indexing.\""
        << "}";
    return out.str();
}

// --- meshlet data -----------------------------------------------------------

bool Renderer::Impl::createMeshletData(std::string& error) {
    if (meshletVertices) return true;
    constexpr unsigned vertexCount = FaceCount * FaceMeshletVertexCount;
    constexpr unsigned triangleIndexCount = FaceCount * FaceMeshletTriangleCount * 3;
    // Generate the unit cube face by face. Six faces x 4 corners = 24 vertices, and each
    // face is its own meshlet with two triangles. This mirrors the raster unit cube.
    Vertex vertices[vertexCount] = {};
    unsigned indices[triangleIndexCount] = {};
    GpuMeshlet meshlets[FaceCount] = {};
    const float axes[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    for (unsigned face = 0; face < FaceCount; ++face) {
        const float* normal = axes[face];
        // Basis vectors for the face plane.
        float right[3];
        float up[3];
        if (normal[0] != 0.0f) {
            right[0] = 0.0f; right[1] = 0.0f; right[2] = -normal[0];
            up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
        } else if (normal[1] != 0.0f) {
            right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f;
            up[0] = 0.0f; up[1] = 0.0f; up[2] = -normal[1];
        } else {
            right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f;
            up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
        }
        const float cornerUv[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        for (unsigned corner = 0; corner < 4; ++corner) {
            const float signU = (corner == 0 || corner == 3) ? -1.0f : 1.0f;
            const float signV = (corner < 2) ? -1.0f : 1.0f;
            Vertex& vertex = vertices[face * 4 + corner];
            vertex.x = normal[0] * 0.5f + right[0] * signU * 0.5f + up[0] * signV * 0.5f;
            vertex.y = normal[1] * 0.5f + right[1] * signU * 0.5f + up[1] * signV * 0.5f;
            vertex.z = normal[2] * 0.5f + right[2] * signU * 0.5f + up[2] * signV * 0.5f;
            vertex.nx = normal[0];
            vertex.ny = normal[1];
            vertex.nz = normal[2];
            vertex.u = cornerUv[corner][0];
            vertex.v = cornerUv[corner][1];
        }
        // Two triangles, clockwise when seen from outside (matching the raster cube).
        const unsigned base = face * 4;
        // Clockwise when seen from outside, matching the raster cube winding
        // (FrontCounterClockwise = FALSE, so clockwise triangles are front facing).
        indices[face * 6 + 0] = base + 0;
        indices[face * 6 + 1] = base + 1;
        indices[face * 6 + 2] = base + 2;
        indices[face * 6 + 3] = base + 0;
        indices[face * 6 + 4] = base + 2;
        indices[face * 6 + 5] = base + 3;
        GpuMeshlet& meshlet = meshlets[face];
        meshlet.vertexOffset = base;
        meshlet.vertexCount = FaceMeshletVertexCount;
        meshlet.triangleOffset = face * FaceMeshletTriangleCount;
        meshlet.triangleCount = FaceMeshletTriangleCount;
        meshlet.center[0] = normal[0] * 0.5f;
        meshlet.center[1] = normal[1] * 0.5f;
        meshlet.center[2] = normal[2] * 0.5f;
        meshlet.center[3] = 0.7071f; // bounding radius of a unit face
        meshlet.normal[0] = normal[0];
        meshlet.normal[1] = normal[1];
        meshlet.normal[2] = normal[2];
        meshlet.normal[3] = 0.0f;
    }

    if (!createBuffer(sizeof(Vertex) * vertexCount, D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &meshletVertices, error) ||
        !createBuffer(sizeof(unsigned) * triangleIndexCount, D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &meshletTriangles, error) ||
        !createBuffer(sizeof(GpuMeshlet) * FaceCount, D3D12_HEAP_TYPE_UPLOAD,
                      D3D12_RESOURCE_STATE_GENERIC_READ, &meshletDescs, error) ||
        !createBuffer(256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                      &meshCameraBuffer, error)) {
        return false;
    }
    D3D12_RANGE noRead{0, 0};
    void* mapped = nullptr;
    if (FAILED(meshletVertices->Map(0, &noRead, &mapped)) || !mapped) {
        error = "Map meshlet vertex buffer failed";
        return false;
    }
    std::memcpy(mapped, vertices, sizeof(vertices));
    D3D12_RANGE vertexWritten{0, sizeof(vertices)};
    meshletVertices->Unmap(0, &vertexWritten);

    if (FAILED(meshletTriangles->Map(0, &noRead, &mapped)) || !mapped) {
        error = "Map meshlet index buffer failed";
        return false;
    }
    std::memcpy(mapped, indices, sizeof(indices));
    D3D12_RANGE triangleWritten{0, sizeof(indices)};
    meshletTriangles->Unmap(0, &triangleWritten);

    if (FAILED(meshletDescs->Map(0, &noRead, &mapped)) || !mapped) {
        error = "Map meshlet descriptor buffer failed";
        return false;
    }
    std::memcpy(mapped, meshlets, sizeof(meshlets));
    D3D12_RANGE meshletWritten{0, sizeof(meshlets)};
    meshletDescs->Unmap(0, &meshletWritten);

    if (FAILED(meshCameraBuffer->Map(0, &noRead,
             reinterpret_cast<void**>(&meshCameraMapped))) || !meshCameraMapped) {
        error = "Map mesh camera buffer failed";
        return false;
    }
    std::memset(meshCameraMapped, 0, 256);
    meshletCount = FaceCount;

    // Structured buffer views share the frame heap with the material table.
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Format = DXGI_FORMAT_UNKNOWN;
    view.Buffer.FirstElement = 0;

    view.Buffer.NumElements = vertexCount;
    view.Buffer.StructureByteStride = sizeof(Vertex);
    device->CreateShaderResourceView(meshletVertices, &view,
                                    cpuDescriptor(detail::SlotMeshVertices));
    view.Buffer.NumElements = triangleIndexCount;
    view.Buffer.StructureByteStride = sizeof(unsigned);
    device->CreateShaderResourceView(meshletTriangles, &view,
                                    cpuDescriptor(detail::SlotMeshTriangles));
    view.Buffer.NumElements = FaceCount;
    view.Buffer.StructureByteStride = sizeof(GpuMeshlet);
    device->CreateShaderResourceView(meshletDescs, &view, cpuDescriptor(detail::SlotMeshlets));
    return true;
}

// --- mesh pipeline ----------------------------------------------------------

void Renderer::Impl::releaseMeshPipeline() {
    destroy(meshPipeline);
    destroy(meshRootSignature);
    if (meshCameraBuffer && meshCameraMapped) {
        D3D12_RANGE noWrite{0, 0};
        meshCameraBuffer->Unmap(0, &noWrite);
        meshCameraMapped = nullptr;
    }
    destroy(meshCameraBuffer);
    destroy(meshletDescs);
    destroy(meshletTriangles);
    destroy(meshletVertices);
    meshPipelineCreated = false;
}

bool Renderer::Impl::buildMeshPipeline(std::string& error) {
    error.clear();
    if (meshPipelineCreated) return true;
    if (meshShaderTier < 10) {
        error = "mesh shader tier is unsupported on this adapter";
        addDiagnostic("NG-E-MESH-UNSUPPORTED", error);
        return false;
    }
    if (!shaders::kDxcAvailable) {
        error = shaders::kDxcError[0] ? shaders::kDxcError : "dxcUnavailable";
        addDiagnostic("NG-E-MESH-UNSUPPORTED", error);
        return false;
    }
    if (shaders::kMeshAmplification.size < 64 || shaders::kMeshShader.size < 64 ||
        shaders::kMeshPixel.size < 64) {
        error = "mesh shader blobs are empty; the mesh shader library did not compile";
        addDiagnostic("NG-E-MESH-PSO", error);
        return false;
    }
    if (!device2) {
        error = "ID3D12Device2 is unavailable (stream pipeline states)";
        addDiagnostic("NG-E-MESH-PSO", error);
        return false;
    }
    if (!createMeshletData(error)) {
        addDiagnostic("NG-E-MESH-DATA", error);
        return false;
    }
    if (!meshRootSignature) {
        D3D12_DESCRIPTOR_RANGE ranges[4]{};
        for (unsigned index = 0; index < 4; ++index) {
            ranges[index].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[index].NumDescriptors = 1;
            ranges[index].BaseShaderRegister = index; // t0 material, t1..t3 meshlet data
            ranges[index].OffsetInDescriptorsFromTableStart = index;
        }
        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].Constants.Num32BitValues = 56;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[1].Descriptor.ShaderRegister = 1;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[2].Descriptor.ShaderRegister = 2;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[3].DescriptorTable.NumDescriptorRanges = 4;
        parameters[3].DescriptorTable.pDescriptorRanges = ranges;
        parameters[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MinLOD = 0;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.MaxAnisotropy = 1;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = 4;
        description.pParameters = parameters;
        description.NumStaticSamplers = 1;
        description.pStaticSamplers = &sampler;
        description.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ID3DBlob* signature = nullptr;
        ID3DBlob* diagnostics = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &signature, &diagnostics);
        if (FAILED(hr)) {
            error = "Serialize mesh root signature failed (" + detail::hresultText(hr) + ")";
            if (diagnostics) {
                error += ": " + std::string(static_cast<char*>(diagnostics->GetBufferPointer()),
                                            diagnostics->GetBufferSize());
            }
            detail::release(diagnostics);
            detail::release(signature);
            addDiagnostic("NG-E-MESH-PSO", error);
            return false;
        }
        detail::release(diagnostics);
        hr = device->CreateRootSignature(0, signature->GetBufferPointer(),
                                         signature->GetBufferSize(),
                                         IID_PPV_ARGS(&meshRootSignature));
        detail::release(signature);
        if (FAILED(hr)) {
            error = "Create mesh root signature failed (" + detail::hresultText(hr) + ")";
            addDiagnostic("NG-E-MESH-PSO", error);
            return false;
        }
        track(meshRootSignature);
    }

    D3D12_SHADER_BYTECODE amplification{};
    amplification.pShaderBytecode = shaders::kMeshAmplification.data;
    amplification.BytecodeLength = shaders::kMeshAmplification.size;
    D3D12_SHADER_BYTECODE meshShader{};
    meshShader.pShaderBytecode = shaders::kMeshShader.data;
    meshShader.BytecodeLength = shaders::kMeshShader.size;
    D3D12_SHADER_BYTECODE pixelShader{};
    pixelShader.pShaderBytecode = shaders::kMeshPixel.data;
    pixelShader.BytecodeLength = shaders::kMeshPixel.size;
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.NumRenderTargets = 2;
    formats.RTFormats[0] = detail::SceneColorFormat;
    formats.RTFormats[1] = detail::SceneColorFormat;
    DXGI_SAMPLE_DESC sample{};
    sample.Count = 1;
    sample.Quality = 0;
    D3D12_DEPTH_STENCIL_DESC depth = depthEnabled();
    D3D12_RASTERIZER_DESC raster = solidRaster();
    D3D12_BLEND_DESC blend = opaqueBlend();
    UINT sampleMask = UINT_MAX;
    UINT nodeMask = 0;

    PipelineStream stream;
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, meshRootSignature);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, amplification);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, meshShader);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, pixelShader);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blend);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, raster);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, depth);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,
               detail::DepthDsvFormat);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, sample);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, sampleMask);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,
               D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    stream.add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, nodeMask);

    D3D12_PIPELINE_STATE_STREAM_DESC streamDescription{};
    streamDescription.SizeInBytes = stream.bytes.size();
    streamDescription.pPipelineStateSubobjectStream = stream.bytes.data();
    HRESULT hr = device2->CreatePipelineState(&streamDescription, IID_PPV_ARGS(&meshPipeline));
    if (FAILED(hr)) {
        error = "CreatePipelineState (mesh shader stream) failed (" +
                detail::hresultText(hr) + ")";
        addDiagnostic("NG-E-MESH-PSO", error);
        return false;
    }
    track(meshPipeline);
    meshPipelineCreated = true;
    meshError.clear();
    return true;
}

bool Renderer::Impl::renderMeshPath(ID3D12GraphicsCommandList4* list,
                                    const std::vector<DrawItem>& items,
                                    const Mat4& viewProjection, const float eye[3]) {
    if (!meshPipelineCreated || !commands6) return false;
    auto* list6 = commands6;
    if (meshCameraMapped) {
        MeshCameraConstants camera{};
        camera.eye[0] = eye[0];
        camera.eye[1] = eye[1];
        camera.eye[2] = eye[2];
        camera.eye[3] = 1.0f;
        camera.params[0] = static_cast<float>(meshletCount);
        std::memcpy(meshCameraMapped, &camera, sizeof(camera));
    }
    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    list->SetDescriptorHeaps(1, heaps);
    list->SetPipelineState(meshPipeline);
    list->SetGraphicsRootSignature(meshRootSignature);
    list->SetGraphicsRootDescriptorTable(3, gpuDescriptor(detail::SlotMaterialTable));
    list->SetGraphicsRootConstantBufferView(1,
                                           sceneConstantBuffer->GetGPUVirtualAddress());
    list->SetGraphicsRootConstantBufferView(2, meshCameraBuffer->GetGPUVirtualAddress());
    unsigned drawn = 0;
    unsigned culled = 0;
    const float axes[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    for (const auto& item : items) {
        detail::DrawConstants constants{};
        constants.model = item.model;
        constants.viewProjection = viewProjection;
        constants.normalMatrix = normalMatrix(item.model);
        std::memcpy(constants.color, item.color, sizeof(constants.color));
        constants.roughness = item.roughness;
        constants.metalness = item.metalness;
        constants.textureIndex = item.texture;
        list->SetGraphicsRoot32BitConstants(0, 56, &constants, 0);
        // One amplification group per instance; it culls the six face meshlets and
        // DispatchMesh launches one group per visible meshlet.
        list6->DispatchMesh(1, 1, 1);
        ++drawn;
        // Amplification culling is disabled, so every meshlet of every instance is
        // forwarded. The counters reflect what the GPU actually dispatches.
        (void)axes;
        (void)culled;
    }
    meshletDrawnLastFrame = drawn * meshletCount;
    meshletCulledLastFrame = 0;
    return true;
}

std::string Renderer::Impl::meshShaderJson() const {
    std::ostringstream out;
    out << "{"
        << "\"available\":" << (meshShaderTier >= 10 ? "true" : "false")
        << ",\"tier\":" << meshShaderTier
        << ",\"dxcAvailable\":" << (shaders::kDxcAvailable ? "true" : "false")
        << ",\"requested\":" << (meshRequested ? "true" : "false")
        << ",\"implemented\":" << (meshPipelineCreated ? "true" : "false")
        << ",\"fallbackUsed\":" << (meshPathUsedLastFrame ? "false" : "true")
        << ",\"pipelineStateType\":\"stream\""
        << ",\"stages\":[\"amplification\",\"mesh\",\"pixel\"]"
        << ",\"meshlets\":" << meshletCount
        << ",\"instances\":" << drawCallsLastFrame
        << ",\"meshletInstancesDrawn\":" << meshletDrawnLastFrame
        << ",\"meshletsCulled\":" << meshletCulledLastFrame
        << ",\"amplificationCulling\":false"
        << ",\"frames\":" << meshFrames
        << ",\"fallbackFrames\":" << meshFallbackFrames
        << ",\"error\":" << detail::jsonString(meshError)
        << "}";
    return out.str();
}

} // namespace nativegpu
