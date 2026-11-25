#include "d3d12_renderer_internal.h"
#include <cstring>
#include <cmath>
namespace nativegpu {
bool Renderer::Impl::prepareSmokeMedia(ID3D12GraphicsCommandList* list,unsigned frame,const float camera[3],std::string& error){
    smokeScene={};smokePixelReads.clear();smokeFrameSources.clear();if(gpuSmokeService)gpuSmokeService->collect();
    const auto outputs=gpuSmokeService?gpuSmokeService->outputs():std::vector<engine::smoke::ServiceOutput>{};
    const auto shared=gpuSmokeSharing?gpuSmokeSharing->media():std::vector<engine::smoke::SharedDensityMedium>{};
    if(!outputs.empty()&&!compositeMediaActive&&!(environment.quality&&qualityReady)){error="NG-E-SMOKE-RENDER: no compatible optical pipeline";return false;}
    if(outputs.size()>detail::MaxSmokeMedia){error="NG-E-SMOKE-RENDER: optical source budget exceeded";return false;}
    for(unsigned i=0;i<detail::MaxSmokeMedia;++i){D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R32_TYPELESS;view.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Buffer.NumElements=1;view.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
        ID3D12Resource* resource=nullptr;if(i<outputs.size()){const auto& out=outputs[i];const auto& b=out.buffers;auto& v=smokeScene.volumes[i];bool coarse=false;
            const float dx=camera[0]-(b.spec.originMetres.x+b.spec.dimensions[0]*b.spec.cellMetres*.5f),dy=camera[1]-(b.spec.originMetres.y+b.spec.dimensions[1]*b.spec.cellMetres*.5f),dz=camera[2]-(b.spec.originMetres.z+b.spec.dimensions[2]*b.spec.cellMetres*.5f);
            if(dx*dx+dy*dy+dz*dz>16.f*16.f&&(!(b.spec.channels&engine::smoke::Temperature)||out.optics.emissionPerDensity==0))for(const auto& candidate:shared)if(candidate.serial==out.serial&&candidate.inputVersion==b.version){
                if(!candidate.projection->markUsed(nextFence)){error="NG-E-SMOKE-RENDER: shared proxy read fence";return false;}const auto p=candidate.projection->bindings();const auto& plane=p.planes.at(0);resource=plane.values;view.Buffer.NumElements=static_cast<UINT>(resource->GetDesc().Width/4);const float grid[]={p.grid.originMetres.x,p.grid.originMetres.y,p.grid.originMetres.z};const int origin[]={p.region.min.x,p.region.min.y,p.region.min.z},high[]={p.region.max.x,p.region.max.y,p.region.max.z};for(unsigned a=0;a<3;++a){v.originCell[a]=grid[a]+origin[a]*p.grid.worldCellMetres;v.dimensions[a]=high[a]-origin[a];}v.originCell[3]=p.grid.worldCellMetres;v.dimensions[3]=1;v.offsets[0]=0;v.offsets[1]=v.offsets[2]=UINT32_MAX;v.optics[3]=1.f/plane.definition.unitsPerInteger;coarse=true;++smokeScene.info[1];break;
            }
            if(!coarse){if(!gpuSmokeService->prepareRead(out.serial,list,nextFence,error))return false;resource=b.scalars;view.Buffer.NumElements=b.layout.scalarCount;v.originCell[0]=b.spec.originMetres.x;v.originCell[1]=b.spec.originMetres.y;v.originCell[2]=b.spec.originMetres.z;v.originCell[3]=b.spec.cellMetres;for(unsigned a=0;a<3;++a){v.dimensions[a]=b.spec.dimensions[a];v.offsets[a]=b.layout.offsets[a];}}
            v.offsets[3]=detail::SmokeDescriptorBase+frame*detail::MaxSmokeMedia+i;v.albedoEmission[0]=out.optics.scatteringAlbedo.x;v.albedoEmission[1]=out.optics.scatteringAlbedo.y;v.albedoEmission[2]=out.optics.scatteringAlbedo.z;v.albedoEmission[3]=out.optics.emissionPerDensity;v.optics[0]=out.optics.extinctionPerDensity;v.optics[1]=out.optics.emissionStartKelvin;v.optics[2]=b.spec.ambientKelvin;
            auto barrier=detail::transitionBarrier(resource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));list->ResourceBarrier(1,&barrier);smokePixelReads.push_back(resource);smokeFrameSources.push_back({out.serial,b.sourceTick,b.version,coarse,view.Buffer.NumElements,v.originCell[3]});
        }device->CreateShaderResourceView(resource,&view,cpuDescriptor(detail::SmokeDescriptorBase+frame*detail::MaxSmokeMedia+i));
    }
    smokeScene.info[0]=static_cast<unsigned>(outputs.size());if(!outputs.empty()){++smokeMediaFrames;smokeMediaInstances+=outputs.size();}if(smokeScene.info[1]){++smokeSharedFrames;smokeSharedInstances+=smokeScene.info[1];}return true;
}
void Renderer::Impl::finishSmokeMedia(ID3D12GraphicsCommandList* list){
    for(auto* resource:smokePixelReads){auto b=detail::transitionBarrier(resource,static_cast<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);}smokePixelReads.clear();
}
}
