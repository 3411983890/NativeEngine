cbuffer WorldConstants:register(b0){uint pageCount;uint pageWords;uint2 reserved;};
struct WorkPage{int3 brick;uint channel;uint first;uint count;uint maximum;uint merge;};
struct Contribution{int3 minimum;uint descriptor;int3 maximum;uint sourceLow;uint sourceHigh;uint dimX;uint dimY;uint padding;};
StructuredBuffer<WorkPage> work:register(t0);
StructuredBuffer<Contribution> sources:register(t1);
RWByteAddressBuffer outputPages:register(u0);
uint wfThread(uint3 id){return id.x+id.y*(65535*64);}
uint2 wfAdd(uint2 a,uint2 b){uint low=a.x+b.x;return uint2(low,a.y+b.y+(low<a.x));}
bool wfLess(uint2 a,uint2 b){return a.y<b.y||(a.y==b.y&&a.x<b.x);}
[numthreads(64,1,1)]
void CSClear(uint3 tid:SV_DispatchThreadID){uint id=wfThread(tid);if(id<pageCount*pageWords)outputPages.Store(id*4,0);}
[numthreads(64,1,1)]
void CSAggregate(uint3 tid:SV_DispatchThreadID){
    uint id=wfThread(tid);if(id>=pageCount*4096)return;uint page=id/4096,cell=id%4096;WorkPage p=work[page];
    int3 at=p.brick*16+int3(cell%16,(cell/16)%16,cell/256);bool known=false;uint2 sum=0,oldest=uint2(0xffffffff,0xffffffff);uint maximum=0;
    for(uint row=0;row<p.count;++row){Contribution c=sources[p.first+row];if(any(at<c.minimum)||any(at>=c.maximum))continue;
        ByteAddressBuffer values=ResourceDescriptorHeap[NonUniformResourceIndex(c.descriptor)];uint3 local=(uint3)(at-c.minimum);uint value=values.Load((local.x+c.dimX*(local.y+c.dimY*local.z))*4);
        sum=wfAdd(sum,uint2(value,0));maximum=max(maximum,value);uint2 tick=uint2(c.sourceLow,c.sourceHigh);if(wfLess(tick,oldest))oldest=tick;known=true;
    }
    if(!known)return;bool clamped=p.merge==0?(sum.y!=0||sum.x>p.maximum):maximum>p.maximum;
    uint value=p.merge==0?(clamped?p.maximum:sum.x):min(maximum,p.maximum);uint base=page*pageWords;
    outputPages.Store((base+cell)*4,value);outputPages.Store2((base+4096+cell*2)*4,oldest);
    uint ignored;outputPages.InterlockedOr((base+12288+cell/32)*4,1u<<(cell%32),ignored);if(clamped)outputPages.InterlockedOr((base+12416+cell/32)*4,1u<<(cell%32),ignored);
}
