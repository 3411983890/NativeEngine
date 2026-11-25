#include "d3d12_renderer_internal.h"
#include "nativegpu_shaders.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <sstream>

namespace nativegpu {
namespace {
constexpr float Pi=3.14159265359f;
constexpr auto ReadState=static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
unsigned slot(unsigned value) { return detail::QualityDescriptorBase+value; }
void normalize3(float* p) {
    float length=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
    if(length<1e-6f){p[0]=0;p[1]=1;p[2]=0;return;}
    for(unsigned i=0;i<3;i++)p[i]/=length;
}
void uavBarrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource=resource;list->ResourceBarrier(1,&barrier);
}
}

void Renderer::Impl::releaseEnvironmentTexture(){
    destroy(environmentTexture.resource);destroy(environmentInlineTexture.resource);destroy(environmentUpload);environmentTexture.state=environmentInlineTexture.state=D3D12_RESOURCE_STATE_COMMON;environmentTextureHash.clear();environmentCopyPending=false;
}
bool Renderer::Impl::prepareEnvironmentTexture(std::string& error){
    const auto& map=environment.panorama;
    if(!map){if(environmentTexture){if(!waitIdle(error))return false;releaseQuality();releaseEnvironmentTexture();}return true;}
    if(!std::isfinite(environment.environmentIntensity)||environment.environmentIntensity<0||environment.environmentIntensity>10000||!std::isfinite(environment.environmentRotation)||std::abs(environment.environmentRotation)>3600){error="NG-E-ASSET-ENVIRONMENT: invalid intensity or rotation";return false;}
    if(environmentTexture&&environmentTextureHash==map->contentHash)return true;
    if(map->width<2||map->width>16384||map->height==0||std::uint64_t(map->width)!=std::uint64_t(map->height)*2||std::uint64_t(map->width)*map->height*16>256ull*1024*1024||map->pixels.size()!=std::uint64_t(map->width)*map->height*4||map->contentHash.empty()){error="NG-E-ASSET-ENVIRONMENT: invalid panorama upload";return false;}
    for(std::size_t i=0;i<map->pixels.size();i++)if(!std::isfinite(map->pixels[i])||map->pixels[i]<0||map->pixels[i]>1e12f||(i%4==3&&map->pixels[i]!=1)){error="NG-E-ASSET-ENVIRONMENT: invalid panorama radiance";return false;}
    for(const auto& row:map->diffuseSH)for(float value:row)if(!std::isfinite(value)||std::abs(value)>1e14f){error="NG-E-ASSET-ENVIRONMENT: invalid diffuse SH upload";return false;}
    if(!waitIdle(error))return false;releaseQuality();releaseEnvironmentTexture();
    if(!inlineEnvironmentPipeline&&!createBindlessPipeline(error,true))return false;
    if(!createTexture2D(map->width,map->height,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE,nullptr,environmentTexture,error))return false;
    auto array=detail::bufferDescription(1);array.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;array.Width=64;array.Height=64;array.DepthOrArraySize=8;array.MipLevels=1;array.Format=DXGI_FORMAT_R8G8B8A8_UNORM;array.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const auto heap=detail::heapProperties(D3D12_HEAP_TYPE_DEFAULT);if(!detail::checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&array,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&environmentInlineTexture.resource)),"Create environment inline textures",error))return false;track(environmentInlineTexture.resource);environmentInlineTexture.state=D3D12_RESOURCE_STATE_COPY_DEST;
    UINT64 bytes=0;const auto desc=environmentTexture.resource->GetDesc();device->GetCopyableFootprints(&desc,0,1,0,&environmentFootprint,nullptr,nullptr,&bytes);device->GetCopyableFootprints(&array,0,8,(bytes+511)&~UINT64(511),environmentInlineFootprints,nullptr,nullptr,nullptr);for(const auto& footprint:environmentInlineFootprints)bytes=std::max(bytes,footprint.Offset+UINT64(footprint.Footprint.RowPitch)*footprint.Footprint.Height);
    if(!createBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,&environmentUpload,error))return false;void* pixels=nullptr;D3D12_RANGE noRead{0,0};if(!detail::checked(environmentUpload->Map(0,&noRead,&pixels),"Map environment upload",error))return false;std::memset(pixels,0,static_cast<std::size_t>(bytes));
    for(unsigned y=0;y<map->height;y++)std::memcpy(static_cast<unsigned char*>(pixels)+environmentFootprint.Offset+y*environmentFootprint.Footprint.RowPitch,map->pixels.data()+std::size_t(y)*map->width*4,std::size_t(map->width)*16);
    // Match the engine's procedural checker/brick/metal palette for inline scene
    // materials, while using the same environment BRDF as imported materials.
    for(unsigned slice=0;slice<8;slice++)for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++){
        unsigned char r,g,b;if(slice==0){r=g=b=((x/8+y/8)&1)?205:255;}else if(slice==1){const bool mortar=(y%16<2)||((x+((y/16)%2)*8)%32<2);if(mortar){r=205;g=198;b=184;}else{const unsigned v=18+((x/32+y/16)&3)*7;r=static_cast<unsigned char>(138+v);g=static_cast<unsigned char>(62+v/2);b=static_cast<unsigned char>(48+v/3);}}else if(slice==3){const bool light=((x/8+y/8)&1)!=0;r=light?255:40;g=light?40:0;b=light?220:60;}else if(slice==4){r=90;g=170;b=210;}else if(slice==5){r=200;g=190;b=120;}else if(slice==6){r=120;g=200;b=140;}else{const unsigned stripe=((x/4)&1)?188:132;r=static_cast<unsigned char>(stripe-12);g=static_cast<unsigned char>(stripe);b=static_cast<unsigned char>(stripe+18);}auto* pixel=static_cast<unsigned char*>(pixels)+environmentInlineFootprints[slice].Offset+y*environmentInlineFootprints[slice].Footprint.RowPitch+x*4;pixel[0]=r;pixel[1]=g;pixel[2]=b;pixel[3]=255;
    }
    D3D12_RANGE written{0,static_cast<SIZE_T>(bytes)};environmentUpload->Unmap(0,&written);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;device->CreateShaderResourceView(environmentTexture.resource,&view,cpuDescriptor(slot(detail::QEnvironment)));
    view={};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2DArray.MipLevels=1;view.Texture2DArray.ArraySize=8;device->CreateShaderResourceView(environmentInlineTexture.resource,&view,cpuDescriptor(slot(detail::QInlineMaterials)));
    environmentTextureHash=map->contentHash;environmentCopyPending=true;return true;
}
void Renderer::Impl::recordEnvironmentTexture(ID3D12GraphicsCommandList* list){
    if(!environmentCopyPending)return;const auto copy=[&](ID3D12Resource* texture,unsigned subresource,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint){D3D12_TEXTURE_COPY_LOCATION to{},from{};to.pResource=texture;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=subresource;from.pResource=environmentUpload;from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);};
    copy(environmentTexture.resource,0,environmentFootprint);for(unsigned i=0;i<8;i++)copy(environmentInlineTexture.resource,i,environmentInlineFootprints[i]);transitionTo(environmentTexture,ReadState,list);transitionTo(environmentInlineTexture,ReadState,list);environmentCopyPending=false;
}

bool Renderer::Impl::createQuality(std::string& error) {
    if(qualityReady)return true;
    if((!materialBindless&&!inlineEnvironmentPipeline)||raytracingTier<11||!blasBuilt) {
        error="world lighting needs SM 6.6 materials, inline DXR 1.1 and rigid TLAS";
        return false;
    }
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister=0;
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister=1;parameters[1].Constants.Num32BitValues=4;
    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    for(unsigned i=0;i<2;i++) {
        samplers[i].ShaderRegister=i;samplers[i].Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU=samplers[i].AddressV=samplers[i].AddressW=
            i==1?D3D12_TEXTURE_ADDRESS_MODE_WRAP:D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxLOD=D3D12_FLOAT32_MAX;samplers[i].MaxAnisotropy=1;
        samplers[i].ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    }
    D3D12_ROOT_SIGNATURE_DESC root{};root.NumParameters=2;root.pParameters=parameters;
    root.NumStaticSamplers=2;root.pStaticSamplers=samplers;
    root.Flags=D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
    ID3DBlob* blob=nullptr;ID3DBlob* diagnostics=nullptr;
    HRESULT hr=D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&diagnostics);
    if(FAILED(hr)) {
        error=diagnostics?std::string(static_cast<char*>(diagnostics->GetBufferPointer()),diagnostics->GetBufferSize()):"quality root serialization failed";
        detail::release(blob);detail::release(diagnostics);return false;
    }
    detail::release(diagnostics);
    hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&qualityRoot));
    detail::release(blob);if(!detail::checked(hr,"Create world lighting root",error))return false;
    track(qualityRoot);
    const shaders::Blob* shaders[]={&shaders::kQualityNoise,&shaders::kQualityIbl,
        &shaders::kQualityBrdf,&shaders::kQualityProbe,&shaders::kQualityCloudShadow,
        &shaders::kQualityFogInject,&shaders::kQualityFogIntegrate,&shaders::kQualityCloud,&shaders::kQualityReset,
        &shaders::kQualityLocalShadowCache};
    for(unsigned i=0;i<10;i++) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=qualityRoot;
        desc.CS={shaders[i]->data,shaders[i]->size};
        if(!detail::checked(device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&qualityPipelines[i])),
                            "Create world lighting compute pass",error))return false;
        track(qualityPipelines[i]);
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{};graphics.pRootSignature=qualityRoot;
    graphics.VS={shaders::kQualityCompositeVS.data,shaders::kQualityCompositeVS.size};
    graphics.PS={shaders::kQualityCompositePS.data,shaders::kQualityCompositePS.size};
    auto& blend=graphics.BlendState.RenderTarget[0];blend.SrcBlend=D3D12_BLEND_ONE;
    blend.DestBlend=D3D12_BLEND_ZERO;blend.BlendOp=D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_ZERO;
    blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    graphics.SampleMask=UINT_MAX;graphics.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
    graphics.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;graphics.RasterizerState.DepthClipEnable=TRUE;
    graphics.DepthStencilState.DepthEnable=FALSE;graphics.DepthStencilState.StencilEnable=FALSE;
    graphics.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    graphics.NumRenderTargets=1;graphics.RTVFormats[0]=detail::ColorFormat;graphics.SampleDesc.Count=1;
    if(!detail::checked(device->CreateGraphicsPipelineState(&graphics,IID_PPV_ARGS(&qualityCompositePipeline)),
                        "Create world lighting composite",error))return false;
    track(qualityCompositePipeline);

    qualityGrid[0]=environment.quality>1?160:80;qualityGrid[1]=environment.quality>1?90:45;
    qualityGrid[2]=environment.quality>1?64:48;
    auto texture=[&](TrackedResource& output,unsigned w,unsigned h,unsigned d,DXGI_FORMAT format,
                     unsigned descriptor,bool volume=false,unsigned mips=1,unsigned array=1) {
        D3D12_RESOURCE_DESC desc{};desc.Dimension=volume?D3D12_RESOURCE_DIMENSION_TEXTURE3D:D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=w;desc.Height=h;desc.DepthOrArraySize=static_cast<UINT16>(volume?d:array);
        desc.MipLevels=static_cast<UINT16>(mips);desc.Format=format;desc.SampleDesc.Count=1;
        desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        auto heap=detail::heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        if(!detail::checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&output.resource)),"Create lighting texture",error))return false;
        track(output.resource);output.state=D3D12_RESOURCE_STATE_COMMON;
        qualityTextureBytes+=device->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if(volume){srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;srv.Texture3D.MipLevels=mips;}
        else if(array==6){srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;srv.TextureCube.MipLevels=mips;}
        else if(array>6){srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;srv.Texture2DArray.MipLevels=mips;srv.Texture2DArray.ArraySize=array;}
        else {srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=mips;}
        device->CreateShaderResourceView(output.resource,&srv,cpuDescriptor(slot(descriptor)));
        for(unsigned mip=0;mip<mips;mip++) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=format;
            if(volume){uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE3D;uav.Texture3D.WSize=d;uav.Texture3D.MipSlice=mip;}
            else if(array>1){uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;uav.Texture2DArray.ArraySize=array;uav.Texture2DArray.MipSlice=mip;}
            else {uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;uav.Texture2D.MipSlice=mip;}
            device->CreateUnorderedAccessView(output.resource,nullptr,&uav,cpuDescriptor(slot(descriptor+1+mip)));
        }
        return true;
    };
    if(!texture(qualityIbl,64,64,1,environment.panorama?DXGI_FORMAT_R32G32B32A32_FLOAT:detail::SceneColorFormat,detail::QIbl,false,7,6)||
       !texture(qualityBrdf,64,64,1,DXGI_FORMAT_R16G16_FLOAT,detail::QBrdf)||
       !texture(qualityNoise,64,64,64,DXGI_FORMAT_R8G8B8A8_UNORM,detail::QNoise,true)||
       !texture(qualityWeather,64,64,1,DXGI_FORMAT_R16_FLOAT,detail::QWeather)||
       !texture(qualityCloudShadow,128,128,1,DXGI_FORMAT_R16_FLOAT,detail::QCloudShadow)||
       !texture(qualityLocalShadowCache,128,128,1,DXGI_FORMAT_R32_FLOAT,detail::QLocalShadowCache,false,1,24))return false;
    for(unsigned i=0;i<2;i++) {
        // Probe views are grouped SRV[3], UAV[3], unlike ordinary SRV/UAV pairs.
        unsigned base=detail::QProbe+i*6;
        if(!texture(qualityProbeIrradiance[i],128,1024,1,detail::SceneColorFormat,base)||
           !texture(qualityProbeDistance[i],128,1024,1,detail::SceneColorFormat,base+1)||
           !texture(qualityProbeData[i],16,8,16,detail::SceneColorFormat,base+2,true))return false;
        // Rewrite the three UAV slots after all SRVs have been installed.
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=detail::SceneColorFormat;
        uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(qualityProbeIrradiance[i].resource,nullptr,&uav,cpuDescriptor(slot(base+3)));
        device->CreateUnorderedAccessView(qualityProbeDistance[i].resource,nullptr,&uav,cpuDescriptor(slot(base+4)));
        uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE3D;uav.Texture3D.WSize=16;
        device->CreateUnorderedAccessView(qualityProbeData[i].resource,nullptr,&uav,cpuDescriptor(slot(base+5)));
        if(!texture(qualityFogHistory[i],qualityGrid[0],qualityGrid[1],qualityGrid[2],DXGI_FORMAT_R32G32B32A32_FLOAT,
                    detail::QFogHistory+i*2,true)||
           !texture(qualityCloud[i],(width+1)/2,(height+1)/2,1,detail::SceneColorFormat,detail::QCloud+i*4)||
           !texture(qualityCloudDepth[i],(width+1)/2,(height+1)/2,1,DXGI_FORMAT_R16_FLOAT,detail::QCloud+i*4+2))return false;
    }
    if(!texture(qualityFogScratch,qualityGrid[0],qualityGrid[1],qualityGrid[2],DXGI_FORMAT_R32G32B32A32_FLOAT,detail::QFogScratch,true)||
       !texture(qualityFogIntegrated,qualityGrid[0],qualityGrid[1],qualityGrid[2],DXGI_FORMAT_R32G32B32A32_FLOAT,detail::QFogIntegrated,true))return false;
    if(std::any_of(configuredGpuEffects.begin(),configuredGpuEffects.end(),[](const auto& a){return bool(a->smoke);})&&
       !texture(qualityFogIncident,qualityGrid[0],qualityGrid[1],qualityGrid[2],DXGI_FORMAT_R32G32B32A32_FLOAT,
                detail::SmokeIncidentSrv-detail::QualityDescriptorBase,true))return false;
    D3D12_RANGE noRead{0,0};
    if(!createBuffer(detail::QualityCbStride*detail::FrameCount,D3D12_HEAP_TYPE_UPLOAD,
                     D3D12_RESOURCE_STATE_GENERIC_READ,&qualityConstantsBuffer,error)||
       !detail::checked(qualityConstantsBuffer->Map(0,&noRead,&qualityConstantsMapped),"Map lighting constants",error))return false;
    for(unsigned i=0;i<detail::FrameCount;i++) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};cbv.BufferLocation=qualityConstantsBuffer->GetGPUVirtualAddress()+i*detail::QualityCbStride;
        cbv.SizeInBytes=detail::QualityCbStride;device->CreateConstantBufferView(&cbv,cpuDescriptor(slot(detail::QConstants+i)));
        if(!createBuffer(detail::MaxInstances*sizeof(detail::QualityInstance),D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_STATE_GENERIC_READ,&qualityInstances[i],error)||
           !detail::checked(qualityInstances[i]->Map(0,&noRead,&qualityInstancesMapped[i]),"Map lighting hit materials",error))return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.NumElements=detail::MaxInstances;srv.Buffer.StructureByteStride=sizeof(detail::QualityInstance);
        device->CreateShaderResourceView(qualityInstances[i],&srv,cpuDescriptor(slot(detail::QInstances+i)));
    }
    qualityReady=true;qualityEnvironmentDirty=true;return true;
}

void Renderer::Impl::releaseQuality() {
    auto releaseTexture=[&](TrackedResource& r){destroy(r.resource);r.state=D3D12_RESOURCE_STATE_COMMON;};
    releaseTexture(qualityIbl);releaseTexture(qualityBrdf);releaseTexture(qualityNoise);
    releaseTexture(qualityWeather);releaseTexture(qualityCloudShadow);
    releaseTexture(qualityLocalShadowCache);
    releaseTexture(qualityFogScratch);releaseTexture(qualityFogIntegrated);releaseTexture(qualityFogIncident);
    for(unsigned i=0;i<2;i++) {
        releaseTexture(qualityProbeIrradiance[i]);releaseTexture(qualityProbeDistance[i]);releaseTexture(qualityProbeData[i]);
        releaseTexture(qualityFogHistory[i]);releaseTexture(qualityCloud[i]);releaseTexture(qualityCloudDepth[i]);
    }
    if(qualityConstantsBuffer&&qualityConstantsMapped)qualityConstantsBuffer->Unmap(0,nullptr);
    qualityConstantsMapped=nullptr;destroy(qualityConstantsBuffer);
    for(unsigned i=0;i<detail::FrameCount;i++) {
        if(qualityInstances[i]&&qualityInstancesMapped[i])qualityInstances[i]->Unmap(0,nullptr);
        qualityInstancesMapped[i]=nullptr;destroy(qualityInstances[i]);
    }
    for(auto*& p:qualityPipelines)destroy(p);
    destroy(qualityCompositePipeline);destroy(qualityRoot);
    qualityReady=false;qualityInitialized=false;qualityHistoryValid=false;qualityTextureBytes=0;
    qualityFrames=0;qualityShadowCount=0;
    qualityProbeTimes.clear();qualityCloudShadowTimes.clear();qualityFogTimes.clear();qualityCloudTimes.clear();
    qualityLocalShadowCacheTimes.clear();qualityStaticGeometryHash=0;qualityCachedLightCount=0;qualityCacheBuilds=0;
    std::fill(std::begin(qualityCachedLightIds),std::end(qualityCachedLightIds),0);
}

bool Renderer::Impl::prepareQuality(unsigned frame,const Mat4& vp,const float eye[3],
    const std::vector<DrawItem>& items,const std::vector<LightDesc>& lights,bool tlasValid,std::string& error) {
    auto* list=commands;
    auto stamp=[&](unsigned which){if(timestampsAvailable)list->EndQuery(timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,frame*detail::TimestampsPerFrame+which);};
    stamp(detail::TimestampQualityBegin);
    auto& q=qualityParams;q={};
    if(!invertMatrix(vp,q.inverseViewProjection)){error="quality view projection is singular";return false;}
    q.viewProjection=vp;q.previousViewProjection=qualityPreviousViewProjection;
    std::memcpy(q.eyeFrame,eye,12);q.eyeFrame[3]=static_cast<float>(qualityFrames);
    const float center[3]={0,0,1};float farPoint[3];transformPoint(q.inverseViewProjection,center,farPoint);
    for(unsigned i=0;i<3;i++)q.forwardHistory[i]=farPoint[i]-eye[i];normalize3(q.forwardHistory);
    float movement=0,direction=0;
    for(unsigned i=0;i<3;i++) {
        movement+=(eye[i]-qualityPreviousEye[i])*(eye[i]-qualityPreviousEye[i]);
        direction+=q.forwardHistory[i]*qualityPreviousForward[i];
    }
    bool reset=!qualityInitialized||qualityEnvironmentDirty;
    if(!qualityInitialized) {
        qualityProbeOrigin[0]=std::floor(eye[0]/4)*4-28;
        qualityProbeOrigin[1]=-2;qualityProbeOrigin[2]=std::floor(eye[2]/4)*4-28;
    }
    if(std::abs(eye[0]-qualityProbeOrigin[0]-30)>26||std::abs(eye[2]-qualityProbeOrigin[2]-30)>26) {
        qualityProbeOrigin[0]=std::floor(eye[0]/4)*4-28;
        qualityProbeOrigin[2]=std::floor(eye[2]/4)*4-28;reset=true;
    }
    q.forwardHistory[3]=qualityHistoryValid&&environment.temporal&&!reset&&movement<4&&direction>0.75f?1.0f:0.0f;
    if(!q.forwardHistory[3])++qualityHistoryResets;
    std::memcpy(q.previousEye,qualityPreviousEye,12);std::memcpy(q.previousForward,qualityPreviousForward,12);
    std::memcpy(q.keyDirectionIntensity,environment.keyDirection,12);normalize3(q.keyDirectionIntensity);
    q.keyDirectionIntensity[3]=environment.keyIntensity;std::memcpy(q.keyColor,environment.keyColor,12);
    std::memcpy(q.skyExposure,environment.skyColor,12);q.skyExposure[3]=environment.exposure;
    std::memcpy(q.ambient,environment.ambient,12);
    q.fog[0]=environment.fogDensity;q.fog[1]=environment.fogHeightFalloff;q.fog[2]=environment.fogBaseHeight;q.fog[3]=96;
    q.cloud[0]=environment.cloudCoverage;q.cloud[1]=environment.cloudHeight;q.cloud[2]=environment.cloudThickness;
    q.cloud[3]=static_cast<float>(qualityFrames)/60;q.wind[0]=environment.cloudSpeed*10;q.wind[2]=environment.cloudSpeed*2;q.wind[3]=1.0f/60;
    std::memcpy(q.probeOriginSpacing,qualityProbeOrigin,12);q.probeOriginSpacing[3]=4;
    q.probeCountsHistory[0]=16;q.probeCountsHistory[1]=8;q.probeCountsHistory[2]=16;q.probeCountsHistory[3]=0.90f;
    for(unsigned i=0;i<3;i++)q.grid[i]=qualityGrid[i];q.grid[3]=qualityFrames;
    q.screen[0]=width;q.screen[1]=height;q.screen[2]=(width+1)/2;q.screen[3]=(height+1)/2;
    unsigned current=qualityFrames%2,previous=1-current,probe=detail::QProbe+current*6,oldProbe=detail::QProbe+previous*6;
    auto row=[&](unsigned r,unsigned a,unsigned b,unsigned c,unsigned d){q.slots[r][0]=a;q.slots[r][1]=b;q.slots[r][2]=c;q.slots[r][3]=d;};
    row(0,detail::FrameDescriptorBase+frame*detail::FrameDescriptorStride+detail::FdsTopLevelAccelerationStructure,slot(detail::QInstances+frame),slot(detail::QIbl),slot(detail::QBrdf));
    row(1,slot(probe),slot(probe+1),slot(probe+2),slot(oldProbe));
    row(2,slot(oldProbe+1),slot(oldProbe+2),slot(detail::QFogHistory+previous*2),slot(detail::QFogHistory+current*2+1));
    row(3,slot(detail::QFogScratch+1),slot(detail::QFogScratch),slot(detail::QFogIntegrated+1),slot(detail::QFogIntegrated));
    row(4,slot(detail::QNoise),slot(detail::QNoise+1),slot(detail::QWeather),slot(detail::QWeather+1));
    row(5,slot(detail::QCloudShadow),slot(detail::QCloudShadow+1),slot(detail::QCloud+current*4+1),slot(detail::QCloud+current*4));
    row(6,slot(detail::QCloud+previous*4),slot(detail::QCloud+current*4+3),slot(detail::QCloud+current*4+2),slot(detail::QCloud+previous*4+2));
    const unsigned probeUpdates=environment.quality>1?256:64;
    row(7,slot(probe+3),slot(probe+4),slot(probe+5),(qualityFrames%(2048/probeUpdates))*probeUpdates);
    unsigned base=detail::FrameDescriptorBase+frame*detail::FrameDescriptorStride;
    row(8,base+detail::FdsBaseColor,base+detail::FdsKeyLight,base+detail::FdsShadowMaskSrv,base+detail::FdsDepth);
    row(9,slot(detail::QIblMipUav),slot(detail::QBrdfUav),compositeDebugView,
        (environment.ibl?1u:0u)|(environment.ddgi&&environment.quality>1&&tlasValid?2u:0u)|(environment.cloudShadows?4u:0u)|
        (environment.toneMap?8u:0u));
    unsigned count=static_cast<unsigned>(std::min<size_t>(lights.size(),64));
    row(10,tlasValid?1u:0u,static_cast<unsigned>(std::min<size_t>(items.size(),detail::MaxInstances)),count,reset?1u:0u);
    row(11,environment.quality,probeUpdates,environment.quality>1?64:24,environment.quality>1?12:4);
    row(14,slot(detail::QEnvironment),environment.panorama&&environmentTexture?1u:0u,slot(detail::QInlineMaterials),qualityFogIncident?detail::SmokeIncidentSrv:0);
    const float rotation=std::remainder(environment.environmentRotation,360.0f)*Pi/180;q.sourceEnvironment[0]=std::cos(rotation);q.sourceEnvironment[1]=std::sin(rotation);q.sourceEnvironment[2]=environment.environmentIntensity;
    q.cloudShadowOrigin[0]=std::floor(eye[0]/2)*2;q.cloudShadowOrigin[1]=std::floor(eye[2]/2)*2;q.cloudShadowOrigin[2]=512;
    q.smoke=smokeScene;
    q.volumeInfo[0]=static_cast<unsigned>(std::min<size_t>(environment.fogVolumes.size(),8));
    for(unsigned i=0;i<q.volumeInfo[0];i++) {
        const auto& source=environment.fogVolumes[i];
        std::memcpy(q.volumeCenters[i],source.center,12);q.volumeCenters[i][3]=source.density;
        std::memcpy(q.volumeExtents[i],source.extent,12);
    }
    // Integrate the same procedural sky used by the GGX prefilter into irradiance SH.
    if(environment.panorama){for(unsigned i=0;i<9;i++)for(unsigned c=0;c<3;c++)q.diffuseSH[i][c]=environment.panorama->diffuseSH[i][c];}
    else for(unsigned sample=0;sample<512;sample++) {
        float z=1-2*(sample+0.5f)/512,phi=sample*2.39996323f,r=std::sqrt(1-z*z);
        float x=r*std::cos(phi),y=r*std::sin(phi);
        float basis[9]={0.282095f,0.488603f*y,0.488603f*z,0.488603f*x,
            1.092548f*x*y,1.092548f*y*z,0.315392f*(3*z*z-1),1.092548f*x*z,0.546274f*(x*x-y*y)};
        float factor=y<0?0.12f:0.45f+1.35f*std::sqrt(y);
        for(unsigned b=0;b<9;b++)for(unsigned channel=0;channel<3;channel++)
            q.diffuseSH[b][channel]+=environment.skyColor[channel]*factor*basis[b]*(4*Pi/512)*(b==0?Pi:b<4?2*Pi/3:Pi/4);
    }
    struct Candidate{unsigned id,index;float score;};std::vector<Candidate> candidates;
    for(unsigned i=0;i<count;i++) {
        const auto& source=lights[i];auto& target=q.lights[i];
        std::memcpy(target.positionRange,source.position,12);target.positionRange[3]=std::max(source.range,0.001f);
        std::memcpy(target.colorIntensity,source.color,12);target.colorIntensity[3]=source.intensity;
        std::memcpy(target.directionCos,source.direction,12);normalize3(target.directionCos);target.directionCos[3]=source.cosOuter;
        target.params[0]=static_cast<float>(source.type);target.params[1]=source.cosInner;
        target.shadow[1]=source.castVolumetricShadow?1.0f:0.0f;
        target.shadow[2]=source.volumetricIntensity;target.shadow[3]=static_cast<float>(source.stableId?source.stableId:i+1);
        float d2=0;for(unsigned k=0;k<3;k++)d2+=(source.position[k]-eye[k])*(source.position[k]-eye[k]);
        float luminance=source.color[0]*0.2126f+source.color[1]*0.7152f+source.color[2]*0.0722f;
        unsigned id=source.stableId?source.stableId:i+1;
        float score=luminance*source.intensity*source.range*source.range/(d2+4);
        for(unsigned j=0;j<qualityShadowCount;j++)if(qualityShadowIds[j]==id)score*=1.25f;
        if(source.castSurfaceShadow&&environment.localShadows)candidates.push_back({id,i,score});
    }
    UINT64 geometryHash=1469598103934665603ull;
    for(const auto& item:items)if(item.rayVisibilityMask&1u) {
        for(float component:item.model.m) {
            unsigned bits=0;std::memcpy(&bits,&component,sizeof(bits));
            geometryHash^=bits;geometryHash*=1099511628211ull;
        }
        geometryHash^=item.rayVisibilityMask;geometryHash*=1099511628211ull;
    }
    bool geometryChanged=!qualityInitialized||geometryHash!=qualityStaticGeometryHash;
    unsigned cacheCount=0,dirtyMask=0;
    for(unsigned i=0;i<count&&cacheCount<4&&tlasValid;i++) {
        if(!lights[i].isStatic||(!lights[i].castSurfaceShadow&&!lights[i].castVolumetricShadow))continue;
        unsigned id=lights[i].stableId?lights[i].stableId:i+1;
        if(geometryChanged||qualityCachedLightIds[cacheCount]!=id||
           std::memcmp(qualityCachedLightPositions[cacheCount],q.lights[i].positionRange,16)!=0)
            dirtyMask|=1u<<cacheCount;
        qualityCachedLightIds[cacheCount]=id;
        std::memcpy(qualityCachedLightPositions[cacheCount],q.lights[i].positionRange,16);
        q.slots[13][cacheCount]=i;q.lights[i].params[2]=static_cast<float>(cacheCount+1);
        ++cacheCount;
    }
    row(12,slot(detail::QLocalShadowCache),slot(detail::QLocalShadowCache+1),cacheCount,dirtyMask);
    qualityCachedLightCount=cacheCount;qualityStaticGeometryHash=geometryHash;
    std::stable_sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b){return a.score==b.score?a.id<b.id:a.score>b.score;});
    const unsigned shadowBudget=std::min(environment.maxShadowLights,environment.quality>1?4u:2u);
    unsigned desired=std::min<unsigned>(shadowBudget,static_cast<unsigned>(candidates.size()));
    std::array<unsigned,4> ids{};std::array<float,4> weights{};
    unsigned selected=0;
    for(unsigned old=0;old<qualityShadowCount;old++) {
        bool wanted=false;unsigned index=count;
        for(unsigned i=0;i<desired;i++)if(candidates[i].id==qualityShadowIds[old])wanted=true;
        for(unsigned i=0;i<count;i++)if((lights[i].stableId?lights[i].stableId:i+1)==qualityShadowIds[old])index=i;
        float weight=std::clamp(qualityShadowWeights[old]+(wanted?0.125f:-0.125f),0.0f,1.0f);
        if(weight>0&&index<count&&selected<shadowBudget) {
            ids[selected]=qualityShadowIds[old];weights[selected++]=weight;q.lights[index].shadow[0]=weight;
        }
    }
    for(unsigned i=0;i<desired&&selected<shadowBudget;i++) {
        bool exists=false;for(unsigned j=0;j<selected;j++)if(ids[j]==candidates[i].id)exists=true;
        if(!exists){ids[selected]=candidates[i].id;weights[selected++]=0.125f;q.lights[candidates[i].index].shadow[0]=0.125f;}
    }
    qualityShadowCount=selected;std::copy(ids.begin(),ids.end(),qualityShadowIds);std::copy(weights.begin(),weights.end(),qualityShadowWeights);
    auto* instances=static_cast<detail::QualityInstance*>(qualityInstancesMapped[frame]);
    for(unsigned i=0;i<q.slots[10][1];i++) {
        auto& target=instances[i];target={};const auto& source=items[i];
        invertMatrix(source.model,target.inverseModel);std::memcpy(target.color,source.color,16);
        target.material[0]=source.roughness;target.material[1]=source.metalness;target.material[2]=1;
        if(!configuredMaterials.empty()) {
            unsigned material=source.material<0?0u:static_cast<unsigned>(source.material);
            if(material>=configuredMaterials.size())material=0;
            const auto& entry=configuredMaterials[material];
            target.textures[0]=detail::CookedTextureSrvBase+entry.albedo;
            target.textures[1]=detail::CookedTextureSrvBase+entry.orm;
            target.textures[2]=detail::CookedTextureSrvBase+entry.normal;
            target.textures[3]=1u|(static_cast<unsigned>(std::clamp(entry.wear,0.0f,1.0f)*255)<<8);
            target.material[0]=entry.roughness;target.material[1]=entry.metalness;target.material[2]=entry.uvScale;
            target.material[3]=entry.wetness;
        }
    }
    std::memcpy(static_cast<unsigned char*>(qualityConstantsMapped)+frame*detail::QualityCbStride,&q,sizeof(q));
    ID3D12DescriptorHeap* heaps[]={srvHeap};list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(qualityRoot);
    list->SetComputeRootConstantBufferView(0,qualityConstantsBuffer->GetGPUVirtualAddress()+frame*detail::QualityCbStride);
    const unsigned dispatch[4]={0,0,0,0};list->SetComputeRoot32BitConstants(1,4,dispatch,0);
    if(!qualityInitialized) {
        transitionTo(qualityNoise,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);transitionTo(qualityWeather,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
        list->SetPipelineState(qualityPipelines[0]);list->Dispatch(16,16,16);
        transitionTo(qualityNoise,ReadState,list);transitionTo(qualityWeather,ReadState,list);
        transitionTo(qualityBrdf,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
        list->SetPipelineState(qualityPipelines[2]);list->Dispatch(8,8,1);transitionTo(qualityBrdf,ReadState,list);
    }
    if(!qualityInitialized||qualityEnvironmentDirty) {
        transitionTo(qualityIbl,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);list->SetPipelineState(qualityPipelines[1]);
        for(unsigned mip=0;mip<7;mip++) {
            list->SetComputeRoot32BitConstant(1,mip,0);unsigned size=std::max(64u>>mip,1u);
            list->Dispatch((size+7)/8,(size+7)/8,6);
        }
        transitionTo(qualityIbl,ReadState,list);list->SetComputeRoot32BitConstant(1,0,0);
    }
    transitionTo(qualityCloudShadow,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
    list->SetPipelineState(qualityPipelines[4]);list->Dispatch(16,16,1);transitionTo(qualityCloudShadow,ReadState,list);
    stamp(detail::TimestampCloudShadowEnd);
    if(dirtyMask) {
        transitionTo(qualityLocalShadowCache,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
        list->SetPipelineState(qualityPipelines[9]);list->Dispatch(16,16,cacheCount*6);
        for(unsigned i=0;i<cacheCount;i++)if(dirtyMask&(1u<<i))++qualityCacheBuilds;
    }
    if(cacheCount)transitionTo(qualityLocalShadowCache,ReadState,list);
    stamp(detail::TimestampLocalShadowCacheEnd);
    if(reset&&environment.ddgi&&environment.quality>1&&tlasValid) {
        for(unsigned i=0;i<2;i++) {
            transitionTo(qualityProbeIrradiance[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
            transitionTo(qualityProbeDistance[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
            transitionTo(qualityProbeData[i],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
        }
        list->SetPipelineState(qualityPipelines[8]);list->Dispatch(16,128,1);
        for(unsigned i=0;i<2;i++) {
            uavBarrier(list,qualityProbeIrradiance[i].resource);uavBarrier(list,qualityProbeDistance[i].resource);uavBarrier(list,qualityProbeData[i].resource);
        }
    }
    auto copyHistory=[&](TrackedResource& destination,TrackedResource& source) {
        transitionTo(source,D3D12_RESOURCE_STATE_COPY_SOURCE,list);transitionTo(destination,D3D12_RESOURCE_STATE_COPY_DEST,list);
        list->CopyResource(destination.resource,source.resource);transitionTo(source,ReadState,list);
        transitionTo(destination,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
    };
    if(environment.ddgi&&environment.quality>1&&tlasValid) {
        copyHistory(qualityProbeIrradiance[current],qualityProbeIrradiance[previous]);
        copyHistory(qualityProbeDistance[current],qualityProbeDistance[previous]);copyHistory(qualityProbeData[current],qualityProbeData[previous]);
        list->SetPipelineState(qualityPipelines[3]);list->Dispatch(probeUpdates,1,1);
    }
    transitionTo(qualityProbeIrradiance[current],ReadState,list);transitionTo(qualityProbeDistance[current],ReadState,list);transitionTo(qualityProbeData[current],ReadState,list);
    stamp(detail::TimestampProbeEnd);
    qualityEnvironmentDirty=false;qualityInitialized=true;qualityHistoryValid=true;
    qualityPreviousViewProjection=vp;std::memcpy(qualityPreviousEye,eye,12);std::memcpy(qualityPreviousForward,q.forwardHistory,12);
    return true;
}

void Renderer::Impl::dispatchQualityVolumes(unsigned frame) {
    auto* list=commands;unsigned current=qualityFrames%2,previous=1-current;
    auto stamp=[&](unsigned which){if(timestampsAvailable)list->EndQuery(timestampHeap,D3D12_QUERY_TYPE_TIMESTAMP,frame*detail::TimestampsPerFrame+which);};
    list->SetComputeRootSignature(qualityRoot);list->SetComputeRootConstantBufferView(0,
        qualityConstantsBuffer->GetGPUVirtualAddress()+frame*detail::QualityCbStride);
    unsigned dispatch[4]{};list->SetComputeRoot32BitConstants(1,4,dispatch,0);
    stamp(detail::TimestampFogBegin);
    transitionTo(qualityFogHistory[previous],ReadState,list);
    transitionTo(qualityFogHistory[current],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
    transitionTo(qualityFogScratch,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
    if(qualityFogIncident)transitionTo(qualityFogIncident,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
    list->SetPipelineState(qualityPipelines[5]);list->Dispatch((qualityGrid[0]+3)/4,(qualityGrid[1]+3)/4,(qualityGrid[2]+3)/4);
    transitionTo(qualityFogScratch,ReadState,list);transitionTo(qualityFogHistory[current],ReadState,list);
    if(qualityFogIncident)transitionTo(qualityFogIncident,ReadState,list);
    transitionTo(qualityFogIntegrated,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
    list->SetPipelineState(qualityPipelines[6]);list->Dispatch((qualityGrid[0]+7)/8,(qualityGrid[1]+7)/8,1);
    transitionTo(qualityFogIntegrated,ReadState,list);stamp(detail::TimestampFogEnd);
    transitionTo(qualityCloud[previous],ReadState,list);transitionTo(qualityCloudDepth[previous],ReadState,list);
    transitionTo(qualityCloud[current],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
    transitionTo(qualityCloudDepth[current],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,list);
    list->SetPipelineState(qualityPipelines[7]);list->Dispatch(((width+1)/2+7)/8,((height+1)/2+7)/8,1);
    transitionTo(qualityCloud[current],ReadState,list);transitionTo(qualityCloudDepth[current],ReadState,list);
    stamp(detail::TimestampCloudEnd);
}

void Renderer::Impl::compositeQuality(unsigned frame) {
    commands->SetGraphicsRootSignature(qualityRoot);commands->SetPipelineState(qualityCompositePipeline);
    commands->SetGraphicsRootConstantBufferView(0,qualityConstantsBuffer->GetGPUVirtualAddress()+frame*detail::QualityCbStride);
    const auto base=detail::FrameDescriptorBase+frame*detail::FrameDescriptorStride;
    unsigned dispatch[4]={0,base+detail::FdsTransparencyHeads,base+detail::FdsTransparencyNodes,transparencyActive?1u:0u};commands->SetGraphicsRoot32BitConstants(1,4,dispatch,0);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands->DrawInstanced(3,1,0,0);
    ++qualityFrames;
}
std::string Renderer::Impl::qualityJson() const {
    auto timing=[](const std::vector<double>& values) {
        std::vector<double> sorted=values;std::sort(sorted.begin(),sorted.end());
        auto percentile=[&](double fraction){return sorted.empty()?0.0:sorted[static_cast<size_t>((sorted.size()-1)*fraction)];};
        std::ostringstream out;out<<"{\"p50Ms\":"<<percentile(0.5)<<",\"p95Ms\":"<<percentile(0.95)<<",\"samples\":"<<sorted.size()<<"}";return out.str();
    };
    std::ostringstream out;out<<"{\"requested\":"<<environment.quality<<",\"implemented\":"<<(environment.quality&&qualityReady&&qualityFrames?"true":"false")
        <<",\"frames\":"<<qualityFrames<<",\"froxel\":["<<qualityGrid[0]<<","<<qualityGrid[1]<<","<<qualityGrid[2]<<"]"
        <<",\"shadowLights\":"<<qualityShadowCount<<",\"ibl\":"<<(environment.ibl&&qualityFrames?"true":"false")
        <<",\"environmentMap\":{\"active\":"<<(environment.panorama&&environmentTexture?"true":"false")<<",\"sha256\":"<<detail::jsonString(environmentTextureHash)<<",\"width\":"<<(environment.panorama?environment.panorama->width:0)<<",\"height\":"<<(environment.panorama?environment.panorama->height:0)<<",\"rotation\":"<<environment.environmentRotation<<",\"intensity\":"<<environment.environmentIntensity<<",\"radianceFormat\":\"RGBA32F\",\"diffuse\":\"SH9 cosine-convolved irradiance\"}"
        <<",\"ddgi\":"<<(environment.ddgi&&environment.quality>1&&qualityFrames?"true":"false")<<",\"probeCount\":2048,\"probeUpdatesPerFrame\":"<<qualityParams.slots[11][1]<<",\"raysPerProbe\":64"
        <<",\"probeHitLighting\":\"rigid cube materials, direct visibility, emissive, sky SH; one diffuse bounce\""
        <<",\"cloudSelfShadow\":"<<(environment.cloudShadows&&qualityFrames?"true":"false")<<",\"cloudTemporal\":"<<(environment.temporal&&qualityFrames?"true":"false")
        <<",\"historyResets\":"<<qualityHistoryResets<<",\"textureBytes\":"<<qualityTextureBytes
        <<",\"localFogVolumes\":"<<qualityParams.volumeInfo[0]
        <<",\"staticShadowCache\":{\"lights\":"<<qualityCachedLightCount<<",\"builds\":"<<qualityCacheBuilds
        <<",\"resolution\":128,\"facesPerLight\":6,\"dynamicVisibility\":\"current TLAS mask 2\"}"
        <<",\"gpuProbeMs\":"<<qualityProbeMs<<",\"gpuCloudShadowMs\":"<<qualityCloudShadowMs
        <<",\"gpuFogMs\":"<<qualityFogMs<<",\"gpuCloudMs\":"<<qualityCloudMs
        <<",\"gpuPasses\":{\"ddgi\":"<<timing(qualityProbeTimes)<<",\"cloudShadow\":"<<timing(qualityCloudShadowTimes)
        <<",\"froxel\":"<<timing(qualityFogTimes)<<",\"cloud\":"<<timing(qualityCloudTimes)
        <<",\"localShadowCache\":"<<timing(qualityLocalShadowCacheTimes)<<"}"
        <<",\"geometry\":\"rigid boxes only; skinned actors excluded from TLAS\",\"error\":"<<detail::jsonString(qualityError)<<"}";
    return out.str();
}
} // namespace nativegpu
