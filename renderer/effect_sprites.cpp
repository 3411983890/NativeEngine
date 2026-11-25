#include "d3d12_renderer_internal.h"
#include "nativegpu_shaders.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

namespace nativegpu {
namespace {
struct GpuSprite { float positionWidth[4]; float color[4]; float shape[4]; };
static_assert(sizeof(GpuSprite)==48,"Effect sprite GPU layout changed");
struct SpriteConstants { Mat4 vp; float right[4]; float up[4]; unsigned offset,texture,blend,padding; };
static_assert(sizeof(SpriteConstants)==28*4,"Effect sprite root layout changed");
}
bool Renderer::configureEffectTextures(std::vector<TextureUpload> textures,std::string& error) {
    error.clear(); auto& state=*impl_;
    if (state.initialized) { error="NG-E-EFFECT-STATE: texture configuration requires a stopped renderer"; return false; }
    if (textures.size()>detail::MaxEffectTextures) { error="NG-E-EFFECT-BUDGET: too many sprite textures"; return false; }
    std::size_t total=0;
    for (const auto& texture:textures) {
        if (!texture.width||!texture.height||texture.width>4096||texture.height>4096||texture.format>4||!texture.mips) {
            error="NG-E-EFFECT-TEXTURE: invalid dimensions or format"; return false;
        }
        unsigned w=texture.width,h=texture.height,mips=0; std::size_t expected=0;
        do {
            expected+=texture.format==0?static_cast<std::size_t>(w)*h*4:
                static_cast<std::size_t>((w+3)/4)*((h+3)/4)*(texture.format==1?8:16);
            ++mips; if (w==1&&h==1) break; w=std::max(1u,w/2);h=std::max(1u,h/2);
        } while (true);
        if (mips!=texture.mips||expected!=texture.pixels.size()) { error="NG-E-EFFECT-TEXTURE: invalid mip payload"; return false; }
        total+=expected;
    }
    if (total>128*1024*1024) { error="NG-E-EFFECT-BUDGET: sprite textures exceed 128 MiB"; return false; }
    state.configuredEffectTextures=std::move(textures);
    return true;
}
bool Renderer::setEffectSprites(std::vector<EffectSprite> sprites,std::string& error) {
    error.clear(); auto& state=*impl_;
    if(!state.configuredGpuEffects.empty()&&!sprites.empty()){error="NG-E-EFFECT-STATE: choose one CPU/GPU effect backend per renderer";return false;}
    if (sprites.size()>detail::MaxEffectSprites) { error="NG-E-EFFECT-BUDGET: sprite draw capacity exceeded"; return false; }
    for (const auto& sprite:sprites) {
        if (sprite.texture>=state.configuredEffectTextures.size()||sprite.blend>2||!std::isfinite(sprite.rotation)) {
            error="NG-E-EFFECT-SPRITE: invalid texture or blend"; return false;
        }
        for (float p:sprite.position) if (!std::isfinite(p)) { error="NG-E-EFFECT-SPRITE: non-finite position"; return false; }
        for (float c:sprite.color) if (!std::isfinite(c)||c<0||c>64) { error="NG-E-EFFECT-SPRITE: invalid color"; return false; }
        for (float s:sprite.size) if (!std::isfinite(s)||s<0||s>10000) { error="NG-E-EFFECT-SPRITE: invalid size"; return false; }
        if (sprite.color[3]>1) { error="NG-E-EFFECT-SPRITE: alpha outside [0,1]"; return false; }
    }
    state.effectSprites=std::move(sprites);
    return true;
}
bool Renderer::Impl::createEffectSprites(std::string& error) {
    if (configuredEffectTextures.empty()) return true;
    if (!shaders::kDxcAvailable||shaders::kEffectSpriteVS.size<64||shaders::kEffectSpritePS.size<64) {
        error="NG-E-EFFECT-SHADER: sprite SM 6.6 shaders unavailable"; return false;
    }
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.ShaderRegister=0; parameters[0].Constants.Num32BitValues=28;
    parameters[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor.ShaderRegister=0; parameters[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD=D3D12_FLOAT32_MAX; sampler.MaxAnisotropy=1;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS; sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters=2; signature.pParameters=parameters;
    signature.NumStaticSamplers=1; signature.pStaticSamplers=&sampler;
    signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    ID3DBlob* bytes=nullptr; ID3DBlob* diagnostics=nullptr;
    const auto serialized=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&diagnostics);
    if (FAILED(serialized)) {
        error=diagnostics?std::string(static_cast<const char*>(diagnostics->GetBufferPointer()),diagnostics->GetBufferSize()):"Effect sprite root serialization failed";
        detail::release(diagnostics); detail::release(bytes); return false;
    }
    detail::release(diagnostics);
    const auto created=device->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&effectSpriteRoot));
    detail::release(bytes);
    if (!detail::checked(created,"Create effect sprite root",error)) return false;
    track(effectSpriteRoot);
    for (unsigned blend=0;blend<3;++blend) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature=effectSpriteRoot;
        pipeline.VS={shaders::kEffectSpriteVS.data,shaders::kEffectSpriteVS.size};
        pipeline.PS={shaders::kEffectSpritePS.data,shaders::kEffectSpritePS.size};
        pipeline.SampleMask=std::numeric_limits<UINT>::max();
        pipeline.SampleDesc.Count=1;
        pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable=TRUE;
        pipeline.DepthStencilState.DepthEnable=TRUE;
        pipeline.DepthStencilState.DepthWriteMask=blend==2?D3D12_DEPTH_WRITE_MASK_ALL:D3D12_DEPTH_WRITE_MASK_ZERO;
        pipeline.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pipeline.DSVFormat=detail::DepthDsvFormat;
        pipeline.NumRenderTargets=2;
        pipeline.RTVFormats[0]=pipeline.RTVFormats[1]=detail::SceneColorFormat;
        pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        auto& target=pipeline.BlendState.RenderTarget[0];
        target.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        target.BlendEnable=blend!=2; target.SrcBlend=D3D12_BLEND_ONE;
        target.DestBlend=blend==0?D3D12_BLEND_ONE:D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOp=D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha=blend==0?D3D12_BLEND_ZERO:D3D12_BLEND_ONE;
        target.DestBlendAlpha=blend==0?D3D12_BLEND_ONE:D3D12_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha=D3D12_BLEND_OP_ADD;
        if (!detail::checked(device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&effectSpritePipelines[blend])),"Create effect sprite pipeline",error)) return false;
        track(effectSpritePipelines[blend]);
    }
    return uploadTextureSet(configuredEffectTextures,detail::EffectTextureSrvBase,effectTextures,error);
}
void Renderer::Impl::releaseEffectSprites() {
    for (auto& resource:effectTextures) destroy(resource);
    effectTextures.clear(); effectSprites.clear();
    for (unsigned i=0;i<detail::FrameCount;++i) { destroy(effectSpriteBuffers[i]); effectSpriteCapacity[i]=0; }
    for (auto& pipeline:effectSpritePipelines) destroy(pipeline);
    destroy(effectSpriteRoot);
    effectSpriteDraws=effectSpriteFrames=effectSpritesRendered=0;
}
bool Renderer::Impl::drawEffectSprites(ID3D12GraphicsCommandList* list,unsigned index,const Mat4& vp,
                                      const float camera[3],std::string& error) {
    (void)camera;
    if (effectSprites.empty()) return true;
    if (!effectSpriteRoot) { error="NG-E-EFFECT-STATE: sprite textures/pipeline not configured"; return false; }
    Mat4 inverse;
    if (!invertMatrix(vp,inverse)) { error="NG-E-EFFECT-CAMERA: singular view projection"; return false; }
    auto sorted=effectSprites;
    const auto depth=[&](const EffectSprite& s) { return vp.m[3]*s.position[0]+vp.m[7]*s.position[1]+vp.m[11]*s.position[2]+vp.m[15]; };
    const auto order=[](unsigned blend) { return blend==2?0u:blend==1?1u:2u; };
    std::stable_sort(sorted.begin(),sorted.end(),[&](const EffectSprite& a,const EffectSprite& b) {
        if (a.blend!=b.blend) return order(a.blend)<order(b.blend);
        if (a.blend==0&&a.texture!=b.texture) return a.texture<b.texture;
        if (a.blend!=0&&depth(a)!=depth(b)) return a.blend==1?depth(a)>depth(b):depth(a)<depth(b);
        return a.stableId<b.stableId;
    });
    struct Batch { unsigned start,count,texture,blend; };
    std::vector<Batch> batches;
    for (unsigned start=0;start<sorted.size();) {
        unsigned end=start+1;
        while(end<sorted.size()&&sorted[end].texture==sorted[start].texture&&sorted[end].blend==sorted[start].blend)++end;
        batches.push_back({start,end-start,sorted[start].texture,sorted[start].blend});start=end;
    }
    if (batches.size()>8192) { error="NG-E-EFFECT-BUDGET: sorted sprite batches exceed 8192";return false; }
    // Renderer::render has already waited for this frame index's fence. Only
    // that upload buffer can be resized or written; other in-flight frames keep theirs.
    if (effectSpriteCapacity[index]<sorted.size()) {
        destroy(effectSpriteBuffers[index]);
        unsigned capacity=256; while(capacity<sorted.size())capacity*=2;
        if (!createBuffer(static_cast<UINT64>(capacity)*sizeof(GpuSprite),D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ,&effectSpriteBuffers[index],error)) {effectSpriteCapacity[index]=0;return false;}
        effectSpriteCapacity[index]=capacity;
    }
    D3D12_RANGE noRead{0,0}; void* mapped=nullptr;
    if (!detail::checked(effectSpriteBuffers[index]->Map(0,&noRead,&mapped),"Map effect sprite instances",error))return false;
    auto* records=static_cast<GpuSprite*>(mapped);
    for(std::size_t i=0;i<sorted.size();++i) {
        std::memcpy(records[i].positionWidth,sorted[i].position,3*sizeof(float)); records[i].positionWidth[3]=sorted[i].size[0];
        std::memcpy(records[i].color,sorted[i].color,4*sizeof(float));
        records[i].shape[0]=sorted[i].size[1];records[i].shape[1]=sorted[i].rotation;records[i].shape[2]=records[i].shape[3]=0;
    }
    D3D12_RANGE written{0,sorted.size()*sizeof(GpuSprite)}; effectSpriteBuffers[index]->Unmap(0,&written);
    SpriteConstants constants{}; constants.vp=vp;
    const auto basis=[&](unsigned column,float target[4]) {
        const float* v=inverse.m+column*4; const float norm=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
        if (!(norm>0))return false; for(unsigned k=0;k<3;++k)target[k]=v[k]/norm;return true;
    };
    if(!basis(0,constants.right)||!basis(1,constants.up)){error="NG-E-EFFECT-CAMERA: invalid camera basis";return false;}
    D3D12_CPU_DESCRIPTOR_HANDLE targets[2];targets[0]=rtvHeap->GetCPUDescriptorHandleForHeapStart();
    targets[0].ptr+=static_cast<SIZE_T>(detail::RtvSlotBase+index*detail::RtvSlotsPerFrame)*rtvStride;
    targets[1]=targets[0];targets[1].ptr+=rtvStride;
    auto dsv=dsvHeap->GetCPUDescriptorHandleForHeapStart();dsv.ptr+=static_cast<SIZE_T>(index)*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    list->OMSetRenderTargets(2,targets,FALSE,&dsv);
    D3D12_VIEWPORT viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
    D3D12_RECT scissor{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);
    list->SetGraphicsRootSignature(effectSpriteRoot);
    list->SetGraphicsRootShaderResourceView(1,effectSpriteBuffers[index]->GetGPUVirtualAddress());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for(const auto& batch:batches) {
        constants.offset=batch.start;constants.texture=detail::EffectTextureSrvBase+batch.texture;constants.blend=batch.blend;
        list->SetPipelineState(effectSpritePipelines[batch.blend]);
        list->SetGraphicsRoot32BitConstants(0,28,&constants,0);
        list->DrawInstanced(6,batch.count,0,0);++effectSpriteDraws;
    }
    ++effectSpriteFrames;effectSpritesRendered+=static_cast<unsigned>(sorted.size());
    return true;
}
std::string Renderer::Impl::effectSpritesJson() const {
    std::ostringstream out;std::size_t bytes=0;for(auto capacity:effectSpriteCapacity)bytes+=capacity*sizeof(GpuSprite);
    out<<"\"effectSprites\":{\"available\":"<<(effectSpriteRoot?"true":"false")
       <<",\"implemented\":"<<(effectSpriteFrames?"true":"false")<<",\"frames\":"<<effectSpriteFrames
       <<",\"draws\":"<<effectSpriteDraws<<",\"rendered\":"<<effectSpritesRendered
       <<",\"textures\":"<<effectTextures.size()<<",\"uploadBytes\":"<<bytes
       <<",\"frameBuffered\":true,\"alphaSorted\":true,\"lit\":false,\"softParticles\":false},";
    return out.str();
}
} // namespace nativegpu
