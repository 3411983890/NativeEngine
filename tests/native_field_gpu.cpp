#include "../engine/native/fields/gpu_storage.hpp"
#include "../engine/native/fields/local.hpp"
#include "../third_party/json.hpp"
#include "nativegpu_shaders.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace engine::fields;
namespace {
unsigned checks=0,groups=0,comparisons=0;
void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
void hr(HRESULT value,const char* why){if(FAILED(value))throw std::runtime_error(std::string(why)+" HRESULT="+std::to_string(static_cast<unsigned>(value)));}
template<class T> struct Com {T* p=nullptr;~Com(){if(p)p->Release();}T** out(){return &p;}Com()=default;Com(const Com&)=delete;};
void makeBuffer(ID3D12Device* device,Com<ID3D12Resource>& out,std::size_t bytes,D3D12_HEAP_TYPE heapType,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=heapType;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;
    hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(out.out())),"Create test buffer");
}
void write(ID3D12Resource* buffer,const void* data,std::size_t bytes) {void* p=nullptr;D3D12_RANGE range{0,0};hr(buffer->Map(0,&range,&p),"Map test upload");std::memcpy(p,data,bytes);D3D12_RANGE written{0,bytes};buffer->Unmap(0,&written);}
std::string adapterName(const wchar_t* name) {const int n=static_cast<int>(std::wcslen(name));const int bytes=WideCharToMultiByte(CP_UTF8,0,name,n,nullptr,0,nullptr,nullptr);std::string out(bytes,'\0');WideCharToMultiByte(CP_UTF8,0,name,n,out.data(),bytes,nullptr,nullptr);return out;}
nlohmann::json adapterInventory() {
    Com<IDXGIFactory6> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(factory.out())),"Create inventory factory");auto adapters=nlohmann::json::array();
    for(unsigned i=0;;++i){Com<IDXGIAdapter1> candidate;auto status=factory.p->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(candidate.out()));if(status==DXGI_ERROR_NOT_FOUND)break;hr(status,"Enumerate inventory adapter");DXGI_ADAPTER_DESC1 d{};candidate.p->GetDesc1(&d);Com<ID3D12Device> device;const auto creation=D3D12CreateDevice(candidate.p,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(device.out()));D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};HRESULT feature=E_FAIL;if(SUCCEEDED(creation))feature=device.p->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model));adapters.push_back({{"name",adapterName(d.Description)},{"software",static_cast<bool>(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)},{"deviceCreated",SUCCEEDED(creation)},{"deviceHresult",static_cast<unsigned>(creation)},{"shaderModel66",SUCCEEDED(feature)&&model.HighestShaderModel>=D3D_SHADER_MODEL_6_6}});}
    return {{"schema","nativegpu.gpu-adapter-inventory/1"},{"adapters",adapters}};
}
struct Rig {
    Com<IDXGIFactory6> factory;Com<ID3D12Device> device;Com<ID3D12CommandQueue> queue;Com<ID3D12CommandAllocator> allocator;
    Com<ID3D12GraphicsCommandList> commands;Com<ID3D12Fence> fence;Com<ID3D12RootSignature> root;Com<ID3D12PipelineState> pipeline;Com<ID3D12InfoQueue> info;
    HANDLE event=nullptr;std::uint64_t submitted=0;std::string adapter;bool debug=false;
    explicit Rig(const std::string& preferred="") {
        {Com<ID3D12Debug> layer;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(layer.out())))){layer.p->EnableDebugLayer();debug=true;}}
        hr(CreateDXGIFactory2(0,IID_PPV_ARGS(factory.out())),"Create factory");
        for(unsigned i=0;;++i) {
            Com<IDXGIAdapter1> candidate;if(factory.p->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(candidate.out()))==DXGI_ERROR_NOT_FOUND)break;
            if(!candidate.p)continue;DXGI_ADAPTER_DESC1 desc{};candidate.p->GetDesc1(&desc);if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
            const auto name=adapterName(desc.Description);
            if(!preferred.empty()&&name.find(preferred)==std::string::npos)continue;
            if(SUCCEEDED(D3D12CreateDevice(candidate.p,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(device.out())))) {
                adapter=name;break;
            }
        }
        require(device.p!=nullptr,"SM 6.6 hardware device unavailable");
        D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};hr(device.p->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&model,sizeof(model)),"Shader model query");require(model.HighestShaderModel>=D3D_SHADER_MODEL_6_6,"SM 6.6 unsupported");
        device.p->QueryInterface(IID_PPV_ARGS(info.out()));
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;hr(device.p->CreateCommandQueue(&q,IID_PPV_ARGS(queue.out())),"Create queue");
        hr(device.p->CreateCommandAllocator(q.Type,IID_PPV_ARGS(allocator.out())),"Create allocator");hr(device.p->CreateCommandList(0,q.Type,allocator.p,nullptr,IID_PPV_ARGS(commands.out())),"Create commands");hr(commands.p->Close(),"Close initial commands");
        hr(device.p->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.out())),"Create fence");event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"Create fence event failed");
        D3D12_ROOT_PARAMETER parameters[4]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants.Num32BitValues=28;
        parameters[1].ParameterType=parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[1].Descriptor.ShaderRegister=0;parameters[2].Descriptor.ShaderRegister=1;
        parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[3].Descriptor.ShaderRegister=0;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=4;signature.pParameters=parameters;signature.Flags=D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        Com<ID3DBlob> bytes,diagnostics;hr(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,bytes.out(),diagnostics.out()),"Serialize query root");
        hr(device.p->CreateRootSignature(0,bytes.p->GetBufferPointer(),bytes.p->GetBufferSize(),IID_PPV_ARGS(root.out())),"Create query root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.p;pso.CS={nativegpu::shaders::kFieldQuery.data,nativegpu::shaders::kFieldQuery.size};hr(device.p->CreateComputePipelineState(&pso,IID_PPV_ARGS(pipeline.out())),"Create field query pipeline");
    }
    ~Rig(){if(event)CloseHandle(event);}
    std::uint64_t ticket() const{return submitted+1;}
    void begin(){hr(allocator.p->Reset(),"Reset allocator");hr(commands.p->Reset(allocator.p,nullptr),"Reset commands");}
    void wait(){if(fence.p->GetCompletedValue()<submitted){hr(fence.p->SetEventOnCompletion(submitted,event),"Set fence event");if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU fence timed out");}require(fence.p->GetCompletedValue()>=submitted,"GPU fence incomplete");}
    void submit(bool waitNow=true){hr(commands.p->Close(),"Close commands");ID3D12CommandList* lists[]={commands.p};queue.p->ExecuteCommandLists(1,lists);hr(queue.p->Signal(fence.p,++submitted),"Signal queue");if(waitNow)wait();}
    std::vector<GpuFieldAnswer> query(GpuFieldStorage& fields,GpuView view,const std::vector<GpuFieldQuery>& input,Tick tick) {
        require(!input.empty(),"empty GPU query batch");GpuBindings b;require(fields.bindings(view,b),"invalid GPU view bindings");
        Com<ID3D12Resource> requests,result,readback;makeBuffer(device.p,requests,input.size()*sizeof(input[0]),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);write(requests.p,input.data(),input.size()*sizeof(input[0]));
        const auto bytes=input.size()*sizeof(GpuFieldAnswer);makeBuffer(device.p,result,bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);makeBuffer(device.p,readback,bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        begin();require(fields.markUsed(view,ticket()),"read fence registration failed");commands.p->SetDescriptorHeaps(1,&b.descriptors);commands.p->SetComputeRootSignature(root.p);commands.p->SetPipelineState(pipeline.p);
        const auto constants=b.constants(static_cast<std::uint32_t>(input.size()),tick);commands.p->SetComputeRoot32BitConstants(0,28,constants.data(),0);commands.p->SetComputeRootShaderResourceView(1,b.pageTable);commands.p->SetComputeRootShaderResourceView(2,requests.p->GetGPUVirtualAddress());commands.p->SetComputeRootUnorderedAccessView(3,result.p->GetGPUVirtualAddress());commands.p->Dispatch(static_cast<UINT>((input.size()+63)/64),1,1);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=result.p;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;commands.p->ResourceBarrier(1,&barrier);commands.p->CopyBufferRegion(readback.p,0,result.p,0,bytes);submit();fields.collect(fence.p->GetCompletedValue());
        void* data=nullptr;D3D12_RANGE range{0,bytes};hr(readback.p->Map(0,&range,&data),"Map GPU result");std::vector<GpuFieldAnswer> output(input.size());std::memcpy(output.data(),data,bytes);D3D12_RANGE none{0,0};readback.p->Unmap(0,&none);return output;
    }
    unsigned debugErrors() const {
        unsigned errors=0;if(!info.p)return 0;
        for(UINT64 i=0;i<info.p->GetNumStoredMessages();++i){SIZE_T size=0;info.p->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());if(SUCCEEDED(info.p->GetMessage(i,message,&size))&&(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR||message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)){++errors;std::cerr<<message->pDescription<<'\n';}}
        return errors;
    }
};
LeaseRequest request(Region region={{0,0,0},{1,1,1}},std::vector<ChannelId> channels={0},Consistency consistency=Consistency::Eventual) {LeaseRequest r;r.region=region;r.channels=std::move(channels);r.owner="consumer";r.maxPages=32;r.consistency=consistency;r.maxAgeTicks=3;return r;}
Lease writer(World& world,std::vector<ChannelId> channels={0},const char* owner="producer",Region region={{-16,0,0},{32,1,1}}){auto r=request(region,std::move(channels));r.owner=owner;r.writable=true;const auto result=world.acquire(r);require(result.accepted,"CPU writer rejected");return result.lease;}
GpuLease gpuLease(GpuFieldStorage& storage,LeaseRequest r=request()){const auto result=storage.acquire(std::move(r));require(result.accepted,"GPU lease rejected");return result.lease;}
GpuView gpuView(GpuFieldStorage& storage,GpuLease lease){const auto result=storage.view(lease);require(result.accepted,"GPU view rejected");return result.view;}
void publish(Rig& rig,GpuFieldStorage& storage){std::string error;rig.begin();require(storage.publish(rig.commands.p,rig.ticket(),error),error.c_str());rig.submit();storage.collect(rig.fence.p->GetCompletedValue());}
void compare(const Sample& cpu,const GpuFieldAnswer& gpu) {
    ++comparisons;require(static_cast<unsigned>(cpu.status)==gpu.status,"GPU status differs from CPU field reference");
    require(cpu.version==((static_cast<std::uint64_t>(gpu.versionHigh)<<32)|gpu.versionLow),"GPU snapshot version differs");
    if(cpu.status==Status::Ok||cpu.status==Status::Stale){require(cpu.value==gpu.value,"GPU value differs");require(cpu.sourceTick==((static_cast<std::uint64_t>(gpu.sourceHigh)<<32)|gpu.sourceLow),"GPU source tick differs");require(cpu.ageTicks==((static_cast<std::uint64_t>(gpu.ageHigh)<<32)|gpu.ageLow),"GPU age differs");require(cpu.clamped==static_cast<bool>(gpu.clamped),"GPU saturation flag differs");}
}
void sparseQueries(Rig& rig) {
    World world;world.registerChannel({"density","fixed","producer",Domain::VisualReference,Merge::Sum,0,1000,1});world.registerChannel({"wind","fixed","gameplay",Domain::CpuAuthority,Merge::Sum,-1000,1000,1});
    for(unsigned i=2;i<=130;++i)world.registerChannel({"channel"+std::to_string(i),"fixed","producer",Domain::VisualReference,Merge::Sum,0,1000,1});
    auto source=writer(world,{0,130});auto authority=writer(world,{1},"gameplay");
    require(world.publish(source,0,1,1,1,Domain::VisualReference,{{{-1,0,0},100},{{0,0,0},200},{{15,0,0},300},{{16,0,0},400}})==PublishResult::Accepted,"density publish failed");
    world.publish(source,0,2,1,1,Domain::VisualReference,{{{0,0,0},900}});world.publish(source,130,1,1,1,Domain::VisualReference,{{{0,0,0},321}});world.publish(authority,1,1,1,1,Domain::CpuAuthority,{{{0,0,0},-75}});require(world.commit(1),"initial commit failed");const auto digest=world.digest();
    GpuFieldStorage fields(world,rig.device.p,{8,3,16,16,8*1024*1024});require(fields.stats().defaultAllocationBytes==0&&fields.stats().uploadAllocationBytes==0,"unleased cache allocated GPU payload");
    auto primaryRequest=request({{-1,0,0},{17,1,1}});auto primary=gpuLease(fields,primaryRequest);auto overlap=gpuLease(fields);auto high=gpuLease(fields,request({{0,0,0},{1,1,1}},{130}));
    require(fields.stats().defaultAllocationBytes==0,"spatial metadata allocated GPU payload before publication");publish(rig,fields);
    require(fields.stats().currentPages==4,"overlapping consumers or unused channels duplicated GPU pages");
    auto a=gpuView(fields,primary),b=gpuView(fields,overlap),c=gpuView(fields,high);GpuBindings ba,bb;fields.bindings(a,ba);fields.bindings(b,bb);require(ba.descriptors==bb.descriptors&&ba.serial==bb.serial,"overlapping views did not share immutable GPU snapshot");
    auto cpu=world.acquire(primaryRequest);require(cpu.accepted,"CPU comparison lease failed");
    const std::vector<GpuFieldQuery> queries={{-1,0,0,0},{0,0,0,0},{1,0,0,0},{15,0,0,0},{16,0,0,0},{17,0,0,0},{-2,0,0,0},{0,0,0,130},{0,0,0,500},{0,0,0,1}};
    auto answers=rig.query(fields,a,queries,1);for(std::size_t i=0;i<queries.size();++i)compare(world.sample(cpu.lease,queries[i].channel,{queries[i].x,queries[i].y,queries[i].z},1),answers[i]);
    require(rig.query(fields,c,{{0,0,0,130}},1).front().value==321,"upper channel-mask words failed");
    auto writeAttempt=request();writeAttempt.writable=true;require(!fields.acquire(writeAttempt).accepted,"GPU cache acquired a writer capability");
    auto mirrored=gpuLease(fields,request({{0,0,0},{1,1,1}},{1},Consistency::Strong));publish(rig,fields);auto authView=gpuView(fields,mirrored);
    auto mirroredResult=rig.query(fields,authView,{{0,0,0,1}},1).front();require(mirroredResult.status==0&&mirroredResult.value==-75,"read-only authority mirror lost signed value");require(world.digest()==digest,"GPU cache changed authoritative field data");
    for(auto v:{a,b,c,authView})require(fields.releaseView(v,rig.submitted),"view release failed");for(auto l:{primary,overlap,high,mirrored})require(fields.release(l,rig.submitted),"spatial release failed");fields.collect(rig.fence.p->GetCompletedValue());std::string error;require(fields.shutdown(rig.submitted,error),error.c_str());require(fields.stats().defaultAllocationBytes==0&&fields.stats().uploadAllocationBytes==0,"GPU cache payload leaked");require(!fields.bindings(a,ba)&&!fields.view(primary).accepted,"retired GPU handles remained valid");world.release(cpu.lease);++groups;
}
void freshnessAndVersions(Rig& rig) {
    World world;world.registerChannel({"density","fixed","producer",Domain::VisualReference,Merge::Sum,0,1000,1});auto source=writer(world);world.publish(source,0,1,1,5,Domain::VisualReference,{{{0,0,0},70}});world.commit(5);
    GpuFieldStorage fields(world,rig.device.p);auto eventual=gpuLease(fields),strong=gpuLease(fields,request({{0,0,0},{1,1,1}},{0},Consistency::Strong));publish(rig,fields);auto old=gpuView(fields,eventual),oldStrong=gpuView(fields,strong);
    world.publishAt({source,0,1,2,6,8,Domain::VisualReference,{{{0,0,0},90}}});world.commit(8);publish(rig,fields);auto current=gpuView(fields,eventual),currentStrong=gpuView(fields,strong);
    require(rig.query(fields,old,{{0,0,0,0}},5).front().value==70,"new publication modified in-flight GPU snapshot");
    require(rig.query(fields,oldStrong,{{0,0,0,0}},8).front().status==static_cast<unsigned>(Status::TickMismatch),"Strong GPU query served another snapshot tick");
    auto cpu=world.acquire(request());auto cpuStrong=world.acquire(request({{0,0,0},{1,1,1}},{0},Consistency::Strong));
    for(auto tick:{8ull,9ull,10ull})compare(world.sample(cpu.lease,0,{0,0,0},tick),rig.query(fields,current,{{0,0,0,0}},tick).front());
    compare(world.sample(cpuStrong.lease,0,{0,0,0},8),rig.query(fields,currentStrong,{{0,0,0,0}},8).front());
    require(!fields.markUsed(current,fields.stats().completedFence),"past read fence accepted");
    const Tick rollover=(1ull<<32)+1;world.publishAt({source,0,1,3,rollover-2,rollover,Domain::VisualReference,{{{0,0,0},95}}});world.commit(rollover);publish(rig,fields);auto crossed=gpuView(fields,eventual);
    compare(world.sample(cpu.lease,0,{0,0,0},rollover),rig.query(fields,crossed,{{0,0,0,0}},rollover).front());
    compare(world.sample(cpu.lease,0,{0,0,0},rollover+(1ull<<32)),rig.query(fields,crossed,{{0,0,0,0}},rollover+(1ull<<32)).front());
    for(auto v:{old,oldStrong,current,currentStrong,crossed})fields.releaseView(v,rig.submitted);fields.release(eventual,rig.submitted);fields.release(strong,rig.submitted);fields.collect(rig.submitted);std::string error;require(fields.shutdown(rig.submitted,error),error.c_str());world.release(cpu.lease);world.release(cpuStrong.lease);++groups;
}
void snapshotBudgets(Rig& rig) {
    World world;world.registerChannel({"density","fixed","producer",Domain::VisualReference,Merge::Sum,0,1000,1});auto source=writer(world);
    GpuFieldStorage fields(world,rig.device.p,{2,2,4,4,2*1024*1024});auto lease=gpuLease(fields);
    world.publish(source,0,1,1,1,Domain::VisualReference,{{{0,0,0},111}});world.commit(1);publish(rig,fields);auto first=gpuView(fields,lease);
    world.publish(source,0,1,2,2,Domain::VisualReference,{{{0,0,0},222}});world.commit(2);publish(rig,fields);auto second=gpuView(fields,lease);
    world.publish(source,0,1,3,3,Domain::VisualReference,{{{0,0,0},333}});world.commit(3);rig.begin();std::string error;require(!fields.publish(rig.commands.p,rig.ticket(),error),"in-flight snapshot cap ignored");rig.submit();
    require(rig.query(fields,second,{{0,0,0,0}},2).front().value==222,"rejected publication damaged current snapshot");
    fields.releaseView(first,rig.submitted);fields.collect(rig.submitted);publish(rig,fields);auto third=gpuView(fields,lease);require(rig.query(fields,third,{{0,0,0,0}},3).front().value==333,"snapshot budget never recovered after completed release");
    fields.releaseView(second,rig.submitted);fields.releaseView(third,rig.submitted);fields.release(lease,rig.submitted);fields.collect(rig.submitted);require(fields.shutdown(rig.submitted,error),error.c_str());
    GpuFieldStorage tooSmall(world,rig.device.p,{2,2,4,4,1});auto smallLease=gpuLease(tooSmall);rig.begin();require(!tooSmall.publish(rig.commands.p,rig.ticket(),error)&&tooSmall.stats().snapshots==0,"GPU allocation-byte budget ignored or partially published");rig.submit();tooSmall.release(smallLease);tooSmall.collect(rig.submitted);require(tooSmall.shutdown(rig.submitted,error),error.c_str());++groups;
}
void actualFenceRetention(Rig& rig) {
    World world;world.registerChannel({"density","fixed","producer",Domain::VisualReference,Merge::Sum,0,1000,1});auto source=writer(world);world.publish(source,0,1,1,1,Domain::VisualReference,{{{0,0,0},80}});world.commit(1);
    GpuFieldStorage fields(world,rig.device.p);auto lease=gpuLease(fields);Com<ID3D12Fence> gate;hr(rig.device.p->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.out())),"Create blocking gate");hr(rig.queue.p->Wait(gate.p,1),"Queue gate wait");rig.begin();std::string error;const auto ticket=rig.ticket();require(fields.publish(rig.commands.p,ticket,error),error.c_str());auto view=gpuView(fields,lease);rig.submit(false);
    fields.releaseView(view,ticket);fields.release(lease,ticket);fields.collect(rig.fence.p->GetCompletedValue());require(rig.fence.p->GetCompletedValue()<ticket&&fields.stats().snapshots==1&&fields.stats().defaultAllocationBytes>0,"GPU page freed before real queued copy completed");require(!fields.shutdown(rig.fence.p->GetCompletedValue(),error),"shutdown accepted incomplete GPU work");
    hr(gate.p->Signal(1),"Unblock queue");rig.wait();fields.collect(rig.fence.p->GetCompletedValue());require(fields.shutdown(rig.submitted,error)&&fields.stats().residentPages==0,"completed GPU copy did not release retired pages");++groups;
}
void localProjectionOnGpu(Rig& rig) {
    World world;world.registerChannel({"density","fixed","smoke",Domain::VisualReference,Merge::Sum,0,1000,1});LocalFields local(world);LocalSpec spec;spec.source=1;spec.owner="smoke";spec.dimensions={2,2,2};spec.channels={{0,ProjectionReduction::VolumeAverage}};auto made=local.create(spec);require(made.result.accepted(),"LEF creation failed");local.replace(made.handle,{{0,std::vector<std::int32_t>(8,125)}},1);local.promote(made.handle,1);world.commit(1);local.settle(made.handle);
    GpuFieldStorage fields(world,rig.device.p);auto oldLease=gpuLease(fields);publish(rig,fields);auto old=gpuView(fields,oldLease);require(rig.query(fields,old,{{0,0,0,0}},1).front().value==125,"LEF projection did not reach GPU sparse field");
    require(local.move(made.handle,{32,0,0},2).accepted()&&world.commit(2)&&local.settle(made.handle).accepted(),"LEF movement failed");auto newLease=gpuLease(fields,request({{16,0,0},{17,1,1}}));publish(rig,fields);auto newer=gpuView(fields,newLease),withdrawn=gpuView(fields,oldLease);
    require(rig.query(fields,old,{{0,0,0,0}},1).front().value==125,"old frame did not retain prior projection snapshot");require(rig.query(fields,newer,{{16,0,0,0}},2).front().value==125,"moved LEF projection missing on GPU");require(rig.query(fields,withdrawn,{{0,0,0,0}},2).front().status==static_cast<unsigned>(Status::Unavailable),"withdrawn LEF footprint survived new GPU snapshot");
    for(auto v:{old,newer,withdrawn})fields.releaseView(v,rig.submitted);fields.release(oldLease,rig.submitted);fields.release(newLease,rig.submitted);fields.collect(rig.submitted);std::string error;require(fields.shutdown(rig.submitted,error),error.c_str());local.retire(made.handle,3);world.commit(3);local.settle(made.handle);world.collect(rig.submitted);++groups;
}
void cyclesAndLongTicks(Rig& rig) {
    World world;world.registerChannel({"density","fixed","producer",Domain::VisualReference,Merge::Sum,0,1000,1});auto source=writer(world);GpuFieldStorage fields(world,rig.device.p);GpuLease previous{};GpuView previousView{};GpuBindings bindings;
    for(unsigned i=1;i<=64;++i){const Tick tick=(1ull<<32)+i;world.publish(source,0,1,i,tick,Domain::VisualReference,{{{0,0,0},static_cast<int>(i)}});world.commit(tick);auto lease=gpuLease(fields);require(!fields.view(previous).accepted&&!fields.bindings(previousView,bindings),"GPU handle generation reused");publish(rig,fields);auto view=gpuView(fields,lease);auto output=rig.query(fields,view,{{0,0,0,0}},tick+2).front();require(output.status==0&&output.value==static_cast<int>(i)&&output.sourceHigh==1&&output.ageLow==2&&output.ageHigh==0,"64-bit tick shader arithmetic or cycle value failed");fields.releaseView(view,rig.submitted);fields.release(lease,rig.submitted);fields.collect(rig.submitted);require(fields.stats().snapshots==0&&fields.stats().defaultAllocationBytes==0&&fields.stats().uploadAllocationBytes==0,"64 GPU lifecycle cycles leaked resources");previous=lease;previousView=view;}
    std::string error;require(fields.shutdown(rig.submitted,error),error.c_str());++groups;
}
void trilinearAcrossBricks(Rig& rig) {
    World world(128,256);
    world.registerChannel({"signed","fixed","producer",Domain::VisualReference,Merge::Sum,std::numeric_limits<int>::min(),std::numeric_limits<int>::max(),1});
    world.registerChannel({"bounded","fixed","producer",Domain::VisualReference,Merge::Sum,-100,100,1});
    auto sourceRequest=request({{-2,-2,-2},{18,18,18}},{0,1});sourceRequest.maxPages=64;sourceRequest.owner="producer";sourceRequest.writable=true;
    auto source=world.acquire(sourceRequest);require(source.accepted,"sampling writer rejected");
    const int extremes[8]={std::numeric_limits<int>::min(),std::numeric_limits<int>::max(),-3,0,1,2,-1700000000,1700000000};
    std::vector<Write> values,bounded;
    for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x) {
        values.push_back({{-1+x,-1+y,-1+z},-500+256*x+512*y+1024*z});
        values.push_back({{15+x,15+y,15+z},extremes[x+2*y+4*z]});
        bounded.push_back({{-1+x,-1+y,-1+z},100});
    }
    values.push_back({{8,8,8},42});
    for(int x=5;x<=6;++x){values.push_back({{x,8,8},std::numeric_limits<int>::min()});values.push_back({{x,10,8},std::numeric_limits<int>::max()});}
    const Tick tick=(1ull<<32)+5;
    require(world.publish(source.lease,0,1,1,tick,Domain::VisualReference,values)==PublishResult::Accepted,"sampling values rejected");
    world.publish(source.lease,1,1,1,tick,Domain::VisualReference,bounded);
    world.publishAt({source.lease,1,2,1,tick-4,tick,Domain::VisualReference,{{{-1,-1,-1},100}}});world.commit(tick);
    auto r=request({{-1,-1,-1},{16,16,16}},{0,1});r.haloCells=1;r.maxPages=64;
    const auto leasesBefore=world.stats().leases;
    GpuFieldStorage tiny(world,rig.device.p,{26,3,8,8,32*1024*1024});auto oneChannel=r;oneChannel.channels={0};
    require(!tiny.acquire(oneChannel).accepted&&tiny.stats().leases==0&&world.stats().leases==leasesBefore,"GPU halo page rejection leaked capability");
    GpuFieldStorage fields(world,rig.device.p,{64,3,8,8,32*1024*1024});
    auto halo=gpuLease(fields,r);auto cpu=world.acquire(r);require(cpu.accepted,"CPU halo rejected");
    auto noHaloRequest=r;noHaloRequest.haloCells=0;auto noHalo=gpuLease(fields,noHaloRequest);auto cpuNarrow=world.acquire(noHaloRequest);
    auto strongRequest=r;strongRequest.consistency=Consistency::Strong;auto strong=gpuLease(fields,strongRequest);auto cpuStrong=world.acquire(strongRequest);
    auto staleRequest=r;staleRequest.consistency=Consistency::Stale;auto stale=gpuLease(fields,staleRequest);auto cpuStale=world.acquire(staleRequest);
    publish(rig,fields);require(fields.stats().currentPages==54,"GPU halos duplicated overlapping pages or omitted corner bricks");
    auto view=gpuView(fields,halo),narrow=gpuView(fields,noHalo),strongView=gpuView(fields,strong),staleView=gpuView(fields,stale);
    std::vector<GpuFieldQuery> queries={{-1,-1,-1,0,64,128,192,1},{15,15,15,0,128,0,0,1},
        {8,8,8,0,0,0,0,1},{8,8,8,0,1,0,0,1},{5,8,8,0,255,0,0,1},{5,10,8,0,255,0,0,1},
        {16,15,15,0,0,0,0,1},{-1,-1,-1,0,256,0,0,1},{-1,-1,-1,1,128,128,128,1},
        {15,15,15,1,128,128,128,1},{-1,-1,-1,500,1,1,1,1}};
    std::uint32_t random=0x873abc1u;
    for(unsigned i=0;i<4096;++i){random=random*1664525u+1013904223u;int base=i%2?15:-1;queries.push_back({base,base,base,0,random&255,(random>>8)&255,(random>>16)&255,1});}
    auto output=rig.query(fields,view,queries,tick);
    for(std::size_t i=0;i<queries.size();++i){const auto& q=queries[i];compare(world.sampleLinear(cpu.lease,q.channel,{{q.x,q.y,q.z},{q.fractionX,q.fractionY,q.fractionZ}},tick),output[i]);}
    require(output[0].value==588&&output[1].value==-1,"GPU polynomial or signed half rounding differs from analytic result");
    require(output[4].value==std::numeric_limits<int>::min()&&output[5].value==std::numeric_limits<int>::max(),"GPU emulated signed product overflowed");
    const std::vector<GpuFieldQuery> boundary={{15,15,15,0,128,128,128,1},{-1,-1,-1,1,128,128,128,1},{8,8,8,0,0,0,0,1}};
    for(auto pair:{std::make_pair(narrow,cpuNarrow.lease),std::make_pair(strongView,cpuStrong.lease),std::make_pair(staleView,cpuStale.lease)}) {
        for(Tick requested:{tick,tick+1,tick+(1ull<<32)}) {
            auto results=rig.query(fields,pair.first,boundary,requested);
            for(std::size_t i=0;i<boundary.size();++i){auto q=boundary[i];compare(world.sampleLinear(pair.second,q.channel,{{q.x,q.y,q.z},{q.fractionX,q.fractionY,q.fractionZ}},requested),results[i]);}
        }
    }
    const auto malformed=rig.query(fields,view,{{-1,-1,-1,0,0,0,0,2},{-1,-1,-1,0,1,0,0,0}},tick);
    require(malformed[0].status==static_cast<unsigned>(Status::InvalidPosition)&&malformed[1].status==static_cast<unsigned>(Status::InvalidPosition),"GPU malformed filter or lattice fraction accepted");
    for(auto v:{view,narrow,strongView,staleView})fields.releaseView(v,rig.submitted);
    for(auto l:{halo,noHalo,strong,stale})fields.release(l,rig.submitted);
    fields.collect(rig.submitted);std::string error;require(fields.shutdown(rig.submitted,error)&&fields.stats().residentPages==0,"sampling GPU allocations survived release");
    for(auto l:{cpu.lease,cpuNarrow.lease,cpuStrong.lease,cpuStale.lease,source.lease})world.release(l);
    world.collect(rig.submitted);require(world.stats().committedBytes==0,"sampling CPU halo allocation leaked");++groups;
}
}
int main(int argc,char** argv) {
    try {if(argc==2&&std::string(argv[1])=="--list-adapters"){std::cout<<adapterInventory().dump()<<'\n';return 0;}std::string preferred;if(argc==3&&std::string(argv[1])=="--adapter")preferred=argv[2];else if(argc!=1)throw std::invalid_argument("Usage: native_field_gpu_tests [--adapter substring | --list-adapters]");Rig rig(preferred);sparseQueries(rig);freshnessAndVersions(rig);snapshotBudgets(rig);actualFenceRetention(rig);localProjectionOnGpu(rig);cyclesAndLongTicks(rig);trilinearAcrossBricks(rig);const auto errors=rig.debugErrors();require(errors==0,"D3D12 debug errors");
        std::cout<<nlohmann::json({{"schema","nativegpu.fields-gpu-evidence/1"},{"ok",true},{"groups",groups},{"checks",checks},{"cpuComparisons",comparisons},{"gpuLifecycleCycles",64},{"adapter",rig.adapter},{"debugLayer",rig.debug},{"debugErrors",errors},{"realGpuDispatch",true},{"fixedPointTrilinear",true},{"readHalos",true},{"referenceInput","committed CPU fields and LEF projections"},{"validationReadbackOnly",true},{"rendererIntegrated",false},{"gpuAuthorityWriteback",false}}).dump()<<'\n';
    }catch(const std::exception& error){std::cerr<<"GPU fields failed after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}
}
