#include "water_draw.hpp"
#include "nativegpu_shaders.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace nativegpu {
namespace {
template<class T>struct Com{T* p=nullptr;~Com(){if(p)p->Release();}T** out(){return &p;}};
void check(HRESULT hr,const char* label){if(FAILED(hr))throw std::runtime_error(std::string("NG-E-WATER-DRAW: ")+label);}
bool bounded(float v,float low,float high){return std::isfinite(v)&&v>=low&&v<=high;}
struct Constants {
    Mat4 vp{},inverse{};
    float cameraTime[4]{},keyDirectionIntensity[4]{},keyColorRoughness[4]{},skyIor[4]{},absorption[4]{},scattering[4]{};
    unsigned grid[4]{};
    float viewport[4]{};
    float rippleOriginExtent[4]{},rippleGrid[4]{},rippleScale[4]{};
};
static_assert(sizeof(Constants)==304,"Water constant ABI");
}
bool validateWaterFrame(const WaterSurfaceFrame& f,std::string& error){
    error.clear();if(f.vertices.empty())return true;
    if(f.columns<2||f.rows<2||f.columns>257||f.rows>257||f.vertices.size()!=std::size_t(f.columns)*f.rows||
       !bounded(f.time,0,1.e9f)||!bounded(f.roughness,.025f,1)||!bounded(f.ior,1.01f,2)||f.debugMode>7){error="NG-E-WATER-FRAME: dimensions or optical parameters";return false;}
    for(unsigned i=0;i<3;++i)if(!bounded(f.absorption[i],0,100)||!bounded(f.scattering[i],0,1)){error="NG-E-WATER-FRAME: medium coefficients";return false;}
    for(unsigned i=0;i<2;++i)if(!bounded(f.rippleOrigin[i],-1.e6f,1.e6f)||!bounded(f.rippleExtent[i],1.e-3f,1.e4f)){error="NG-E-WATER-FRAME: residual footprint";return false;}
    // The event list is a bounded visual feed, not a ledger: an oversized frame is
    // rejected instead of silently truncated so a caller cannot believe it delivered
    // interactions that the solver never saw. The bound matches the CPU history ring;
    // the residual delivers at most 16 per dispatch and keeps the rest for next frame.
    if(f.events.size()>128){error="NG-E-WATER-FRAME: residual event bound";return false;}
    std::uint32_t previousSequence=0;bool first=true;
    for(const auto& e:f.events){
        if(!bounded(e.x,-1.e6f,1.e6f)||!bounded(e.z,-1.e6f,1.e6f)||!bounded(e.radius,.01f,8)||
           !bounded(e.displacement,-.5f,.5f)||e.tick>1000000000u||
           !bounded(e.directionX,-1,1)||!bounded(e.directionZ,-1,1)||!bounded(e.stretch,.5f,1)||
           !bounded(e.normalSpeed,0,50)){
            error="NG-E-WATER-FRAME: residual event";return false;}
        if(e.timelineEpoch!=f.timelineEpoch){error="NG-E-WATER-FRAME: residual event epoch";return false;}
        // Sequence is the identity; a history has to be strictly ordered, otherwise two
        // same-tick contacts could be delivered out of order or dropped as duplicates.
        if(!first&&e.sequence<=previousSequence){error="NG-E-WATER-FRAME: residual event order";return false;}
        previousSequence=e.sequence;first=false;
    }
    for(const auto& v:f.vertices){
        for(float p:v.position)if(!bounded(p,-1.e6f,1.e6f)){error="NG-E-WATER-FRAME: nonfinite position";return false;}
        float n=0;for(float x:v.normal){if(!std::isfinite(x)){error="NG-E-WATER-FRAME: nonfinite normal";return false;}n+=x*x;}
        if(n<.9f||n>1.1f||!bounded(v.depth,0,200)||!bounded(v.foam,0,1)){error="NG-E-WATER-FRAME: normal, depth or foam";return false;}
        if(!bounded(v.velocity[0],-50,50)||!bounded(v.velocity[1],-50,50)){error="NG-E-WATER-FRAME: nonfinite velocity";return false;}
    }return true;
}
struct WaterDraw::Impl {
    Com<ID3D12Device> device;Com<ID3D12RootSignature> root;Com<ID3D12PipelineState> pipeline;
    struct Slot {
        Com<ID3D12Resource> snapshots[3],vertices,constants;
        Com<ID3D12DescriptorHeap> heap;
        unsigned width=0,height=0;std::size_t capacity=0;WaterDrawStats stats;
        void* mappedVertices=nullptr;void* mappedConstants=nullptr;
        ID3D12Resource* boundRipple=nullptr;
    } slots[3];
    WaterDrawStats stats;
    explicit Impl(ID3D12Device* d){
        device.p=d;d->AddRef();
        D3D12_DESCRIPTOR_RANGE range[2]{};range[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range[0].NumDescriptors=4;range[0].BaseShaderRegister=0;
        // The residual field is a separate range because the vertex stream owns t4.
        range[1].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range[1].NumDescriptors=1;range[1].BaseShaderRegister=5;
        // A zero-initialized offset aliases t0: HDR colour would become metres of
        // displacement. t4 is a root SRV, so t5 lives at heap descriptor 4.
        range[1].OffsetInDescriptorsFromTableStart=4;
        D3D12_ROOT_PARAMETER p[3]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;p[0].Descriptor.ShaderRegister=0;
        p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[1].DescriptorTable.NumDescriptorRanges=2;p[1].DescriptorTable.pDescriptorRanges=range;p[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[2].Descriptor.ShaderRegister=4;p[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC sig{};sig.NumParameters=3;sig.pParameters=p;sig.NumStaticSamplers=1;sig.pStaticSamplers=&sampler;
        Com<ID3DBlob> bytes,diagnostics;check(D3D12SerializeRootSignature(&sig,D3D_ROOT_SIGNATURE_VERSION_1,bytes.out(),diagnostics.out()),"serialize root");
        check(d->CreateRootSignature(0,bytes.p->GetBufferPointer(),bytes.p->GetBufferSize(),IID_PPV_ARGS(root.out())),"create root");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.p;ps.VS={shaders::kWaterVS.data,shaders::kWaterVS.size};ps.PS={shaders::kWaterPS.data,shaders::kWaterPS.size};
        ps.SampleMask=UINT_MAX;ps.SampleDesc.Count=1;ps.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;ps.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;ps.RasterizerState.DepthClipEnable=TRUE;
        ps.DepthStencilState.DepthEnable=TRUE;ps.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;ps.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        ps.DSVFormat=DXGI_FORMAT_D32_FLOAT;ps.NumRenderTargets=2;ps.RTVFormats[0]=ps.RTVFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;
        ps.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        ps.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        check(d->CreateGraphicsPipelineState(&ps,IID_PPV_ARGS(pipeline.out())),"create pipeline");
    }
    void upload(Com<ID3D12Resource>& resource,std::size_t size,void** mapped){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=size;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device.p->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(resource.out())),"create upload");
        D3D12_RANGE read{0,0};check(resource.p->Map(0,&read,mapped),"map upload");
    }
};
WaterDraw::WaterDraw(ID3D12Device* device):impl_(std::make_unique<Impl>(device)){}
WaterDraw::~WaterDraw()=default;
bool WaterDraw::record(ID3D12GraphicsCommandList* list,unsigned slot,unsigned width,unsigned height,ID3D12Resource* base,ID3D12Resource* key,ID3D12Resource* depth,ID3D12Resource* shadow,ID3D12Resource* ripple,const WaterSurfaceFrame& frame,const Mat4& vp,const WaterLighting& light,std::string& error){
    if(!validateWaterFrame(frame,error))return false;if(frame.vertices.empty())return true;
    try{
        if(slot>=3||!list||!base||!key||!depth||!shadow||!width||!height)throw std::runtime_error("NG-E-WATER-DRAW: missing resources");
        auto& state=*impl_;auto& s=state.slots[slot];
        const auto bindRipple=[&](D3D12_CPU_DESCRIPTOR_HANDLE target,ID3D12Resource* resource){
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_FLOAT;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
            state.device.p->CreateShaderResourceView(resource,&view,target);s.boundRipple=resource;
        };
        if(s.width!=width||s.height!=height){
            // The outer renderer waits this frame slot and all frames on resize.
            for(auto& r:s.snapshots){if(r.p){r.p->Release();r.p=nullptr;}}
            if(s.heap.p){s.heap.p->Release();s.heap.p=nullptr;}
            D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=5;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            check(state.device.p->CreateDescriptorHeap(&heap,IID_PPV_ARGS(s.heap.out())),"create snapshot heap");
            ID3D12Resource* sources[]{base,key,depth};
            const unsigned stride=state.device.p->GetDescriptorHandleIncrementSize(heap.Type);
            auto cpu=s.heap.p->GetCPUDescriptorHandleForHeapStart();
            for(unsigned i=0;i<3;++i){
                auto desc=sources[i]->GetDesc();desc.Flags=D3D12_RESOURCE_FLAG_NONE;
                D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_DEFAULT;
                check(state.device.p->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(s.snapshots[i].out())),"create HDR/depth snapshot");
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=i==2?DXGI_FORMAT_R32_FLOAT:desc.Format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
                state.device.p->CreateShaderResourceView(s.snapshots[i].p,&view,cpu);cpu.ptr+=stride;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R16_FLOAT;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
            state.device.p->CreateShaderResourceView(shadow,&view,cpu);cpu.ptr+=stride;
            if(!ripple)throw std::runtime_error("NG-E-WATER-DRAW: missing residual field");
            bindRipple(cpu,ripple);
            s.width=width;s.height=height;
        }else if(ripple&&s.boundRipple!=ripple){
            // A device rebuild replaces the residual textures without resizing the
            // swap chain; only the last descriptor changes.
            const unsigned stride=state.device.p->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto cpu=s.heap.p->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=static_cast<SIZE_T>(4)*stride;
            bindRipple(cpu,ripple);
        }else{
            for(auto& r:s.snapshots){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={r.p,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST};list->ResourceBarrier(1,&barrier);}
        }
        ID3D12Resource* sources[]{base,key,depth};
        for(unsigned i=0;i<3;++i){list->CopyResource(s.snapshots[i].p,sources[i]);D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={s.snapshots[i].p,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};list->ResourceBarrier(1,&barrier);}
        // Sources must return to their output states before drawing. The caller
        // tracks these identical transitions in its resource state objects.
        for(unsigned i=0;i<3;++i){D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={sources[i],D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_SOURCE,i==2?D3D12_RESOURCE_STATE_DEPTH_WRITE:D3D12_RESOURCE_STATE_RENDER_TARGET};list->ResourceBarrier(1,&barrier);}
        const auto bytes=frame.vertices.size()*sizeof(WaterVertex);
        if(s.capacity<bytes){if(s.vertices.p){s.vertices.p->Release();s.vertices.p=nullptr;}state.upload(s.vertices,bytes,&s.mappedVertices);s.capacity=bytes;}
        if(!s.constants.p)state.upload(s.constants,sizeof(Constants),&s.mappedConstants);
        std::memcpy(s.mappedVertices,frame.vertices.data(),bytes);
        Constants c;c.vp=vp;if(!invertMatrix(vp,c.inverse))throw std::runtime_error("NG-E-WATER-DRAW: singular camera");
        for(unsigned i=0;i<3;++i){c.cameraTime[i]=light.camera[i];c.keyDirectionIntensity[i]=light.keyDirection[i];c.keyColorRoughness[i]=light.keyColor[i];c.skyIor[i]=light.sky[i];c.absorption[i]=frame.absorption[i];c.scattering[i]=frame.scattering[i];}
        c.cameraTime[3]=frame.time;c.keyDirectionIntensity[3]=light.keyIntensity;c.keyColorRoughness[3]=frame.roughness;c.skyIor[3]=frame.ior;
        c.grid[0]=frame.columns;c.grid[1]=frame.rows;c.grid[2]=frame.debugMode;c.viewport[0]=float(width);c.viewport[1]=float(height);
        c.rippleOriginExtent[0]=frame.rippleOrigin[0];c.rippleOriginExtent[1]=frame.rippleOrigin[1];
        c.rippleOriginExtent[2]=std::max(frame.rippleExtent[0],1.e-3f);c.rippleOriginExtent[3]=std::max(frame.rippleExtent[1],1.e-3f);
        c.rippleGrid[0]=float(WaterRippleResolution);c.rippleGrid[1]=float(WaterRippleResolution);
        c.rippleGrid[2]=c.rippleOriginExtent[2]/float(WaterRippleResolution);
        c.rippleGrid[3]=c.rippleOriginExtent[3]/float(WaterRippleResolution);
        // Uploads are fenced per frame and immutable per substep; the initialized,
        // one-shot residual can now contribute bounded contact detail.
        const bool residualAffectsSurface=ripple!=nullptr;
        c.rippleScale[0]=residualAffectsSurface?1.f:0.f;c.rippleScale[1]=residualAffectsSurface?1.f:0.f;
        c.rippleScale[2]=ripple?1:0;
        std::memcpy(s.mappedConstants,&c,sizeof(c));
        ID3D12DescriptorHeap* heaps[]{s.heap.p};list->SetDescriptorHeaps(1,heaps);list->SetGraphicsRootSignature(state.root.p);list->SetPipelineState(state.pipeline.p);
        list->SetGraphicsRootConstantBufferView(0,s.constants.p->GetGPUVirtualAddress());list->SetGraphicsRootDescriptorTable(1,s.heap.p->GetGPUDescriptorHandleForHeapStart());list->SetGraphicsRootShaderResourceView(2,s.vertices.p->GetGPUVirtualAddress());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced((frame.columns-1)*(frame.rows-1)*6,1,0,0);
        ++state.stats.frames;state.stats.triangles+=(frame.columns-1)*(frame.rows-1)*2;state.stats.uploadBytes+=bytes;return true;
    }catch(const std::exception& ex){error=ex.what();return false;}
}
WaterDrawStats WaterDraw::stats() const{return impl_->stats;}
}
