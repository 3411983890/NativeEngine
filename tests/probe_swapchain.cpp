// Probe: which swap-chain descriptions this adapter/driver accepts.
//
// Kept as a test artifact because "flip model rejects _SRGB" is a load-bearing fact for the
// viewport host: it decides whether colour correction is a format choice or a shader pass.
// Run: tools/viewport-host.ps1 -Probe (or build and run directly).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>

namespace {
template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** out() { if (p) { p->Release(); p = nullptr; } return &p; }
    T* operator->() const { return p; }
};
}

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"SwapChainProbe";
    RegisterClassExW(&cls);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 640, 400, nullptr, nullptr, cls.hInstance, nullptr);
    if (!window) { std::printf("window failed\n"); return 1; }

    Com<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(device.out())))) {
        std::printf("device failed\n");
        return 1;
    }
    Com<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    device.p->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(queue.out()));
    Com<IDXGIFactory2> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(factory.out()));

    struct Case { const char* name; DXGI_SWAP_EFFECT effect; UINT buffers; DXGI_FORMAT format; };
    const Case cases[] = {
        {"flip_discard 3 unorm", DXGI_SWAP_EFFECT_FLIP_DISCARD, 3, DXGI_FORMAT_B8G8R8A8_UNORM},
        {"flip_discard 3 srgb ", DXGI_SWAP_EFFECT_FLIP_DISCARD, 3, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB},
        {"flip_discard 2 unorm", DXGI_SWAP_EFFECT_FLIP_DISCARD, 2, DXGI_FORMAT_B8G8R8A8_UNORM},
        {"discard      1 unorm", DXGI_SWAP_EFFECT_DISCARD, 1, DXGI_FORMAT_B8G8R8A8_UNORM},
        {"discard      2 unorm", DXGI_SWAP_EFFECT_DISCARD, 2, DXGI_FORMAT_B8G8R8A8_UNORM},
        {"discard      1 srgb ", DXGI_SWAP_EFFECT_DISCARD, 1, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB},
        {"discard      2 srgb ", DXGI_SWAP_EFFECT_DISCARD, 2, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB},
        {"flip_discard 3 fp16 ", DXGI_SWAP_EFFECT_FLIP_DISCARD, 3, DXGI_FORMAT_R16G16B16A16_FLOAT},
    };
    for (const auto& item : cases) {
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = 640;
        description.Height = 400;
        description.Format = item.format;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = item.buffers;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = item.effect;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        Com<IDXGISwapChain1> swap;
        const HRESULT result = factory->CreateSwapChainForHwnd(queue.p, window, &description, nullptr, nullptr, swap.out());
        std::printf("%s -> 0x%08lx %s", item.name, static_cast<unsigned long>(result), SUCCEEDED(result) ? "ok" : "fail");
        if (SUCCEEDED(result)) {
            // A linear (scRGB) flip chain is the only way to get correct colour without a
            // gamma pass: the compositor converts G10 to the display's sRGB itself.
            Com<IDXGISwapChain3> swap3;
            if (SUCCEEDED(swap.p->QueryInterface(IID_PPV_ARGS(swap3.out())))) {
                const DXGI_COLOR_SPACE_TYPE spaces[] = {DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};
                for (const auto space : spaces) {
                    UINT support = 0;
                    const HRESULT query = swap3->CheckColorSpaceSupport(space, &support);
                    const bool supported = SUCCEEDED(query) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0;
                    std::printf("  space %d supported=%d", static_cast<int>(space), supported ? 1 : 0);
                    if (supported) {
                        const HRESULT set = swap3->SetColorSpace1(space);
                        std::printf(" set=0x%08lx", static_cast<unsigned long>(set));
                    }
                }
            }
        }
        std::printf("\n");
    }
    DestroyWindow(window);
    return 0;
}
