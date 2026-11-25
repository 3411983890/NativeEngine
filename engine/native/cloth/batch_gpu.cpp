#include "batch_gpu.hpp"
#include "nativegpu_shaders.h"
#include <algorithm>
#include <cstring>

namespace engine::cloth {
namespace {
template<class T>struct Com{T* p=nullptr;~Com(){if(p)p->Release();}T** out(){return &p;}};
void check(HRESULT result,const char* why){if(FAILED(result))throw Error("NG-E-CLOTH-BATCH",why);}
struct Record{std::array<std::uint32_t,32> constants;std::array<std::uint32_t,12> descriptors;};
static_assert(sizeof(Record)==176,"Cloth batch ABI changed");
}
struct GpuBatch::Impl {
    Com<ID3D12Device> device;Com<ID3D12Fence> timeline;Com<ID3D12Resource> metadata;Com<ID3D12DescriptorHeap> heap;
    Com<ID3D12RootSignature> root;Com<ID3D12PipelineState> pipeline;GpuBatchLimits limits;GpuBatchStats stats;
    struct Slot{std::uint64_t fence=0;std::vector<ID3D12Resource*> held;};std::vector<Slot> slots;
    unsigned stride=0;std::uint64_t slotBytes=0;char* mapped=nullptr;
    Impl(ID3D12Device* d,ID3D12Fence* f,GpuBatchLimits l):limits(l),slots(l.maxInFlight){
        device.p=d;d->AddRef();timeline.p=f;f->AddRef();stride=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        slotBytes=(sizeof(Record)*l.maxInstances+255)&~255ull;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=slotBytes*l.maxInFlight;desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        stats.metadataAllocationBytes=d->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
        stats.descriptorSlots=l.maxInstances*l.maxInFlight*12;stats.descriptorEstimateBytes=std::uint64_t(stats.descriptorSlots)*stride;
        if(stats.metadataAllocationBytes>l.maxAllocationBytes||stats.descriptorEstimateBytes>l.maxAllocationBytes-stats.metadataAllocationBytes)throw Error("NG-E-CLOTH-BATCH-BUDGET","Batch metadata and descriptor estimate exceed budget");
        stats.accountedBytes=stats.metadataAllocationBytes+stats.descriptorEstimateBytes;
        D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_UPLOAD;properties.CreationNodeMask=properties.VisibleNodeMask=1;
        check(d->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(metadata.out())),"Create batch metadata");D3D12_RANGE none{0,0};check(metadata.p->Map(0,&none,reinterpret_cast<void**>(&mapped)),"Map batch metadata");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=stats.descriptorSlots;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(heap.out())),"Create batch descriptor ring");
        D3D12_ROOT_PARAMETER parameters[2]{};parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.Num32BitValues=1;parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[1].Descriptor.ShaderRegister=6;
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=parameters;rd.Flags=D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        Com<ID3DBlob> bytes,errors;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,bytes.out(),errors.out()),"Serialize batch root");check(d->CreateRootSignature(0,bytes.p->GetBufferPointer(),bytes.p->GetBufferSize(),IID_PPV_ARGS(root.out())),"Create batch root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.p;pd.CS={nativegpu::shaders::kClothBatchTick.data,nativegpu::shaders::kClothBatchTick.size};check(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(pipeline.out())),"Create batch pipeline");
        for(auto& slot:slots)slot.held.reserve(l.maxInstances*12);
    }
    ~Impl(){for(auto& slot:slots)release(slot);if(mapped)metadata.p->Unmap(0,nullptr);}
    void release(Slot& slot){for(auto* resource:slot.held)resource->Release();slot.held.clear();slot.fence=0;}
    void collect(){const auto done=timeline.p->GetCompletedValue();for(auto& slot:slots)if(slot.fence&&slot.fence<=done)release(slot);}
    void descriptors(unsigned base,const std::array<ID3D12Resource*,12>& resources){
        const unsigned strides[]={32,48,32,4,4,32,80,4,0,16,16,16};auto handle=heap.p->GetCPUDescriptorHandleForHeapStart();handle.ptr+=std::uint64_t(base)*stride;
        for(unsigned i=0;i<12;++i){auto* resource=resources[i<6?i+6:i-6];const auto width=resource->GetDesc().Width;
            if(i<6){D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Buffer.NumElements=width/strides[i];view.Buffer.StructureByteStride=strides[i];device.p->CreateShaderResourceView(resource,&view,handle);}
            else{D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;view.Buffer.NumElements=width/(strides[i]?strides[i]:4);view.Buffer.StructureByteStride=strides[i];if(i==8){view.Format=DXGI_FORMAT_R32_TYPELESS;view.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;}device.p->CreateUnorderedAccessView(resource,nullptr,&view,handle);}handle.ptr+=stride;
        }
    }
};
GpuBatch::GpuBatch(ID3D12Device* device,ID3D12Fence* timeline,GpuBatchLimits limits){
    if(!device||!timeline||!limits.maxInstances||limits.maxInstances>256||!limits.maxInFlight||limits.maxInFlight>128||!limits.maxAllocationBytes)throw Error("NG-E-CLOTH-BATCH-BUDGET","Invalid batch device or limits");
    Com<ID3D12Device> owner;check(timeline->GetDevice(IID_PPV_ARGS(owner.out())),"Query batch fence device");if(owner.p!=device)throw Error("NG-E-CLOTH-BATCH-STATE","Batch fence belongs to another device");if(!nativegpu::shaders::kDxcAvailable)throw Error("NG-E-CLOTH-BATCH-SHADER","Batch shader unavailable");impl_=std::make_unique<Impl>(device,timeline,limits);
}
GpuBatch::~GpuBatch()=default;
bool GpuBatch::step(ID3D12GraphicsCommandList* commands,std::uint64_t fence,const std::vector<GpuBatchStep>& steps,std::string& error){
    error.clear();if(!impl_||!commands||steps.empty()||steps.size()>impl_->limits.maxInstances||!fence||fence<impl_->stats.lastFence||fence<=impl_->timeline.p->GetCompletedValue()){error="NG-E-CLOTH-BATCH-STATE: invalid batch";return false;}
    auto& s=*impl_;std::vector<GpuSolver::BatchInfo> info(steps.size());unsigned grouped=0;
    for(unsigned i=0;i<steps.size();++i){const auto& step=steps[i];if(!step.solver||step.solver->device()!=s.device.p||step.solver->timeline()!=s.timeline.p){error="NG-E-CLOTH-BATCH-STATE: incompatible solver";return false;}
        for(unsigned j=0;j<i;++j)if(steps[j].solver==step.solver){error="NG-E-CLOTH-BATCH-STATE: duplicate solver";return false;}
        if(!step.solver->batchInfo(commands,fence,step.dt,step.fieldTick,info[i],error))return false;if(info[i].grouped&&!info[i].paused)++grouped;
    }
    s.collect();unsigned slotIndex=0;
    if(grouped){while(slotIndex<s.slots.size()&&s.slots[slotIndex].fence)++slotIndex;if(slotIndex==s.slots.size()){error="NG-E-CLOTH-BATCH-BUDGET: metadata ring is still in use";return false;}}
    // All rejection paths above leave every solver and command list untouched.
    if(grouped){auto& slot=s.slots[slotIndex];unsigned actor=0;
        for(unsigned i=0;i<steps.size();++i)if(info[i].grouped&&!info[i].paused){Record record{};record.constants=info[i].constants;const unsigned base=(slotIndex*s.limits.maxInstances+actor)*12;for(unsigned j=0;j<12;++j){record.descriptors[j]=base+j;info[i].resources[j]->AddRef();slot.held.push_back(info[i].resources[j]);}s.descriptors(base,info[i].resources);std::memcpy(s.mapped+s.slotBytes*slotIndex+sizeof(Record)*actor,&record,sizeof(record));steps[i].solver->batchPrepare(commands,info[i]);++actor;}
        ID3D12DescriptorHeap* heaps[]={s.heap.p};commands->SetDescriptorHeaps(1,heaps);commands->SetComputeRootSignature(s.root.p);commands->SetComputeRoot32BitConstant(0,grouped,0);commands->SetComputeRootShaderResourceView(1,s.metadata.p->GetGPUVirtualAddress()+s.slotBytes*slotIndex);commands->SetPipelineState(s.pipeline.p);commands->Dispatch(grouped,1,1);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;commands->ResourceBarrier(1,&barrier);slot.fence=fence;++s.stats.dispatches;
    }
    for(unsigned i=0;i<steps.size();++i){if(info[i].paused)++s.stats.pausedSteps;else if(info[i].grouped){steps[i].solver->batchFinish(fence,info[i],steps[i].fieldTick);++s.stats.batchedSteps;}else{if(!steps[i].solver->step(commands,fence,steps[i].dt,error,steps[i].fieldTick))return false;++s.stats.fallbackSteps;}}
    ++s.stats.calls;for(const auto& actor:info)if(!actor.paused){s.stats.lastFence=fence;break;}return true;
}
void GpuBatch::collect(){if(impl_)impl_->collect();}
GpuBatchStats GpuBatch::stats() const{if(!impl_)return {};impl_->collect();auto result=impl_->stats;for(const auto& slot:impl_->slots){if(slot.fence)++result.activeSlots;result.heldResourceReferences+=slot.held.size();}return result;}
bool GpuBatch::shutdown(std::string& error){error.clear();if(impl_){impl_->collect();if(impl_->timeline.p->GetCompletedValue()<impl_->stats.lastFence){error="NG-E-CLOTH-BATCH-FENCE: batch is still in use";return false;}}impl_.reset();return true;}
}
