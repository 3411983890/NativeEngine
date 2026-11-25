#ifndef NATIVEGPU_TRANSPARENCY_COMMON
#define NATIVEGPU_TRANSPARENCY_COMMON
// 32-byte record; the legacy composite contains the same SM 5.1-compatible ABI.
struct TransparencyNode { float4 color; float depth; uint next; uint draw; uint primitive; };
static const uint TransparencyNull=0xffffffffu;
static const uint TransparencyMaxLayers=64;
bool TransparencyBefore(TransparencyNode a,TransparencyNode b){
    return a.depth<b.depth||(a.depth==b.depth&&(a.draw<b.draw||(a.draw==b.draw&&a.primitive<b.primitive)));
}
bool TransparencyNext(StructuredBuffer<TransparencyNode> nodes,uint head,float opaqueDepth,
                      bool hasPrevious,TransparencyNode previous,out TransparencyNode result){
    bool found=false;result=(TransparencyNode)0;
    [loop]for(uint i=0;i<TransparencyMaxLayers&&head!=TransparencyNull;i++){
        TransparencyNode n=nodes[head];head=n.next;
        if(n.depth<=opaqueDepth&&(!hasPrevious||TransparencyBefore(previous,n))&&(!found||TransparencyBefore(n,result))){result=n;found=true;}
    }
    return found;
}
#endif
