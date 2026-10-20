// Standalone probe for the D3D12 texture -> readback path used by screenshots.
// Clears a 64x64 render target to a known colour, copies it to a READBACK buffer and
// verifies the pixels. Exits 0 on success, 2 on a device problem, 1 on a pixel mismatch.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

template<class T> void release(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}

int fail(const char* what, HRESULT hr) {
    std::printf("FAIL %s (0x%08lX)\n", what, static_cast<unsigned long>(hr));
    return 1;
}

} // namespace

int main() {
    IDXGIFactory4* factory = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Resource* texture = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    int result = 1;

    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return fail("factory", E_FAIL);
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        if (SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
            break;
        release(adapter);
    }
    if (!device) return fail("device", E_FAIL);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) return fail("queue", E_FAIL);
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator)))) return fail("allocator", E_FAIL);
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                         IID_PPV_ARGS(&list)))) return fail("list", E_FAIL);
    list->Close();

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 64;
    desc.Height = 64;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                               IID_PPV_ARGS(&texture)))) return fail("texture", E_FAIL);

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.NumDescriptors = 1;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)))) return fail("rtv heap", E_FAIL);
    const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(texture, nullptr, rtv);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    readbackHeap.CreationNodeMask = 1;
    readbackHeap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = totalBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&readback)))) return fail("readback", E_FAIL);

    allocator->Reset();
    list->Reset(allocator, nullptr);
    const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    list->ClearRenderTargetView(rtv, color, 0, nullptr);
    D3D12_RESOURCE_BARRIER toSource{};
    toSource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSource.Transition.pResource = texture;
    toSource.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toSource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toSource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &toSource);

    D3D12_TEXTURE_COPY_LOCATION target{};
    target.pResource = readback;
    target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    target.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    if (FAILED(list->Close())) return fail("close", E_FAIL);

    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return fail("fence", E_FAIL);
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, event);
    WaitForSingleObject(event, 5000);

    void* mapped = nullptr;
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(totalBytes)};
    if (FAILED(readback->Map(0, &readRange, &mapped))) return fail("map", E_FAIL);
    const auto* pixels = static_cast<const unsigned char*>(mapped) + footprint.Offset;
    const auto rowPitch = static_cast<std::size_t>(footprint.Footprint.RowPitch);
    std::printf("rowPitch=%zu totalBytes=%llu\n", rowPitch,
                static_cast<unsigned long long>(totalBytes));
    unsigned long long sums[3] = {0, 0, 0};
    for (int y = 0; y < 64; ++y) {
        const unsigned char* line = pixels + static_cast<std::size_t>(y) * rowPitch;
        for (int x = 0; x < 64; ++x) {
            sums[0] += line[x * 4 + 0];
            sums[1] += line[x * 4 + 1];
            sums[2] += line[x * 4 + 2];
        }
    }
    const double count = 64.0 * 64.0;
    const double r = sums[0] / count, g = sums[1] / count, b = sums[2] / count;
    std::printf("mean rgb = %.1f %.1f %.1f (expected 63.8 127.5 191.3)\n", r, g, b);
    if (r > 60.0 && r < 68.0 && g > 124.0 && g < 131.0 && b > 188.0 && b < 195.0) {
        std::printf("readback probe: PASS\n");
        result = 0;
    } else {
        std::printf("readback probe: FAIL (pixels did not match the cleared colour)\n");
        result = 1;
    }
    D3D12_RANGE noWrite{0, 0};
    readback->Unmap(0, &noWrite);

    if (event) CloseHandle(event);
    release(fence); release(rtvHeap); release(readback); release(texture);
    release(list); release(allocator); release(queue); release(device); release(adapter);
    release(factory);
    return result;
}
