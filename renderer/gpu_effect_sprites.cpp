#include "gpu_effect_sprites.hpp"
#include "nativegpu_shaders.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace nativegpu {
namespace {
template<class T>struct Com {T* p=nullptr;Com()=default;Com(const Com&)=delete;~Com(){if(p)p->Release();}T** out(){return &p;}};
void check(HRESULT hr,const char* why){if(FAILED(hr))throw engine::effects::Error("NG-E-EFFECT-DRAW",why);}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=r;b.Transition.StateBefore=from;b.Transition.StateAfter=to;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;list->ResourceBarrier(1,&b);}
void barrier(ID3D12GraphicsCommandList* list){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;list->ResourceBarrier(1,&b);}
struct Constants {Mat4 vp;unsigned layout[4]{};float origin[3]{};unsigned local=0;unsigned capacity=0,texture=0,blend=0,ordinal=0,k=0,j=0,padding[2]{};float right[4]{},up[4]{};};
static_assert(sizeof(Constants)==160,"GPU sprite constants changed");
}
struct GpuEffectSprites::Impl {
    Com<ID3D12RootSignature> root;std::array<Com<ID3D12PipelineState>,7> compute;
    std::array<Com<ID3D12PipelineState>,3> graphics;Com<ID3D12CommandSignature> dispatchSignature,drawSignature;
    Com<ID3D12Resource> records,state,args,draws;GpuSpriteStats stats;Constants constants;unsigned capacity=0;
    bool ready=false,argsIndirect=false;
    Impl(ID3D12Device* device,DXGI_FORMAT color,DXGI_FORMAT light,DXGI_FORMAT depth,GpuSpriteLimits limits):capacity(limits.capacity){
        const std::uint64_t sizes[]={capacity*64ull,64,12,48};D3D12_RESOURCE_DESC descriptions[4]{};
        for(unsigned i=0;i<4;++i){auto& d=descriptions[i];d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=sizes[i];d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            auto bytes=device->GetResourceAllocationInfo(0,1,&d).SizeInBytes;if(bytes>limits.maxAllocationBytes||stats.allocationBytes>limits.maxAllocationBytes-bytes)throw engine::effects::Error("NG-E-EFFECT-DRAW-BUDGET","GPU sprite buffers exceed allocation budget");stats.allocationBytes+=bytes;}
        ID3D12Resource** outputs[]={records.out(),state.out(),args.out(),draws.out()};D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        for(unsigned i=0;i<4;++i)check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&descriptions[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(outputs[i])),"Create GPU sprite buffer");
        D3D12_ROOT_PARAMETER parameters[9]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.Num32BitValues=40;
        for(unsigned i=0;i<4;++i){parameters[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[i+1].Descriptor.ShaderRegister=i;parameters[i+5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[i+5].Descriptor.ShaderRegister=i;}
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.MaxAnisotropy=1;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=9;signature.pParameters=parameters;signature.NumStaticSamplers=1;signature.pStaticSamplers=&sampler;signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        Com<ID3DBlob> bytes,diagnostics;check(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,bytes.out(),diagnostics.out()),"Serialize GPU sprite root");check(device->CreateRootSignature(0,bytes.p->GetBufferPointer(),bytes.p->GetBufferSize(),IID_PPV_ARGS(root.out())),"Create GPU sprite root");
        const shaders::Blob* blobs[]={&shaders::kGpuSpriteReset,&shaders::kGpuSpritePrepareSource,&shaders::kGpuSpriteGather,&shaders::kGpuSpriteFinish,&shaders::kGpuSpritePad,&shaders::kGpuSpritePrepareSort,&shaders::kGpuSpriteSort};
        for(unsigned i=0;i<7;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC p{};p.pRootSignature=root.p;p.CS={blobs[i]->data,blobs[i]->size};check(device->CreateComputePipelineState(&p,IID_PPV_ARGS(compute[i].out())),"Create GPU sprite compute pipeline");}
        for(unsigned blend=0;blend<3;++blend){D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=root.p;p.VS={shaders::kGpuSpriteVS.data,shaders::kGpuSpriteVS.size};p.PS={shaders::kGpuSpritePS.data,shaders::kGpuSpritePS.size};p.SampleMask=UINT_MAX;p.SampleDesc.Count=1;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;p.DepthStencilState.DepthEnable=TRUE;p.DepthStencilState.DepthWriteMask=blend==2?D3D12_DEPTH_WRITE_MASK_ALL:D3D12_DEPTH_WRITE_MASK_ZERO;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;p.DSVFormat=depth;p.NumRenderTargets=2;p.RTVFormats[0]=color;p.RTVFormats[1]=light;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            auto& t=p.BlendState.RenderTarget[0];t.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;t.BlendEnable=blend!=2;t.SrcBlend=D3D12_BLEND_ONE;t.DestBlend=blend==0?D3D12_BLEND_ONE:D3D12_BLEND_INV_SRC_ALPHA;t.BlendOp=D3D12_BLEND_OP_ADD;t.SrcBlendAlpha=blend==0?D3D12_BLEND_ZERO:D3D12_BLEND_ONE;t.DestBlendAlpha=blend==0?D3D12_BLEND_ONE:D3D12_BLEND_INV_SRC_ALPHA;t.BlendOpAlpha=D3D12_BLEND_OP_ADD;
            check(device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(graphics[blend].out())),"Create GPU sprite graphics pipeline");}
        D3D12_INDIRECT_ARGUMENT_DESC argument{};argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;D3D12_COMMAND_SIGNATURE_DESC command{};command.ByteStride=12;command.NumArgumentDescs=1;command.pArgumentDescs=&argument;
        check(device->CreateCommandSignature(&command,nullptr,IID_PPV_ARGS(dispatchSignature.out())),"Create GPU sprite dispatch signature");argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;command.ByteStride=16;check(device->CreateCommandSignature(&command,nullptr,IID_PPV_ARGS(drawSignature.out())),"Create GPU sprite draw signature");
    }
    void constantsTo(ID3D12GraphicsCommandList* list){list->SetComputeRoot32BitConstants(0,40,&constants,0);}
    void restoreArgs(ID3D12GraphicsCommandList* list){if(argsIndirect){transition(list,args.p,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);argsIndirect=false;}}
    void indirect(ID3D12GraphicsCommandList* list,unsigned pipeline){barrier(list);transition(list,args.p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);argsIndirect=true;list->SetPipelineState(compute[pipeline].p);list->ExecuteIndirect(dispatchSignature.p,1,args.p,0,nullptr,0);barrier(list);}
};
GpuEffectSprites::GpuEffectSprites(ID3D12Device* device,DXGI_FORMAT color,DXGI_FORMAT light,DXGI_FORMAT depth,GpuSpriteLimits limits){
    if(!device||!limits.capacity||limits.capacity>262144||(limits.capacity&(limits.capacity-1))||!limits.maxAllocationBytes)throw engine::effects::Error("NG-E-EFFECT-DRAW-BUDGET","GPU sprite capacity must be a power of two in [1,262144]");
    impl_=std::make_unique<Impl>(device,color,light,depth,limits);
}
GpuEffectSprites::~GpuEffectSprites()=default;
bool GpuEffectSprites::prepare(ID3D12GraphicsCommandList* list,std::uint64_t fence,const Mat4& vp,const std::vector<engine::effects::GpuDrawBatch>& batches,const std::map<std::string,unsigned>& textures,std::string& error){
    error.clear();if(!impl_||!list||!fence||fence<impl_->stats.lastFence){error="NG-E-EFFECT-DRAW-STATE: invalid prepare";return false;}
    auto& s=*impl_;for(float v:vp.m)if(!std::isfinite(v)){error="NG-E-EFFECT-DRAW-CAMERA: nonfinite projection";return false;}
    Mat4 inverse;if(!invertMatrix(vp,inverse)){error="NG-E-EFFECT-DRAW-CAMERA: singular projection";return false;}
    std::uint64_t bound=0;std::size_t sources=0;
    for(const auto& batch:batches){const auto& b=batch.buffers;if(!b.attributes||!b.alive||!b.counters||!b.capacity){error="NG-E-EFFECT-DRAW-STATE: missing prepared GPU pool";return false;}
        for(const auto& r:batch.renderers){if(r.type!="sprite"||r.lit||static_cast<unsigned>(r.blend)>2||textures.find(r.texture)==textures.end()){error="NG-E-EFFECT-DRAW-ASSET: unsupported renderer or missing texture binding";return false;}bound+=b.capacity;++sources;}}
    if(sources>8192||bound>8000000){error="NG-E-EFFECT-DRAW-BUDGET: gather source/work limit exceeded";return false;}
    if(s.ready){transition(list,s.records.p,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);transition(list,s.state.p,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);transition(list,s.draws.p,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);s.ready=false;}s.restoreArgs(list);
    s.constants={};s.constants.vp=vp;s.constants.capacity=s.capacity;
    for(unsigned column=0;column<2;++column){const float* v=inverse.m+column*4;float norm=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);if(!(norm>0)){error="NG-E-EFFECT-DRAW-CAMERA: invalid camera basis";return false;}float* target=column?s.constants.up:s.constants.right;for(unsigned i=0;i<3;++i)target[i]=v[i]/norm;}
    list->SetComputeRootSignature(s.root.p);s.constantsTo(list);ID3D12Resource* output[]={s.records.p,s.state.p,s.args.p,s.draws.p};for(unsigned i=0;i<4;++i)list->SetComputeRootUnorderedAccessView(i+5,output[i]->GetGPUVirtualAddress());
    list->SetPipelineState(s.compute[0].p);list->Dispatch(1,1,1);barrier(list);
    unsigned ordinal=0;
    for(const auto& batch:batches){const auto& b=batch.buffers;transition(list,b.counters,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->SetComputeRootShaderResourceView(1,b.attributes->GetGPUVirtualAddress());list->SetComputeRootShaderResourceView(2,b.alive->GetGPUVirtualAddress());list->SetComputeRootShaderResourceView(3,b.counters->GetGPUVirtualAddress());
        s.constants.layout[0]=b.layout.position;s.constants.layout[1]=b.layout.color;s.constants.layout[2]=b.layout.size;s.constants.layout[3]=b.capacity;s.constants.origin[0]=b.origin.x;s.constants.origin[1]=b.origin.y;s.constants.origin[2]=b.origin.z;s.constants.local=b.localSpace;
        for(const auto& renderer:batch.renderers){s.constants.texture=textures.at(renderer.texture);s.constants.blend=static_cast<unsigned>(renderer.blend);s.constants.ordinal=ordinal++;s.constantsTo(list);s.restoreArgs(list);list->SetPipelineState(s.compute[1].p);list->Dispatch(1,1,1);s.indirect(list,2);++s.stats.gatherIndirects;}
        transition(list,b.counters,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
    s.restoreArgs(list);list->SetPipelineState(s.compute[3].p);list->Dispatch(1,1,1);s.indirect(list,4);
    unsigned sortBound=1;while(sortBound<std::min<std::uint64_t>(bound,s.capacity))sortBound*=2;
    for(unsigned k=2;k<=sortBound;k*=2)for(unsigned j=k/2;j;j/=2){s.constants.k=k;s.constants.j=j;s.constantsTo(list);s.restoreArgs(list);list->SetPipelineState(s.compute[5].p);list->Dispatch(1,1,1);s.indirect(list,6);++s.stats.sortIndirects;}
    transition(list,s.records.p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);transition(list,s.state.p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);transition(list,s.draws.p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);s.ready=true;s.stats.lastFence=fence;++s.stats.frames;return true;
}
bool GpuEffectSprites::draw(ID3D12GraphicsCommandList* list,std::uint64_t fence,std::string& error){
    error.clear();if(!impl_||!list||!impl_->ready||!fence||fence<impl_->stats.lastFence){error="NG-E-EFFECT-DRAW-STATE: invalid indirect draw";return false;}auto& s=*impl_;
    list->SetGraphicsRootSignature(s.root.p);list->SetGraphicsRoot32BitConstants(0,40,&s.constants,0);list->SetGraphicsRootShaderResourceView(4,s.records.p->GetGPUVirtualAddress());list->SetGraphicsRootShaderResourceView(3,s.state.p->GetGPUVirtualAddress());list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for(unsigned blend:{2u,1u,0u}){list->SetGraphicsRoot32BitConstant(0,blend,26);list->SetPipelineState(s.graphics[blend].p);list->ExecuteIndirect(s.drawSignature.p,1,s.draws.p,blend*16,nullptr,0);++s.stats.drawIndirects;}s.stats.lastFence=fence;return true;
}
GpuSpriteBuffers GpuEffectSprites::buffers() const{return impl_?GpuSpriteBuffers{impl_->records.p,impl_->state.p,impl_->draws.p,impl_->capacity}:GpuSpriteBuffers{};}
GpuSpriteStats GpuEffectSprites::stats() const{return impl_?impl_->stats:GpuSpriteStats{};}
bool GpuEffectSprites::shutdown(std::uint64_t fence,std::string& error){error.clear();if(!impl_)return true;if(fence<impl_->stats.lastFence){error="NG-E-EFFECT-DRAW-FENCE: draw buffers are in use";return false;}impl_.reset();return true;}
}
