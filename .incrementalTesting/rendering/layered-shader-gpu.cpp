#include "../../tests/d3d12_test_rig.hpp"
#include "../../engine/native/material_layered.hpp"
#include <cmath>
#include <fstream>
#include <iterator>

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Expected compiled shader path");
        std::ifstream file(argv[1], std::ios::binary);
        const std::vector<char> shader((std::istreambuf_iterator<char>(file)), {});
        if (shader.empty()) throw std::runtime_error("Cannot read shader");
        gpu_test::Rig rig;
        gpu_test::Com<ID3D12RootSignature> root;
        gpu_test::Com<ID3D12PipelineState> pipeline;
        gpu_test::Com<ID3DBlob> bytes, errors;
        D3D12_ROOT_PARAMETER parameter{}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_ROOT_SIGNATURE_DESC signature{}; signature.NumParameters = 1; signature.pParameters = &parameter;
        gpu_test::hr(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, bytes.out(), errors.out()), "Serialize root signature");
        gpu_test::hr(rig.device.p->CreateRootSignature(0, bytes.p->GetBufferPointer(), bytes.p->GetBufferSize(), IID_PPV_ARGS(root.out())), "Create root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root.p; pso.CS = {shader.data(), shader.size()};
        gpu_test::hr(rig.device.p->CreateComputePipelineState(&pso, IID_PPV_ARGS(pipeline.out())), "Create pipeline");
        constexpr unsigned count = 336, size = count * 4 * sizeof(float);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        gpu_test::Com<ID3D12Resource> result, readback;
        gpu_test::hr(rig.device.p->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(result.out())), "Create result");
        gpu_test::makeBuffer(rig.device.p, readback, size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        rig.begin(); rig.commands.p->SetComputeRootSignature(root.p); rig.commands.p->SetPipelineState(pipeline.p);
        rig.commands.p->SetComputeRootUnorderedAccessView(0, result.p->GetGPUVirtualAddress()); rig.commands.p->Dispatch((count+63)/64,1,1);
        gpu_test::transition(rig.commands.p,result.p,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        rig.commands.p->CopyBufferRegion(readback.p,0,result.p,0,size); rig.submit();
        void* mapped=nullptr; D3D12_RANGE range{0,size}; gpu_test::hr(readback.p->Map(0,&range,&mapped),"Map result");
        const float* values=static_cast<const float*>(mapped);
        for (unsigned i=0;i<count;++i) {
            const float coverage=(i%7)/6.0f, bottom=((i/7)%4)*.25f, top=((i/28)%4)*.25f, width=((i/112)+1)*.2f;
            const float weight=engine::material::layered::heightCoverage(coverage,bottom,top,width,0);
            const float len=std::hypot(coverage,1-coverage);
            if (!std::isfinite(values[i*4+3]) || std::abs(values[i*4+3]-weight)>0.00001f || std::abs(values[i*4+1]-coverage/len)>0.00001f || std::abs(values[i*4+2]-(1-coverage)/len)>0.00001f) throw std::runtime_error("GPU layered blend differs from CPU reference at index "+std::to_string(i));
        }
        D3D12_RANGE none{0,0};readback.p->Unmap(0,&none);
        if(rig.debugErrors())throw std::runtime_error("D3D12 debug errors");
        std::cout << "GPU layered reference: " << count << " cases on " << rig.adapter << '\n';
        return 0;
    } catch(const std::exception& error) {std::cerr << error.what() << '\n';return 1;}
}
