#include "particle_source.hpp"
#include "rate_view_internal.hpp"
#include "nativegpu_shaders.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

namespace engine::smoke {
namespace {
void check(HRESULT code,const char* why){if(FAILED(code))throw std::runtime_error(std::string("NG-E-SMOKE-PARTICLES: ")+why);}
std::uint32_t bits(float value){std::uint32_t result;std::memcpy(&result,&value,4);return result;}
D3D12_RESOURCE_DESC description(std::uint64_t bytes){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;return d;}
std::uint64_t allocation(ID3D12Device* device,std::uint64_t bytes){const auto d=description(bytes);return device->GetResourceAllocationInfo(0,1,&d).SizeInBytes;}
void make(ID3D12Device* device,RateCom<ID3D12Resource>& resource,std::uint64_t bytes){D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;const auto d=description(bytes);check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(resource.out())),"Create rate buffer");}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resource;b.Transition.StateBefore=before;b.Transition.StateAfter=after;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;list->ResourceBarrier(1,&b);}
}
std::uint64_t particleSourceFrameBytes(ID3D12Device* d,const Spec& s){if(!d)throw std::invalid_argument("NG-E-SMOKE-PARTICLES: missing footprint device");return allocation(d,std::uint64_t(layout(s).scalarCount)*4)*2+allocation(d,48);}
struct GpuParticleSmokeSource::Impl {
    RateCom<ID3D12Device> device;RateCom<ID3D12Fence> timeline;RateCom<ID3D12RootSignature> root;
    std::array<RateCom<ID3D12PipelineState>,3> pipelines;
    Spec spec;Layout grid;ParticleSourceQuantization quantization;ParticleSourceLimits limits;
    std::vector<std::shared_ptr<GpuSmokeRateView>> views;ParticleSourceStats counters;
    Impl(ID3D12Device* d,ID3D12Fence* f,Spec s,ParticleSourceQuantization q,ParticleSourceLimits l):spec(s),grid(layout(s)),quantization(q),limits(l){
        if(!d||!f||!l.maxAllocationBytes||!l.maxFrames||l.maxFrames>64||!l.maxBatches||l.maxBatches>64||!l.maxParticlesPerBatch||l.maxParticlesPerBatch>1000000||!nativegpu::shaders::kDxcAvailable)throw std::invalid_argument("NG-E-SMOKE-PARTICLES: invalid device/limits/shaders");
        for(auto units:q.unitsPerSource)if(!units||units>1000000000)throw std::invalid_argument("NG-E-SMOKE-PARTICLES: invalid source units");
        RateCom<ID3D12Device> owner;check(f->GetDevice(IID_PPV_ARGS(owner.out())),"Query timeline");if(owner.p!=d)throw std::invalid_argument("NG-E-SMOKE-PARTICLES: timeline device mismatch");device.p=d;d->AddRef();timeline.p=f;f->AddRef();views.reserve(l.maxFrames);
        D3D12_ROOT_PARAMETER p[7]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[0].Constants.Num32BitValues=32;for(unsigned i=0;i<3;++i){p[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[i+1].Descriptor.ShaderRegister=i;p[i+4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[i+4].Descriptor.ShaderRegister=i;}
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=7;rd.pParameters=p;RateCom<ID3DBlob> bytes,errors;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,bytes.out(),errors.out()),"Serialize source root");check(d->CreateRootSignature(0,bytes.p->GetBufferPointer(),bytes.p->GetBufferSize(),IID_PPV_ARGS(root.out())),"Create source root");
        const nativegpu::shaders::Blob* blobs[]={&nativegpu::shaders::kSmokeParticleClear,&nativegpu::shaders::kSmokeParticleSplat,&nativegpu::shaders::kSmokeParticleResolve};for(unsigned i=0;i<3;++i){D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.p;pd.CS={blobs[i]->data,blobs[i]->size};check(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(pipelines[i].out())),"Create source pipeline");}
    }
    void collect(){const auto done=timeline.p->GetCompletedValue();for(auto& v:views)v->impl_->collect();views.erase(std::remove_if(views.begin(),views.end(),[&](const auto& v){return v.use_count()==1&&v->lastFence()<=done;}),views.end());}
    ParticleSourceStats stats() const{auto out=counters;out.views=static_cast<unsigned>(views.size());for(const auto& v:views){out.allocationBytes+=v->impl_->bytes;out.heldInputReferences+=static_cast<unsigned>(v->impl_->inputs.size()*4);}return out;}
    bool valid(ID3D12GraphicsCommandList* list,std::uint64_t fence) const{if(!list||!fence||fence<counters.lastFence||fence<=timeline.p->GetCompletedValue()||(list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT&&list->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE))return false;RateCom<ID3D12Device> owner;return SUCCEEDED(list->GetDevice(IID_PPV_ARGS(owner.out())))&&owner.p==device.p;}
    std::array<std::uint32_t,32> constants() const{std::array<std::uint32_t,32> c{};for(unsigned a=0;a<3;++a){c[a]=spec.dimensions[a];c[4+a]=grid.offsets[a];c[24+a]=bits(static_cast<float>(1.0/(quantization.unitsPerSource[a]*std::pow(double(spec.cellMetres),3))));}c[3]=grid.cells;c[7]=grid.scalarCount;c[8]=bits(spec.originMetres.x);c[9]=bits(spec.originMetres.y);c[10]=bits(spec.originMetres.z);c[11]=bits(1/spec.cellMetres);return c;}
    void bind(ID3D12GraphicsCommandList* list,GpuSmokeRateView::Impl& v,const std::array<std::uint32_t,32>& c){list->SetComputeRootSignature(root.p);list->SetComputeRoot32BitConstants(0,32,c.data(),0);ID3D12Resource* buffers[]={v.accumulators.p,v.rates.p,v.diagnostics.p};for(unsigned i=0;i<3;++i){list->SetComputeRootShaderResourceView(i+1,v.accumulators.p->GetGPUVirtualAddress());list->SetComputeRootUnorderedAccessView(i+4,buffers[i]->GetGPUVirtualAddress());}}
    void dispatch(ID3D12GraphicsCommandList* list,unsigned pipeline,unsigned count){const auto groups=(count+63)/64;list->SetPipelineState(pipelines[pipeline].p);list->Dispatch(std::min(groups,65535u),(groups+65534)/65535,1);D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;list->ResourceBarrier(1,&b);++counters.dispatches;}
};
GpuParticleSmokeSource::GpuParticleSmokeSource(ID3D12Device* d,ID3D12Fence* f,Spec s,ParticleSourceQuantization q,ParticleSourceLimits l):impl_(std::make_unique<Impl>(d,f,s,q,l)){}
GpuParticleSmokeSource::~GpuParticleSmokeSource()=default;
std::shared_ptr<GpuSmokeRateView> GpuParticleSmokeSource::produce(const std::vector<ParticleSourceBatch>& batches,ID3D12GraphicsCommandList* list,std::uint64_t fence,std::uint64_t tick,std::string& error){
    error.clear();if(!impl_){error="NG-E-SMOKE-PARTICLES: stopped";return {};}auto& s=*impl_;const auto reject=[&](const char* why)->std::shared_ptr<GpuSmokeRateView>{++s.counters.rejections;error=std::string("NG-E-SMOKE-PARTICLES: ")+why;return {};};
    if(s.counters.failed||!s.valid(list,fence)||tick==UINT64_MAX||(s.counters.frames&&tick<=s.counters.lastTick)||batches.size()>s.limits.maxBatches)return reject("failed recorder/list/fence/tick/batch capacity");
    bool recording=false;
    try{
        std::set<effects::GpuParticlePool*> seen;std::array<std::uint64_t,3> totals{};
        for(const auto& batch:batches){if(!batch.pool||!seen.insert(batch.pool).second)return reject("null or duplicate pool");const auto b=batch.pool->buffers();const auto state=batch.pool->stats();if(!state.initialized||state.lastFence>fence||!b.attributes||!b.alive||!b.counters||!b.drawArguments||!b.capacity||b.capacity>s.limits.maxParticlesPerBatch)return reject("invalid pool capacity/state");
            for(auto* resource:{b.attributes,b.alive,b.counters,b.drawArguments}){RateCom<ID3D12Device> owner;if(FAILED(resource->GetDevice(IID_PPV_ARGS(owner.out())))||owner.p!=s.device.p)return reject("pool device mismatch");}
            bool nonzero=false;for(unsigned a=0;a<3;++a){nonzero|=batch.quantum[a]!=0;if(batch.quantum[a]&&s.grid.offsets[a]==UINT32_MAX)return reject("source for disabled channel");const auto amount=std::uint64_t(b.capacity)*batch.quantum[a];if(amount>UINT32_MAX||totals[a]>UINT32_MAX-amount)return reject("uint32 contribution bound");totals[a]+=amount;}if(!nonzero)return reject("zero batch; omit inactive sources");
        }
        const double volume=std::pow(double(s.spec.cellMetres),3);for(unsigned a=0;a<3;++a)if(totals[a]/(s.quantization.unitsPerSource[a]*volume)>1000000.0)return reject("per-cell rate bound");
        s.collect();const auto scalarBytes=std::uint64_t(s.grid.scalarCount)*4;const auto accumulatorBytes=allocation(s.device.p,scalarBytes);const auto bytes=accumulatorBytes*2+allocation(s.device.p,48);if(s.views.size()>=s.limits.maxFrames||bytes>s.limits.maxAllocationBytes||s.stats().allocationBytes>s.limits.maxAllocationBytes-bytes)return reject("immutable rate snapshot budget");
        auto view=std::shared_ptr<GpuSmokeRateView>(new GpuSmokeRateView);view->impl_=std::make_unique<GpuSmokeRateView::Impl>();auto& v=*view->impl_;v.device.p=s.device.p;v.device.p->AddRef();v.timeline.p=s.timeline.p;v.timeline.p->AddRef();v.bytes=bytes;v.accumulatorBytes=accumulatorBytes;v.producerFence=v.lastFence=fence;
        make(s.device.p,v.accumulators,scalarBytes);make(s.device.p,v.rates,scalarBytes);make(s.device.p,v.diagnostics,48);v.inputs.reserve(batches.size());for(const auto& batch:batches){const auto b=batch.pool->buffers();auto input=std::make_shared<GpuSmokeRateView::Impl::Input>();ID3D12Resource* resources[]={b.attributes,b.alive,b.counters,b.drawArguments};for(unsigned i=0;i<4;++i){input->resources[i].p=resources[i];resources[i]->AddRef();}v.inputs.push_back(std::move(input));}
        // Publish lifetime ownership before the first pool command or dispatch.
        s.views.push_back(view);s.counters.lastFence=fence;recording=true;
        for(auto* resource:{v.accumulators.p,v.rates.p,v.diagnostics.p})transition(list,resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        s.bind(list,v,s.constants());s.dispatch(list,0,std::max(s.grid.scalarCount,12u));
        for(const auto& batch:batches){if(!batch.pool->prepareDraw(list,fence,error))throw std::runtime_error(error);const auto b=batch.pool->buffers();auto c=s.constants();c[12]=bits(b.origin.x);c[13]=bits(b.origin.y);c[14]=bits(b.origin.z);c[15]=b.capacity;c[16]=b.layout.position;c[17]=b.localSpace;c[18]=batch.quantum[0];c[19]=batch.quantum[1];c[20]=batch.quantum[2];transition(list,b.counters,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);s.bind(list,v,c);ID3D12Resource* resources[]={b.attributes,b.alive,b.counters};for(unsigned i=0;i<3;++i)list->SetComputeRootShaderResourceView(i+1,resources[i]->GetGPUVirtualAddress());s.dispatch(list,1,b.capacity);transition(list,b.counters,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);}
        s.bind(list,v,s.constants());s.dispatch(list,2,s.grid.cells);transition(list,v.rates.p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);transition(list,v.diagnostics.p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        v.binding={v.rates.p,v.diagnostics.p,s.spec,s.grid,tick,s.quantization.unitsPerSource};v.ready=true;++s.counters.frames;s.counters.batches+=batches.size();s.counters.lastTick=tick;return view;
    }catch(const std::exception& e){if(recording)s.counters.failed=true;++s.counters.rejections;error=e.what();return {};}
}
void GpuParticleSmokeSource::collect(){if(impl_)impl_->collect();}
ParticleSourceStats GpuParticleSmokeSource::stats() const{if(!impl_)return {};impl_->collect();return impl_->stats();}
bool GpuParticleSmokeSource::shutdown(std::string& error){error.clear();if(!impl_)return true;impl_->collect();if(impl_->timeline.p->GetCompletedValue()<impl_->counters.lastFence){error="NG-E-SMOKE-PARTICLES: actual fence in flight";return false;}for(const auto& view:impl_->views)if(view.use_count()>1||view->lastFence()>impl_->timeline.p->GetCompletedValue()){error="NG-E-SMOKE-PARTICLES: outstanding rate readers";return false;}impl_.reset();return true;}
}
