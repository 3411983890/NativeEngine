// 32 constant DWORDs followed by 12 absolute descriptor indices (176 bytes).
struct ClothBatchRecord{uint4 words[8];uint4 descriptors[3];};
StructuredBuffer<ClothBatchRecord> batchRecords:register(t6);
cbuffer ClothBatchConstants:register(b0){uint batchRecordCount;};
static uint vertexCount,triangleCount,hingeCount,batchStart,batchCount,flags,colliderCount,lambdaCount;
static float h,damping,dragCoefficient,thickness,stretchCompliance,shearCompliance,bendAngleCompliance;
static uint solverIterations,solverSubsteps,membraneColorCount,adjacencyBase,hingeColorCount,colorScheduleBase,batchPreparedWind;
static float3 origin,translation,uniformWind;
static uint4 batchDescriptors[3];
void loadClothBatch(uint index){
    ClothBatchRecord r=batchRecords[index];
    vertexCount=r.words[0].x;triangleCount=r.words[0].y;hingeCount=r.words[0].z;batchStart=r.words[0].w;
    batchCount=r.words[1].x;flags=r.words[1].y;colliderCount=r.words[1].z;lambdaCount=r.words[1].w;
    h=asfloat(r.words[2].x);damping=asfloat(r.words[2].y);dragCoefficient=asfloat(r.words[2].z);thickness=asfloat(r.words[2].w);
    stretchCompliance=asfloat(r.words[3].x);shearCompliance=asfloat(r.words[3].y);bendAngleCompliance=asfloat(r.words[3].z);solverIterations=r.words[3].w;
    origin=asfloat(r.words[4].xyz);solverSubsteps=r.words[4].w;
    translation=asfloat(r.words[5].xyz);membraneColorCount=r.words[5].w;
    uniformWind=asfloat(r.words[6].xyz);adjacencyBase=r.words[6].w;
    hingeColorCount=r.words[7].x;colorScheduleBase=r.words[7].y;batchPreparedWind=r.words[7].z;
    for(uint i=0;i<3;++i)batchDescriptors[i]=r.descriptors[i];
}
StructuredBuffer<RestVertex> batchRest(){return ResourceDescriptorHeap[NonUniformResourceIndex(batchDescriptors[0].x)];}
StructuredBuffer<Membrane> batchMembranes(){return ResourceDescriptorHeap[NonUniformResourceIndex(batchDescriptors[0].y)];}
StructuredBuffer<Hinge> batchHinges(){return ResourceDescriptorHeap[NonUniformResourceIndex(batchDescriptors[0].z)];}
StructuredBuffer<uint> batchSchedule(){return ResourceDescriptorHeap[NonUniformResourceIndex(batchDescriptors[0].w)];}
StructuredBuffer<uint> batchAdjacency(){return ResourceDescriptorHeap[NonUniformResourceIndex(batchDescriptors[1].x)];}
StructuredBuffer<Collider> batchColliders(){return ResourceDescriptorHeap[NonUniformResourceIndex(batchDescriptors[1].y)];}
RWStructuredBuffer<Vertex> batchVertices(){return ResourceDescriptorHeap[NonUniformResourceIndex(batchDescriptors[1].z)];}
RWStructuredBuffer<float> batchLambdas(){return ResourceDescriptorHeap[NonUniformResourceIndex(batchDescriptors[1].w)];}
RWByteAddressBuffer batchDiagnostics(){return ResourceDescriptorHeap[NonUniformResourceIndex(batchDescriptors[2].x)];}
RWStructuredBuffer<float4> batchWinds(){return ResourceDescriptorHeap[NonUniformResourceIndex(batchDescriptors[2].y)];}
RWStructuredBuffer<float4> batchCandidate(){return ResourceDescriptorHeap[NonUniformResourceIndex(batchDescriptors[2].z)];}
RWStructuredBuffer<float4> batchAerodynamics(){return ResourceDescriptorHeap[NonUniformResourceIndex(batchDescriptors[2].w)];}
#define rest batchRest()
#define membranes batchMembranes()
#define hinges batchHinges()
#define schedule batchSchedule()
#define adjacency batchAdjacency()
#define colliders batchColliders()
#define vertices batchVertices()
#define lambdas batchLambdas()
#define diagnostics batchDiagnostics()
#define winds batchWinds()
#define windCandidate batchCandidate()
#define aerodynamics batchAerodynamics()
