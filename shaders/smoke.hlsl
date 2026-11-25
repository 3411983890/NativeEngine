// Independent visual SmokeFireSolver. Cell-centred SoA scalars and positive
// staggered faces; D and G form the same discrete MAC Laplacian. Closed exterior.
cbuffer Params:register(b0) {
    uint3 dim;uint cells;uint3 offsets;uint flags;
    float dt,cellSize,ambient,maxSpeed;
    float densityDecay,cooling,fuelDecay,burnRate;
    float ignition,heatPerFuel,sootPerFuel,maxDensity;
    float maxTemperature,maxFuel,buoyancy,smokeWeight;
    float vorticity,omega;float2 pad0;float4 pad1;
};
ByteAddressBuffer A:register(t0);ByteAddressBuffer B:register(t1);
ByteAddressBuffer P:register(t2);ByteAddressBuffer Boundary:register(t3);
ByteAddressBuffer Input:register(t4);ByteAddressBuffer Acceleration:register(t5);
ByteAddressBuffer Auxiliary:register(t6);RWByteAddressBuffer Output:register(u0);
uint index(int3 c){return (c.z*dim.y+c.y)*dim.x+c.x;}
int3 coord(uint i){return int3(i%dim.x,i/dim.x%dim.y,i/(dim.x*dim.y));}
bool inside(int3 c){return all(c>=0)&&all(c<int3(dim));}
bool solid(int3 c){if(!inside(c))return true;return asfloat(Boundary.Load(index(c)*16+12))>.5;}
float wall(int3 c,uint axis){if(!inside(c))return 0;return asfloat(Boundary.Load(index(c)*16+axis*4));}
bool wallFace(int3 c,uint axis,out float value){int3 n=c;n[axis]++;bool x=solid(c),y=solid(n);value=0;if(!x&&!y)return false;value=((x?wall(c,axis):0)+(y?wall(n,axis):0))/(uint(x)+uint(y));return true;}
float face(ByteAddressBuffer velocity,int3 c,uint axis){if(!inside(c))return 0;float w;if(wallFace(c,axis,w))return w;return asfloat(velocity.Load(index(c)*12+axis*4));}
float3 centerVelocity(ByteAddressBuffer velocity,int3 c){if(solid(c))return 0;float3 result;[unroll]for(uint a=0;a<3;a++){int3 p=c;p[a]--;result[a]=.5*(face(velocity,c,a)+face(velocity,p,a));}return result;}
float scalar(ByteAddressBuffer state,int3 c,uint channel){if(offsets[channel]==0xffffffff)return channel==1?ambient:0;return asfloat(state.Load((offsets[channel]+index(c))*4));}
float sampleFace(ByteAddressBuffer velocity,float3 p,uint axis){p-=.5;p[axis]-=.5;int3 lo=int3(floor(p));float3 f=p-lo;float result=0;[loop]for(uint j=0;j<8;j++){int3 c=lo;float w=1;[unroll]for(uint a=0;a<3;a++){uint bit=(j>>a)&1;c[a]+=bit;w*=bit?f[a]:1-f[a];}result+=w*face(velocity,c,axis);}return result;}
float3 sampleVelocity(ByteAddressBuffer velocity,float3 p){return float3(sampleFace(velocity,p,0),sampleFace(velocity,p,1),sampleFace(velocity,p,2));}
float3 trace(float3 start,float3 target){
    float3 delta=target-start;int3 c=int3(floor(start)),direction;float3 next,stride;
    [unroll]for(uint a=0;a<3;a++){float v=delta[a];direction[a]=v>0?1:-1;stride[a]=abs(v)>1e-12?abs(1/v):1e20;next[a]=abs(v)>1e-12?(c[a]+(v>0?1:0)-start[a])/v:1e20;}
    [loop]for(uint j=0;j<8;j++){uint a=next.x<=next.y&&next.x<=next.z?0:next.y<=next.z?1:2;float t=next[a];if(t>=1)return target;c[a]+=direction[a];if(solid(c))return start+delta*max(0,t-1e-4);next[a]+=stride[a];}return start;
}
float sampleScalar(ByteAddressBuffer state,float3 p,uint channel,out float mn,out float mx){
    p-=.5;int3 lo=int3(floor(p));float3 f=p-lo;float result=0,weights=0;mn=3.402823466e38;mx=-mn;
    [loop]for(uint j=0;j<8;j++){int3 c=lo;float w=1;[unroll]for(uint a=0;a<3;a++){uint bit=(j>>a)&1;c[a]+=bit;w*=bit?f[a]:1-f[a];}if(solid(c))continue;float v=scalar(state,c,channel);result+=w*v;weights+=w;mn=min(mn,v);mx=max(mx,v);}
    if(weights<1e-8){result=channel==1?ambient:0;mn=mx=result;}else result/=weights;return result;
}
float divergence(ByteAddressBuffer velocity,int3 c){if(solid(c))return 0;float value=0;[unroll]for(uint a=0;a<3;a++){int3 p=c;p[a]--;value+=(face(velocity,c,a)-face(velocity,p,a))/cellSize;}return value;}
float3 curlAt(int3 c){if(solid(c))return 0;return asfloat(Auxiliary.Load3(index(c)*16));}
float3 confinement(int3 c){if(solid(c))return 0;float3 gradient;[unroll]for(uint a=0;a<3;a++){int3 lo=c,hi=c;lo[a]--;hi[a]++;gradient[a]=(length(curlAt(hi))-length(curlAt(lo)))/(2*cellSize);}float magnitude=length(gradient);return magnitude>1e-6?cross(gradient/magnitude,curlAt(c))*(vorticity*cellSize):0;}
[numthreads(64,1,1)]void CSInitializeState(uint3 id:SV_DispatchThreadID){uint i=id.x;if(i>=cells)return;int3 c=coord(i);[unroll]for(uint a=0;a<3;a++)if(offsets[a]!=0xffffffff)Output.Store((offsets[a]+i)*4,asuint(solid(c)?(a==1?ambient:0):scalar(A,c,a)));}
[numthreads(64,1,1)]void CSInitializeVelocity(uint3 id:SV_DispatchThreadID){uint i=id.x;if(i>=cells)return;int3 c=coord(i);[unroll]for(uint a=0;a<3;a++){float w;Output.Store(i*12+a*4,asuint(wallFace(c,a,w)?w:asfloat(A.Load(i*12+a*4))));}}
[numthreads(64,1,1)]void CSReact(uint3 id:SV_DispatchThreadID){
    uint i=id.x;if(i>=cells)return;int3 c=coord(i);float3 state=float3(0,ambient,0);[unroll]for(uint a=0;a<3;a++)if(offsets[a]!=0xffffffff)state[a]=asfloat(Output.Load((offsets[a]+i)*4));
    if(solid(c)){state=float3(0,ambient,0);}else{
        float3 rate=0;[unroll]for(uint a=0;a<3;a++)if(offsets[a]!=0xffffffff){uint address=(offsets[a]+i)*4;rate[a]=asfloat(Input.Load(address));if(flags&256)rate[a]+=asfloat(Auxiliary.Load(address));}
        state.x+=rate.x*dt;state.y=ambient+(state.y-ambient)*exp(-cooling*dt)+rate.y*dt;state.z=state.z*exp(-fuelDecay*dt)+rate.z*dt;
        if(offsets.y!=0xffffffff&&offsets.z!=0xffffffff&&state.y>=ignition){float burned=state.z*(1-exp(-burnRate*dt));state.z-=burned;state.y+=burned*heatPerFuel;state.x+=burned*sootPerFuel;}state.x*=exp(-densityDecay*dt);
    }
    state=clamp(state,float3(0,ambient,0),float3(maxDensity,maxTemperature,maxFuel));[unroll]for(uint a=0;a<3;a++)if(offsets[a]!=0xffffffff)Output.Store((offsets[a]+i)*4,asuint(state[a]));
}
[numthreads(64,1,1)]void CSAdvectVelocity(uint3 id:SV_DispatchThreadID){uint i=id.x;if(i>=cells)return;int3 c=coord(i);[unroll]for(uint a=0;a<3;a++){float w;if(wallFace(c,a,w)){Output.Store(i*12+a*4,asuint(w));continue;}float3 at=c+.5;at[a]+=.5;float3 back=trace(at,at-sampleVelocity(A,at)*(dt/cellSize));Output.Store(i*12+a*4,asuint(sampleFace(A,back,a)));}}
[numthreads(64,1,1)]void CSCurl(uint3 id:SV_DispatchThreadID){uint i=id.x;if(i>=cells)return;int3 c=coord(i);float3 derivative[3];[unroll]for(uint a=0;a<3;a++){int3 lo=c,hi=c;lo[a]--;hi[a]++;derivative[a]=(centerVelocity(A,hi)-centerVelocity(A,lo))/(2*cellSize);}float3 value=float3(derivative[1].z-derivative[2].y,derivative[2].x-derivative[0].z,derivative[0].y-derivative[1].x);Output.Store4(i*16,asuint(float4(value,0)));}
[numthreads(64,1,1)]void CSForces(uint3 id:SV_DispatchThreadID){uint i=id.x;if(i>=cells)return;int3 c=coord(i);[unroll]for(uint a=0;a<3;a++){float w;if(wallFace(c,a,w)){Output.Store(i*12+a*4,asuint(w));continue;}int3 n=c;n[a]++;float acceleration=.5*(asfloat(Acceleration.Load(i*12+a*4))+asfloat(Acceleration.Load(index(n)*12+a*4)));if(a==1)acceleration+=buoyancy*(.5*(scalar(B,c,1)+scalar(B,n,1))-ambient)-smokeWeight*.5*(scalar(B,c,0)+scalar(B,n,0));if(vorticity>0)acceleration+=.5*(confinement(c)[a]+confinement(n)[a]);Output.Store(i*12+a*4,asuint(clamp(asfloat(A.Load(i*12+a*4))+dt*acceleration,-maxSpeed,maxSpeed)));}}
[numthreads(64,1,1)]void CSDivergence(uint3 id:SV_DispatchThreadID){uint i=id.x;if(i>=cells)return;Output.Store(i*4,asuint(divergence(A,coord(i))));}
[numthreads(64,1,1)]void CSClearPressure(uint3 id:SV_DispatchThreadID){if(id.x<cells)Output.Store(id.x*4,0);}
[numthreads(64,1,1)]void CSPressure(uint3 id:SV_DispatchThreadID){uint i=id.x;if(i>=cells)return;int3 c=coord(i);float sum=0;uint count=0;if(!solid(c)){[unroll]for(uint a=0;a<3;a++){[unroll]for(int sign=-1;sign<=1;sign+=2){int3 n=c;n[a]+=sign;if(!solid(n)){sum+=asfloat(P.Load(index(n)*4));count++;}}}}float old=asfloat(P.Load(i*4));float value=count?old+omega*((sum-cellSize*cellSize*asfloat(Input.Load(i*4)))/count-old):0;Output.Store(i*4,asuint(value));}
[numthreads(64,1,1)]void CSProject(uint3 id:SV_DispatchThreadID){uint i=id.x;if(i>=cells)return;int3 c=coord(i);[unroll]for(uint a=0;a<3;a++){float w;if(wallFace(c,a,w)){Output.Store(i*12+a*4,asuint(w));continue;}int3 n=c;n[a]++;float v=asfloat(A.Load(i*12+a*4))-(asfloat(P.Load(index(n)*4))-asfloat(P.Load(i*4)))/cellSize;Output.Store(i*12+a*4,asuint(clamp(v,-maxSpeed,maxSpeed)));}}
[numthreads(64,1,1)]void CSDiagnostics(uint3 id:SV_DispatchThreadID){uint i=id.x;if(i>=cells)return;int3 c=coord(i);float before=solid(c)?0:asfloat(Input.Load(i*4)),after=divergence(A,c),laplacian=0;if(!solid(c)){float here=asfloat(P.Load(i*4));[unroll]for(uint a=0;a<3;a++){[unroll]for(int sign=-1;sign<=1;sign+=2){int3 n=c;n[a]+=sign;if(!solid(n))laplacian+=(asfloat(P.Load(index(n)*4))-here)/(cellSize*cellSize);}}}Output.Store3(i*12,asuint(float3(before,after,before-laplacian)));}
[numthreads(64,1,1)]void CSAdvectState(uint3 id:SV_DispatchThreadID){uint i=id.x;if(i>=cells)return;int3 c=coord(i);float3 at=c+.5,back=trace(at,at-centerVelocity(B,c)*(dt/cellSize));[unroll]for(uint a=0;a<3;a++)if(offsets[a]!=0xffffffff){float mn,mx;float v=solid(c)?(a==1?ambient:0):sampleScalar(A,back,a,mn,mx);Output.Store((offsets[a]+i)*4,asuint(v));}}
[numthreads(64,1,1)]void CSCorrectState(uint3 id:SV_DispatchThreadID){uint i=id.x;if(i>=cells)return;int3 c=coord(i);if(solid(c))return;float3 at=c+.5,back=trace(at,at-centerVelocity(B,c)*(dt/cellSize));[unroll]for(uint a=0;a<3;a++)if(offsets[a]!=0xffffffff){float mn,mx;sampleScalar(A,back,a,mn,mx);uint address=(offsets[a]+i)*4;float corrected=asfloat(Output.Load(address))+.5*(scalar(A,c,a)-scalar(Auxiliary,c,a));Output.Store(address,asuint(clamp(corrected,mn,mx)));}}
