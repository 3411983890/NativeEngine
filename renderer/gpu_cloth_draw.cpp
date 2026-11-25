#include "gpu_cloth_draw.hpp"
#include "nativegpu_shaders.h"
#include <cmath>
#include <cstring>

namespace nativegpu {
namespace {
template<class T>struct Com{T* p=nullptr;~Com(){if(p)p->Release();}T** out(){return &p;}};
void check(HRESULT code,const char* why){if(FAILED(code))throw engine::cloth::Error("NG-E-CLOTH-DRAW",why);}
bool finite(engine::Vec3 v,float bound){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z)&&std::fabs(v.x)<=bound&&std::fabs(v.y)<=bound&&std::fabs(v.z)<=bound;}
bool range(float v,float low,float high){return std::isfinite(v)&&v>=low&&v<=high;}
struct Constants {
    Mat4 vp{};float cameraWarp[4]{},baseSheen[4]{},keyIntensity[4]{},keyWeft[4]{},ambientWeave[4]{};
    unsigned columns=0,rows=0,mode=0,reserved=0;float repeat[4]{};
};
static_assert(sizeof(Constants)==176,"Cloth draw constant ABI changed");
}
bool validateClothSurface(const ClothSurface& s){
    for(auto v:s.albedo)if(!range(v,0,1))return false;
    for(auto v:s.weaveRepeat)if(!range(v,.01f,4096))return false;
    return range(s.roughnessWarp,.05f,1)&&range(s.roughnessWeft,.05f,1)&&range(s.sheen,0,1)&&range(s.weaveStrength,0,.3f)&&s.debugMode<=3;
}
struct GpuClothDraw::Impl {
    Com<ID3D12Device> device;Com<ID3D12Fence> timeline;Com<ID3D12RootSignature> root;Com<ID3D12PipelineState> pipeline;ClothDrawStats stats;
    Impl(ID3D12Device* d,ID3D12Fence* f,DXGI_FORMAT color,DXGI_FORMAT key,DXGI_FORMAT depth){
        device.p=d;d->AddRef();timeline.p=f;f->AddRef();
        D3D12_ROOT_PARAMETER parameters[3]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.Num32BitValues=44;
        for(unsigned i=0;i<2;++i){parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[i+1].Descriptor.ShaderRegister=i;parameters[i+1].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;}
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=3;signature.pParameters=parameters;
        Com<ID3DBlob> bytes,diagnostics;check(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,bytes.out(),diagnostics.out()),"Serialize cloth draw root");
        check(d->CreateRootSignature(0,bytes.p->GetBufferPointer(),bytes.p->GetBufferSize(),IID_PPV_ARGS(root.out())),"Create cloth draw root");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=root.p;p.VS={shaders::kClothDrawVS.data,shaders::kClothDrawVS.size};p.PS={shaders::kClothDrawPS.data,shaders::kClothDrawPS.size};
        p.SampleMask=UINT_MAX;p.SampleDesc.Count=1;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
        p.DepthStencilState.DepthEnable=TRUE;p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        p.DSVFormat=depth;p.NumRenderTargets=2;p.RTVFormats[0]=color;p.RTVFormats[1]=key;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        check(d->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(pipeline.out())),"Create cloth draw pipeline");
    }
};
GpuClothDraw::GpuClothDraw(ID3D12Device* device,ID3D12Fence* timeline,DXGI_FORMAT surface,DXGI_FORMAT key,DXGI_FORMAT depth){
    if(!device||!timeline||!shaders::kDxcAvailable)throw engine::cloth::Error("NG-E-CLOTH-DRAW","Missing cloth draw device, fence or shader");
    Com<ID3D12Device> owner;check(timeline->GetDevice(IID_PPV_ARGS(owner.out())),"Query cloth draw fence");
    if(owner.p!=device)throw engine::cloth::Error("NG-E-CLOTH-DRAW-FENCE","Cloth draw fence belongs to another device");
    impl_=std::make_unique<Impl>(device,timeline,surface,key,depth);
}
GpuClothDraw::~GpuClothDraw()=default;
bool GpuClothDraw::draw(ID3D12GraphicsCommandList* list,std::uint64_t fence,engine::cloth::GpuSolver& solver,const Mat4& vp,const ClothSurface& surface,const ClothLighting& lighting,std::string& error){
    error.clear();if(!impl_||!list||list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT||!validateClothSurface(surface)||!finite(lighting.camera,1e6f)||!finite(lighting.keyDirection,1)||!range(lighting.keyIntensity,0,100)){
        error="NG-E-CLOTH-DRAW-STATE: invalid draw input";return false;
    }
    auto& s=*impl_;Com<ID3D12Device> owner;if(FAILED(list->GetDevice(IID_PPV_ARGS(owner.out())))||owner.p!=s.device.p||fence<=s.timeline.p->GetCompletedValue()||fence<s.stats.lastFence){error="NG-E-CLOTH-DRAW-FENCE: invalid command device or timeline";return false;}
    for(float value:vp.m)if(!std::isfinite(value)){error="NG-E-CLOTH-DRAW-STATE: invalid view projection";return false;}
    // Match the scene's linear RGB/intensity contract and keep the shared FP16
    // MRT finite; radiance is not an unbounded arbitrary caller value.
    for(unsigned i=0;i<3;++i)if(!range(lighting.ambient[i],0,1)||!range(lighting.keyColor[i],0,1)){error="NG-E-CLOTH-DRAW-STATE: invalid lighting";return false;}
    const float length=std::sqrt(lighting.keyDirection.x*lighting.keyDirection.x+lighting.keyDirection.y*lighting.keyDirection.y+lighting.keyDirection.z*lighting.keyDirection.z);
    if(length<1e-6f){error="NG-E-CLOTH-DRAW-STATE: zero key direction";return false;}
    if(solver.device()!=s.device.p||solver.timeline()!=s.timeline.p){error="NG-E-CLOTH-DRAW-FENCE: simulation and drawing require the same actual timeline";return false;}
    // prepareRead also registers this actual draw use on the simulation owner.
    if(!solver.prepareRead(list,fence,error))return false;
    const auto buffers=solver.buffers();const auto dimensions=solver.gridDimensions();Constants c;c.vp=vp;c.columns=dimensions[0];c.rows=dimensions[1];c.mode=surface.debugMode;
    c.cameraWarp[0]=lighting.camera.x;c.cameraWarp[1]=lighting.camera.y;c.cameraWarp[2]=lighting.camera.z;c.cameraWarp[3]=surface.roughnessWarp;
    c.baseSheen[3]=surface.sheen;c.keyIntensity[0]=lighting.keyDirection.x/length;c.keyIntensity[1]=lighting.keyDirection.y/length;c.keyIntensity[2]=lighting.keyDirection.z/length;c.keyIntensity[3]=lighting.keyIntensity;
    c.keyWeft[3]=surface.roughnessWeft;c.ambientWeave[3]=surface.weaveStrength;
    for(unsigned i=0;i<3;++i){c.baseSheen[i]=surface.albedo[i];c.keyWeft[i]=lighting.keyColor[i];c.ambientWeave[i]=lighting.ambient[i];}c.repeat[0]=surface.weaveRepeat[0];c.repeat[1]=surface.weaveRepeat[1];
    list->SetGraphicsRootSignature(s.root.p);list->SetPipelineState(s.pipeline.p);list->SetGraphicsRoot32BitConstants(0,44,&c,0);
    list->SetGraphicsRootShaderResourceView(1,buffers.vertices->GetGPUVirtualAddress());list->SetGraphicsRootShaderResourceView(2,buffers.membranes->GetGPUVirtualAddress());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(buffers.triangleCount*3,1,0,0);
    ++s.stats.draws;s.stats.triangles+=buffers.triangleCount;s.stats.lastFence=fence;return true;
}
ClothDrawStats GpuClothDraw::stats() const{return impl_?impl_->stats:ClothDrawStats{};}
bool GpuClothDraw::shutdown(std::string& error){error.clear();if(!impl_)return true;if(impl_->timeline.p->GetCompletedValue()<impl_->stats.lastFence){error="NG-E-CLOTH-DRAW-FENCE: draw pipeline is in use";return false;}impl_.reset();return true;}
}
