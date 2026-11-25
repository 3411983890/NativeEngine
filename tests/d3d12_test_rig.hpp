#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace gpu_test {
inline void hr(HRESULT value,const char* why){if(FAILED(value))throw std::runtime_error(std::string(why)+" HRESULT="+std::to_string(static_cast<unsigned>(value)));}
template<class T>struct Com {T* p=nullptr;Com()=default;Com(const Com&)=delete;~Com(){if(p)p->Release();}T** out(){return &p;}};
inline std::string name(const wchar_t* value){int count=static_cast<int>(std::wcslen(value)),bytes=WideCharToMultiByte(CP_UTF8,0,value,count,nullptr,0,nullptr,nullptr);std::string out(bytes,'\0');WideCharToMultiByte(CP_UTF8,0,value,count,out.data(),bytes,nullptr,nullptr);return out;}
inline void makeBuffer(ID3D12Device* device,Com<ID3D12Resource>& out,std::size_t bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES h{};h.Type=heap;h.CreationNodeMask=h.VisibleNodeMask=1;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(out.out())),"Create test buffer");
}
inline void transition(ID3D12GraphicsCommandList* commands,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resource;b.Transition.StateBefore=before;b.Transition.StateAfter=after;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;commands->ResourceBarrier(1,&b);}
struct Rig {
    Com<IDXGIFactory6> factory;Com<ID3D12Device> device;Com<ID3D12CommandQueue> queue;Com<ID3D12CommandAllocator> allocator;
    Com<ID3D12GraphicsCommandList> commands;Com<ID3D12Fence> fence;Com<ID3D12InfoQueue> info;
    HANDLE event=nullptr;std::uint64_t submitted=0;std::string adapter;bool debug=false;
    explicit Rig(const std::string& preferred={}) {
        {Com<ID3D12Debug> layer;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(layer.out())))){layer.p->EnableDebugLayer();debug=true;}}
        hr(CreateDXGIFactory2(0,IID_PPV_ARGS(factory.out())),"Create test factory");
        for(unsigned i=0;;++i) {Com<IDXGIAdapter1> candidate;auto status=factory.p->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(candidate.out()));if(status==DXGI_ERROR_NOT_FOUND)break;hr(status,"Enumerate test GPU");
            DXGI_ADAPTER_DESC1 d{};candidate.p->GetDesc1(&d);auto label=name(d.Description);if((d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)||(!preferred.empty()&&label.find(preferred)==std::string::npos))continue;
            if(SUCCEEDED(D3D12CreateDevice(candidate.p,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(device.out())))){adapter=label;break;}
        }
        if(!device.p)throw std::runtime_error("Requested hardware GPU unavailable");
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};hr(device.p->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model)),"Query shader model");if(model.HighestShaderModel<D3D_SHADER_MODEL_6_6)throw std::runtime_error("SM 6.6 unavailable");
        device.p->QueryInterface(IID_PPV_ARGS(info.out()));
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;hr(device.p->CreateCommandQueue(&q,IID_PPV_ARGS(queue.out())),"Create test queue");
        hr(device.p->CreateCommandAllocator(q.Type,IID_PPV_ARGS(allocator.out())),"Create allocator");hr(device.p->CreateCommandList(0,q.Type,allocator.p,nullptr,IID_PPV_ARGS(commands.out())),"Create commands");hr(commands.p->Close(),"Close initial commands");
        hr(device.p->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.out())),"Create fence");event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)throw std::runtime_error("Create fence event failed");
    }
    ~Rig(){if(event)CloseHandle(event);}
    std::uint64_t ticket() const{return submitted+1;}
    void begin(){hr(allocator.p->Reset(),"Reset allocator");hr(commands.p->Reset(allocator.p,nullptr),"Reset commands");}
    void wait(){if(fence.p->GetCompletedValue()<submitted){hr(fence.p->SetEventOnCompletion(submitted,event),"Set fence event");if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU wait timed out");}if(fence.p->GetCompletedValue()<submitted)throw std::runtime_error("GPU fence incomplete");}
    void submit(bool waitNow=true){hr(commands.p->Close(),"Close commands");ID3D12CommandList* lists[]={commands.p};queue.p->ExecuteCommandLists(1,lists);hr(queue.p->Signal(fence.p,++submitted),"Signal queue");if(waitNow)wait();}
    unsigned debugErrors() const {
        unsigned errors=0;if(!info.p)return 0;
        for(UINT64 i=0;i<info.p->GetNumStoredMessages();++i){SIZE_T size=0;info.p->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());hr(info.p->GetMessage(i,m,&size),"Read debug message");if(m->Severity==D3D12_MESSAGE_SEVERITY_ERROR||m->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION){++errors;std::cerr<<m->pDescription<<'\n';}}
        return errors;
    }
};
}
