#ifndef NATIVEGPU_FIELD_SAMPLING
#define NATIVEGPU_FIELD_SAMPLING
// Read-only sparse ABI. A binding denotes one immutable snapshot. Halos grant
// stencil access, never additional query centres or writer access.
#ifndef FIELD_EXTERNAL_CONSTANTS
cbuffer QueryConstants : register(b0) {
    #include "field_constants.hlsli"
};
#endif
struct Page { int3 brick; uint channel; uint resource; uint3 padding; };
struct FieldAnswer { int value; uint status; uint2 sourceTick; uint2 age; uint2 version; uint clamped; uint3 padding; };
#ifndef FIELD_PAGE_TABLE_DESCRIPTOR
StructuredBuffer<Page> pages : register(t0);
Page fieldPage(uint i){return pages[i];}
#else
Page fieldPage(uint i){StructuredBuffer<Page> table=ResourceDescriptorHeap[FIELD_PAGE_TABLE_DESCRIPTOR];return table[i];}
#endif
bool less64(uint2 a,uint2 b) {return a.y<b.y||(a.y==b.y&&a.x<b.x);}
uint2 subtract64(uint2 a,uint2 b) {return uint2(a.x-b.x,a.y-b.y-(a.x<b.x?1:0));}
uint2 fieldAdd64(uint2 a,uint2 b) {uint low=a.x+b.x;return uint2(low,a.y+b.y+(low<a.x?1:0));}
uint2 negate64(uint2 a) {return fieldAdd64(~a,uint2(1,0));}
uint2 multiply32(uint a,uint b) {
    uint a0=a&65535,a1=a>>16,b0=b&65535,b1=b>>16;
    uint cross0=a1*b0,cross1=a0*b1;
    return fieldAdd64(fieldAdd64(uint2(a0*b0,a1*b1),uint2(cross0<<16,cross0>>16)),uint2(cross1<<16,cross1>>16));
}
int floorBrick(int v) {return v/16-(v%16<0?1:0);}
bool before(Page p,uint channel,int3 b) {
    if(p.channel!=channel)return p.channel<channel;
    if(p.brick.x!=b.x)return p.brick.x<b.x;
    if(p.brick.y!=b.y)return p.brick.y<b.y;
    return p.brick.z<b.z;
}
FieldAnswer readFieldCell(int3 cell,uint channel) {
    FieldAnswer a=(FieldAnswer)0;a.version=snapshotVersion;a.status=5;
    int3 brick=int3(floorBrick(cell.x),floorBrick(cell.y),floorBrick(cell.z));
    uint lo=0,hi=pageCount;
    while(lo<hi){uint mid=lo+(hi-lo)/2;if(before(fieldPage(mid),channel,brick))lo=mid+1;else hi=mid;}
    if(lo>=pageCount)return a;
    Page page=fieldPage(lo);
    if(page.channel!=channel||any(page.brick!=brick))return a;
    ByteAddressBuffer data=ResourceDescriptorHeap[NonUniformResourceIndex(page.resource)];
    uint3 local=(uint3)(cell-brick*16);uint index=local.x+16*(local.y+16*local.z);
    uint bit=1u<<(index%32);
    if((data.Load((12288+index/32)*4)&bit)==0)return a;
    a.value=asint(data.Load(index*4));a.sourceTick=data.Load2((4096+2*index)*4);
    a.age=subtract64(requestedTick,a.sourceTick);a.clamped=(data.Load((12416+index/32)*4)&bit)!=0;
    a.status=consistency!=2&&less64(maxAge,a.age)?7:0;return a;
}
FieldAnswer sampleField(int3 cell,uint channel,uint3 fraction,uint filter) {
    FieldAnswer a=(FieldAnswer)0;a.version=snapshotVersion;a.status=5;
    if(channel>=channelCount){a.status=3;return a;}
    uint maskWord=channel/32;
    uint mask=maskWord<4?channels0[maskWord]:channels1[maskWord-4];
    if((mask&(1u<<(channel%32)))==0){a.status=2;return a;}
    if(any(cell<regionMin)||any(cell>=regionMax)){a.status=4;return a;}
    if(filter>1||any(fraction>255)||(filter==0&&any(fraction!=0))){a.status=8;return a;}
    if(less64(requestedTick,snapshotTick)||(consistency==0&&any(requestedTick!=snapshotTick))){a.status=6;return a;}
    if(filter==0)return readFieldCell(cell,channel);
    int3 last=cell+int3(fraction.x!=0,fraction.y!=0,fraction.z!=0);
    if(any(last>=regionMax+(int)haloCells)){a.status=4;return a;}
    uint2 sum=0,oldest=uint2(0xffffffff,0xffffffff);uint clamped=0;
    // Emulated signed 64-bit accumulation needs no shaderInt64 feature.
    // Convex Q24 weights bound the sum to signed 56 bits.
    [unroll]for(uint z=0;z<2;++z)[unroll]for(uint y=0;y<2;++y)[unroll]for(uint x=0;x<2;++x) {
        uint weight=(x?fraction.x:256-fraction.x)*(y?fraction.y:256-fraction.y)*(z?fraction.z:256-fraction.z);
        if(weight==0)continue;
        FieldAnswer corner=readFieldCell(cell+int3(x,y,z),channel);
        if(corner.status!=0&&corner.status!=7)return a;
        uint magnitude=corner.value<0?0u-asuint(corner.value):asuint(corner.value);
        uint2 product=multiply32(magnitude,weight);
        sum=fieldAdd64(sum,corner.value<0?negate64(product):product);
        if(less64(corner.sourceTick,oldest))oldest=corner.sourceTick;
        clamped|=corner.clamped;
    }
    bool negative=(sum.y&0x80000000)!=0;
    uint2 magnitude=negative?negate64(sum):sum;
    magnitude=fieldAdd64(magnitude,uint2(1u<<23,0));
    uint rounded=(magnitude.x>>24)|(magnitude.y<<8);
    a.value=asint(negative?0u-rounded:rounded);a.sourceTick=oldest;
    a.age=subtract64(requestedTick,oldest);a.clamped=clamped;
    a.status=consistency!=2&&less64(maxAge,a.age)?7:0;return a;
}
#endif
