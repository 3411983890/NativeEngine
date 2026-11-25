// Bounded visual wave residual.
//
// The CPU shallow-water surface is the authority for height, depth and momentum. This
// pass adds the detail that surface cannot carry: it integrates a linear wave field on a
// finer grid, seeded one-shot by the same interaction events the CPU already applied.
// Nothing here is read back, so a GPU budget cut can only remove detail, never change
// the gameplay ledger.
#include "water_draw.hpp"
#include "nativegpu_shaders.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace nativegpu {
namespace {

constexpr unsigned kResolution = WaterRippleResolution;
constexpr unsigned kMaxEventsPerDispatch = 16;
constexpr unsigned kThreads = 8;
constexpr unsigned kFrameSlots = 3;
constexpr unsigned kMaxSubsteps = 6;
constexpr unsigned kConstantStride = 256; // D3D12 CBV alignment, one immutable slice per dispatch.
// Visual residual per metre of authoritative displacement. One step in this demo is
// 9 cm deep in the CPU solver; the surface mesh draws all of it, so the residual may
// only add the fine structure on top.
constexpr float ResidualGain = 0.35f;

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    // A ref-owning wrapper must never be copied: copying duplicates the pointer and
    // the temporary's destructor releases the only reference, leaving a dangling
    // resource that still looks valid to the next Create*View call.
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& other) noexcept : p(other.p) { other.p = nullptr; }
    Com& operator=(Com&& other) noexcept {
        if (this != &other) { reset(); p = other.p; other.p = nullptr; }
        return *this;
    }
    ~Com() { if (p) p->Release(); }
    T** out() { return &p; }
    void reset() { if (p) { p->Release(); p = nullptr; } }
};

void check(HRESULT hr, const char* label) {
    if (FAILED(hr)) {
        // Keep the stable code prefix and append the raw HRESULT: a resource-creation
        // failure needs the reason, not just the call site.
        char detail[24]{};
        std::snprintf(detail, sizeof(detail), " (0x%08X)", static_cast<unsigned>(hr));
        throw std::runtime_error(std::string("NG-E-WATER-RIPPLE: ") + label + detail);
    }
}

struct RippleConstants {
    float originExtent[4]{};
    float timing[4]{};
    float params[4]{};
    unsigned grid[4]{};
    unsigned surfaceGrid[4]{};
};
static_assert(sizeof(RippleConstants) == 80, "Water ripple constant ABI");

} // namespace

struct WaterRipple::Impl {
    Com<ID3D12Device> device;
    Com<ID3D12RootSignature> root;
    Com<ID3D12PipelineState> pipeline;
    Com<ID3D12DescriptorHeap> heap;
    // [0]=heightA [1]=velocityA [2]=heightB [3]=velocityB SRVs,
    // [4]=heightA [5]=velocityA [6]=heightB [7]=velocityB UAVs. Keeping each ping-pong
    // pair adjacent lets one table describe either parity.
    Com<ID3D12Resource> height[2], velocity[2], result;
    struct UploadFrame {
        Com<ID3D12Resource> constants, eventBuffer, depthBuffer;
        void* mappedConstants = nullptr;
        void* mappedEvents = nullptr;
        void* mappedDepth = nullptr;
        std::size_t depthCapacity = 0;
    } uploads[kFrameSlots];
    D3D12_RESOURCE_STATES heightState[2]{}, velocityState[2]{}, resultState{};
    bool ready = false;
    // Set until the first dispatch has written every texel; the compute stage needs it
    // because a fresh UAV texture has undefined contents.
    bool needsReset = true;
    unsigned parity = 0, resolution = kResolution;
    UINT stride = 0;
    std::uint64_t dispatches = 0, lastFrameTick = 0;
    // Delivery cursor is (timelineEpoch, sequence). A tick may contain several contacts;
    // a sequence is the identity that survives same-tick multiplicity and replay.
    std::uint32_t lastDeliveredSequence = 0, lastTimelineEpoch = 0;
    unsigned injectedTotal = 0;
    float lastTime = 0;

    explicit Impl(ID3D12Device* d) {
        device.p = d;
        d->AddRef();
        D3D12_ROOT_PARAMETER p[5]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].Descriptor.ShaderRegister = 0;
        D3D12_DESCRIPTOR_RANGE srvs{};
        srvs.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvs.NumDescriptors = 2;
        srvs.BaseShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges = &srvs;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[2].Descriptor.ShaderRegister = 2;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[3].Descriptor.ShaderRegister = 3;
        D3D12_DESCRIPTOR_RANGE uavs{};
        uavs.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavs.NumDescriptors = 2;
        uavs.BaseShaderRegister = 0;
        p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[4].DescriptorTable.NumDescriptorRanges = 1;
        p[4].DescriptorTable.pDescriptorRanges = &uavs;
        D3D12_ROOT_SIGNATURE_DESC sig{};
        sig.NumParameters = 5;
        sig.pParameters = p;
        Com<ID3DBlob> bytes, diagnostics;
        check(D3D12SerializeRootSignature(&sig, D3D_ROOT_SIGNATURE_VERSION_1, bytes.out(), diagnostics.out()),
              "serialize root");
        check(d->CreateRootSignature(0, bytes.p->GetBufferPointer(), bytes.p->GetBufferSize(),
                                     IID_PPV_ARGS(root.out())), "create root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
        ps.pRootSignature = root.p;
        ps.CS = {shaders::kWaterRippleCS.data, shaders::kWaterRippleCS.size};
        check(d->CreateComputePipelineState(&ps, IID_PPV_ARGS(pipeline.out())), "create pipeline");
        stride = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void upload(Com<ID3D12Resource>& resource, std::size_t size, void** mapped) {
        resource.reset();
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device.p->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(resource.out())), "create upload");
        D3D12_RANGE read{0, 0};
        check(resource.p->Map(0, &read, mapped), "map upload");
    }

    // Fills an existing owner instead of returning one: every resource here is
    // reference-counted, so ownership has to stay in exactly one place.
    void makeField(Com<ID3D12Resource>& resource, D3D12_RESOURCE_STATES& state) {
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = resolution;
        desc.Height = resolution;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device.p->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                IID_PPV_ARGS(resource.out())), "create field");
        state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    // The reset dispatch reads zero instead of undefined UAV contents, then injects
    // this frame's contacts. Later substeps read the initialized destination pair.
    void ensure() {
        if (ready) return;
        for (unsigned i = 0; i < 2; ++i) {
            makeField(height[i], heightState[i]);
            makeField(velocity[i], velocityState[i]);
        }
        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = resolution;
        desc.Height = resolution;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        check(device.p->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(result.out())), "create result");
        resultState = D3D12_RESOURCE_STATE_COPY_DEST;
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 8;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device.p->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(heap.out())), "create heap");
        auto cpu = heap.p->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_FLOAT;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device.p->CreateShaderResourceView(height[0].p, &view, cpu);
        cpu.ptr += stride;
        device.p->CreateShaderResourceView(velocity[0].p, &view, cpu);
        cpu.ptr += stride;
        device.p->CreateShaderResourceView(height[1].p, &view, cpu);
        cpu.ptr += stride;
        device.p->CreateShaderResourceView(velocity[1].p, &view, cpu);
        cpu.ptr += stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (unsigned i = 0; i < 2; ++i) {
            device.p->CreateUnorderedAccessView(height[i].p, nullptr, &uav, cpu);
            cpu.ptr += stride;
            device.p->CreateUnorderedAccessView(velocity[i].p, nullptr, &uav, cpu);
            cpu.ptr += stride;
        }
        for (auto& frame : uploads) {
            upload(frame.constants, kConstantStride * kMaxSubsteps, &frame.mappedConstants);
            upload(frame.eventBuffer, sizeof(WaterRippleEvent) * kMaxEventsPerDispatch, &frame.mappedEvents);
        }
        ready = true;
    }

    void toState(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                 D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES target) {
        if (current == target) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = current;
        barrier.Transition.StateAfter = target;
        list->ResourceBarrier(1, &barrier);
        current = target;
    }

    void release() {
        for (unsigned i = 0; i < 2; ++i) { height[i].reset(); velocity[i].reset(); }
        result.reset();
        heap.reset();
        ready = false;
        needsReset = true;
        parity = 0;
        lastDeliveredSequence = 0;
        lastTimelineEpoch = 0;
        lastFrameTick = 0;
        lastTime = 0;
        injectedTotal = 0;
    }
};

WaterRipple::WaterRipple(ID3D12Device* device) : impl_(std::make_unique<Impl>(device)) {}
WaterRipple::~WaterRipple() = default;
void WaterRipple::reset() { impl_->release(); }
ID3D12Resource* WaterRipple::result() const { return impl_->result.p; }
unsigned WaterRipple::resolution() const { return impl_->resolution; }
std::uint64_t WaterRipple::dispatches() const { return impl_->dispatches; }

bool WaterRipple::update(ID3D12GraphicsCommandList* list, unsigned slot, const WaterSurfaceFrame& frame,
                         unsigned& injected, std::string& error) {
    injected = 0;
    if (!validateWaterFrame(frame, error)) return false;
    if (frame.vertices.empty() || !list) return true;
    if (slot >= kFrameSlots) { error = "NG-E-WATER-RIPPLE: invalid frame slot"; return false; }
    try {
        auto& s = *impl_;
        s.ensure();
        auto& upload = s.uploads[slot]; // The renderer has waited for this slot's fence.
        // A reset/teleport changes the timeline epoch, and a rewound tick alone can also
        // replay numbers. Either way the old field and its delivery cursor belong to a
        // discarded timeline and must not mask the new contacts as duplicates.
        if (frame.timelineEpoch != s.lastTimelineEpoch) {
            s.lastDeliveredSequence = 0;
            s.lastTimelineEpoch = frame.timelineEpoch;
            s.needsReset = true;
            s.lastTime = frame.time;
        }
        if (frame.tick < s.lastFrameTick) {
            s.lastDeliveredSequence = 0;
            s.needsReset = true;
            s.lastTime = frame.time;
        }
        s.lastFrameTick = frame.tick;
        WaterRippleEvent pending[kMaxEventsPerDispatch]{};
        unsigned count = 0;
        // Advance the cursor only past events actually copied. A batch larger than the
        // per-dispatch capacity is delivered over the following frames instead of being
        // silently skipped, which is what a "latest tick wins" cursor used to do.
        std::uint32_t newest = s.lastDeliveredSequence;
        for (const auto& event : frame.events) {
            if (event.timelineEpoch != s.lastTimelineEpoch) continue;
            if (event.sequence <= s.lastDeliveredSequence) continue;
            if (count == kMaxEventsPerDispatch) break;
            pending[count] = event;
            // The rendered mesh already carries the authoritative displacement, so the
            // residual only contributes the detail the solver cannot express. Injecting
            // the full displacement a second time would double-count the same water and
            // drive every step straight into the visual height limit.
            pending[count].displacement *= ResidualGain;
            ++count;
            newest = event.sequence;
        }
        if (count) {
            std::memcpy(upload.mappedEvents, pending, sizeof(WaterRippleEvent) * count);
            s.lastDeliveredSequence = newest;
        }
        injected = count;
        s.injectedTotal += count;

        // The compute stage needs depth (gate + wave speed) and the horizontal velocity
        // snapshot (advection). One float4 per vertex keeps the whole input in one probe.
        const std::size_t fieldCount = std::size_t(frame.columns) * frame.rows;
        const std::size_t fieldBytes = fieldCount * sizeof(float) * 4;
        if (upload.depthCapacity < fieldBytes) {
            s.upload(upload.depthBuffer, fieldBytes, &upload.mappedDepth);
            upload.depthCapacity = fieldBytes;
        }
        auto* surfaceField = static_cast<float*>(upload.mappedDepth);
        for (std::size_t i = 0; i < fieldCount; ++i) {
            const auto& vertex = frame.vertices[i];
            surfaceField[i * 4 + 0] = vertex.depth;
            surfaceField[i * 4 + 1] = vertex.velocity[0];
            surfaceField[i * 4 + 2] = vertex.velocity[1];
            surfaceField[i * 4 + 3] = vertex.foam;
        }

        RippleConstants constants{};
        constants.originExtent[0] = frame.rippleOrigin[0];
        constants.originExtent[1] = frame.rippleOrigin[1];
        constants.originExtent[2] = std::max(frame.rippleExtent[0], 1e-3f);
        constants.originExtent[3] = std::max(frame.rippleExtent[1], 1e-3f);
        const float cell = std::min(constants.originExtent[2], constants.originExtent[3]) / float(s.resolution);
        // The visual residual runs on render frames, not fixed ticks. A render frame may
        // be longer than a 120 Hz tick, so the step is bounded and the speed clamp inside
        // the shader keeps the explicit update inside its stability limit.
        const float dt = std::clamp(frame.time - s.lastTime, 1e-4f, 1.f / 30.f);
        s.lastTime = frame.time;
        constants.timing[0] = dt;
        constants.timing[1] = float(frame.tick);
        constants.timing[2] = frame.time;
        constants.timing[3] = cell;
        constants.params[0] = 9.81f;
        constants.params[1] = 1.2f;
        constants.params[2] = .03f;
        constants.params[3] = std::clamp(std::min(constants.originExtent[2], constants.originExtent[3]) * .05f, .05f, .5f);
        constants.grid[0] = s.resolution;
        constants.grid[1] = s.resolution;
        constants.grid[2] = 0;
        // Only the first substep ignores undefined/previous-timeline contents.
        constants.grid[3] = s.needsReset ? 1u : 0u;
        constants.surfaceGrid[0] = frame.columns;
        constants.surfaceGrid[1] = frame.rows;

        // A 60 Hz frame is longer than the explicit stability limit of a 3 cm grid, so
        // the field is substepped instead of clamping the wave speed down to whatever
        // one frame allows. The clamp inside the shader stays as a last-resort guard.
        float depth = .02f;
        for (const auto& vertex : frame.vertices) depth = std::max(depth, vertex.depth);
        const float speed = std::sqrt(9.81f * depth);
        // 0.35 instead of the theoretical 0.7: the 2D five-point Laplacian is only
        // marginally stable near its limit, and a marginal step leaves grid-scale
        // checkerboard noise that the injection keeps feeding. The extra substep is
        // far cheaper than a field that has to be hidden behind an amplitude clamp.
        const float stable = .35f * cell / std::max(speed, 1e-3f);
        const unsigned steps = std::clamp(static_cast<unsigned>(std::ceil(dt / stable)), 1u, kMaxSubsteps);
        const float step = dt / float(steps);

        ID3D12DescriptorHeap* heaps[]{s.heap.p};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(s.root.p);
        list->SetPipelineState(s.pipeline.p);
        list->SetComputeRootShaderResourceView(2, upload.depthBuffer.p->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(3, upload.eventBuffer.p->GetGPUVirtualAddress());
        const unsigned groups = (s.resolution + kThreads - 1) / kThreads;
        for (unsigned substep = 0; substep < steps; ++substep) {
            constants.timing[0] = step;
            constants.grid[2] = substep == 0 ? count : 0;
            constants.grid[3] = s.needsReset && substep == 0 ? 1u : 0u;
            const unsigned offset = substep * kConstantStride;
            std::memcpy(static_cast<char*>(upload.mappedConstants) + offset, &constants, sizeof(constants));
            const unsigned previous = s.parity, next = s.parity ^ 1u;
            s.toState(list, s.height[previous].p, s.heightState[previous], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            s.toState(list, s.velocity[previous].p, s.velocityState[previous], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->SetComputeRootConstantBufferView(0, upload.constants.p->GetGPUVirtualAddress() + offset);
            auto gpu = s.heap.p->GetGPUDescriptorHandleForHeapStart();
            auto srv = gpu;
            srv.ptr += static_cast<SIZE_T>(previous) * 2 * s.stride;
            list->SetComputeRootDescriptorTable(1, srv);
            auto uav = gpu;
            uav.ptr += static_cast<SIZE_T>(4 + next * 2) * s.stride;
            list->SetComputeRootDescriptorTable(4, uav);
            list->Dispatch(groups, groups, 1);
            // The pair just written becomes the source of the next substep and the
            // pair just read becomes its destination.
            s.toState(list, s.height[next].p, s.heightState[next], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            s.toState(list, s.velocity[next].p, s.velocityState[next], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            s.toState(list, s.height[previous].p, s.heightState[previous], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            s.toState(list, s.velocity[previous].p, s.velocityState[previous], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            s.parity = next;
        }

        // Publish a stable read target so the draw pass binds one descriptor for the
        // whole frame slot instead of following the ping-pong parity.
        const unsigned written = s.parity;
        s.needsReset = false;
        s.toState(list, s.height[written].p, s.heightState[written], D3D12_RESOURCE_STATE_COPY_SOURCE);
        s.toState(list, s.result.p, s.resultState, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(s.result.p, s.height[written].p);
        s.toState(list, s.result.p, s.resultState,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        s.toState(list, s.height[written].p, s.heightState[written], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        ++s.dispatches;
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

} // namespace nativegpu
