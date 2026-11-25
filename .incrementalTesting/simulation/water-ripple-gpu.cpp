#include "../../tests/d3d12_test_rig.hpp"
#include "../../renderer/water_draw.hpp"
#include <algorithm>
#include <cmath>

namespace {
unsigned checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
std::vector<float> sample(gpu_test::Rig& rig,nativegpu::WaterRipple& ripple){
    const auto desc=ripple.result()->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT rows=0;UINT64 rowBytes=0,total=0;
    rig.device.p->GetCopyableFootprints(&desc,0,1,0,&layout,&rows,&rowBytes,&total);
    gpu_test::Com<ID3D12Resource> readback;
    gpu_test::makeBuffer(rig.device.p,readback,total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    constexpr auto readable=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    rig.begin();gpu_test::transition(rig.commands.p,ripple.result(),readable,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source{},target{};source.pResource=ripple.result();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    target.pResource=readback.p;target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;target.PlacedFootprint=layout;
    rig.commands.p->CopyTextureRegion(&target,0,0,0,&source,nullptr);
    gpu_test::transition(rig.commands.p,ripple.result(),D3D12_RESOURCE_STATE_COPY_SOURCE,readable);rig.submit();
    void* data=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(total)};gpu_test::hr(readback.p->Map(0,&range,&data),"Map ripple evidence");
    std::vector<float> result(desc.Width*desc.Height);
    for(unsigned y=0;y<desc.Height;++y)std::memcpy(result.data()+y*desc.Width,static_cast<char*>(data)+layout.Offset+y*layout.Footprint.RowPitch,desc.Width*sizeof(float));
    D3D12_RANGE written{0,0};readback.p->Unmap(0,&written);return result;
}
nativegpu::WaterSurfaceFrame frame(float extentZ=8){
    nativegpu::WaterSurfaceFrame f;f.columns=f.rows=65;f.rippleOrigin={-4,-extentZ*.5f};f.rippleExtent={8,extentZ};
    f.vertices.resize(f.columns*f.rows);
    for(auto& v:f.vertices)v.depth=.4f;
    return f;
}
void advance(gpu_test::Rig& rig,nativegpu::WaterRipple& ripple,nativegpu::WaterSurfaceFrame& f,unsigned tick){
    f.tick=tick;f.time=tick/120.f;unsigned injected=0;std::string error;
    rig.begin();require(ripple.update(rig.commands.p,tick%3,f,injected,error),error.c_str());rig.submit();
    require(injected==(tick==1?1u:0u),"an event must be injected exactly once");
}
std::vector<float> simulate(gpu_test::Rig& rig,bool retain,bool directed=false,float extentZ=8,unsigned ticks=24,float flowX=0){
    nativegpu::WaterRipple ripple(rig.device.p);auto f=frame(extentZ);
    for(auto& v:f.vertices)v.velocity[0]=flowX;
    nativegpu::WaterRippleEvent e;e.tick=1;e.radius=.18f;e.displacement=-.03f;
    e.sequence=1;e.timelineEpoch=0;
    if(directed){e.directionX=1;e.stretch=1/1.55f;}
    f.events.push_back(e);
    for(unsigned tick=1;tick<=ticks;++tick){advance(rig,ripple,f,tick);if(!retain)f.events.clear();}
    return sample(rig,ripple);
}
double centroidX(const std::vector<float>& h,float extentX){
    double sum=0,weighted=0;
    for(unsigned z=0;z<256;++z)for(unsigned x=0;x<256;++x){
        const double px=(x+.5)/256*extentX-extentX*.5,w=std::fabs(h[z*256+x]);
        sum+=w;weighted+=w*px;
    }
    return sum>1e-9?weighted/sum:0;
}
double aspect(const std::vector<float>& h,float extentZ){
    double xx=0,zz=0;
    for(unsigned z=0;z<256;++z)for(unsigned x=0;x<256;++x){
        const double px=(x+.5)/256*8-4,pz=(z+.5)/256*extentZ-extentZ*.5;
        const double w=std::fabs(h[z*256+x]);xx+=w*px*px;zz+=w*pz*pz;
    }return xx/zz;
}
void drawBinding(gpu_test::Rig& rig){
    // Render a known zero-height field over a bright opaque scene. Sampling t0
    // instead of t5 produces 5.3 rather than 0.5 in the raw HDR debug output.
    gpu_test::Com<ID3D12Resource> colour[2],depth,shadow;
    const auto texture=[&](gpu_test::Com<ID3D12Resource>& target,DXGI_FORMAT format,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state){
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=32;
        d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Format=format;d.Flags=flags;
        gpu_test::hr(rig.device.p->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(target.out())),"Create draw fixture texture");
    };
    for(auto& c:colour)texture(c,DXGI_FORMAT_R16G16B16A16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET);
    texture(depth,DXGI_FORMAT_R32_TYPELESS,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    texture(shadow,DXGI_FORMAT_R16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET);
    gpu_test::Com<ID3D12DescriptorHeap> rtvs,dsvs;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=3;
    gpu_test::hr(rig.device.p->CreateDescriptorHeap(&hd,IID_PPV_ARGS(rtvs.out())),"Create draw RTVs");
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;
    gpu_test::hr(rig.device.p->CreateDescriptorHeap(&hd,IID_PPV_ARGS(dsvs.out())),"Create draw DSV");
    const unsigned stride=rig.device.p->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto rtv=rtvs.p->GetCPUDescriptorHandleForHeapStart();auto handle=rtv;
    for(auto& c:colour){rig.device.p->CreateRenderTargetView(c.p,nullptr,handle);handle.ptr+=stride;}
    rig.device.p->CreateRenderTargetView(shadow.p,nullptr,handle);
    const auto dsv=dsvs.p->GetCPUDescriptorHandleForHeapStart();D3D12_DEPTH_STENCIL_VIEW_DESC view{};view.Format=DXGI_FORMAT_D32_FLOAT;
    view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;rig.device.p->CreateDepthStencilView(depth.p,&view,dsv);
    auto f=frame();f.columns=f.rows=2;f.vertices.resize(4);f.debugMode=6;f.tick=1;f.time=1/120.f;
    for(unsigned z=0;z<2;++z)for(unsigned x=0;x<2;++x){auto& v=f.vertices[z*2+x];v.position[0]=x*8.f-4;v.position[2]=z*8.f-4;v.depth=.4f;}
    nativegpu::WaterRipple ripple(rig.device.p);nativegpu::WaterDraw draw(rig.device.p);
    rig.begin();unsigned injected=0;std::string error;
    require(ripple.update(rig.commands.p,0,f,injected,error),error.c_str());
    const float bright[4]{.8f,.8f,.8f,1};handle=rtv;
    for(auto& c:colour){rig.commands.p->ClearRenderTargetView(handle,bright,0,nullptr);handle.ptr+=stride;
        gpu_test::transition(rig.commands.p,c.p,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);}
    rig.commands.p->ClearRenderTargetView(handle,bright,0,nullptr);
    gpu_test::transition(rig.commands.p,shadow.p,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    rig.commands.p->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
    gpu_test::transition(rig.commands.p,depth.p,D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    rig.commands.p->OMSetRenderTargets(2,&rtv,TRUE,&dsv);
    const D3D12_VIEWPORT viewport{0,0,32,32,0,1};const D3D12_RECT scissor{0,0,32,32};
    rig.commands.p->RSSetViewports(1,&viewport);rig.commands.p->RSSetScissorRects(1,&scissor);
    nativegpu::Mat4 vp{};vp.m[0]=vp.m[9]=.25f;vp.m[6]=.1f;vp.m[14]=.5f;vp.m[15]=1;
    nativegpu::WaterLighting light;light.camera[1]=5;
    require(draw.record(rig.commands.p,0,32,32,colour[0].p,colour[1].p,depth.p,shadow.p,ripple.result(),f,vp,light,error),error.c_str());
    gpu_test::transition(rig.commands.p,colour[0].p,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    const auto desc=colour[0].p->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT rows=0;UINT64 bytes=0,total=0;
    rig.device.p->GetCopyableFootprints(&desc,0,1,0,&layout,&rows,&bytes,&total);
    gpu_test::Com<ID3D12Resource> output;gpu_test::makeBuffer(rig.device.p,output,total,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=colour[0].p;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=output.p;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=layout;
    rig.commands.p->CopyTextureRegion(&dst,0,0,0,&src,nullptr);rig.submit();
    void* data=nullptr;D3D12_RANGE range{0,static_cast<SIZE_T>(total)};gpu_test::hr(output.p->Map(0,&range,&data),"Read raw water draw");
    for(unsigned y=8;y<24;++y)for(unsigned x=8;x<24;++x){
        auto* pixel=reinterpret_cast<const std::uint16_t*>(static_cast<char*>(data)+layout.Offset+y*layout.Footprint.RowPitch)+x*4;
        require(pixel[0]==0x3800&&pixel[1]==0x3800&&pixel[2]==0x3800,"water draw sampled scene colour instead of zero ripple height");
    }
    D3D12_RANGE written{0,0};output.p->Unmap(0,&written);
}
}
int main(){try{
    gpu_test::Rig rig;
    const auto first=simulate(rig,true,false,8,1);
    const auto minimum=*std::min_element(first.begin(),first.end());
    require(minimum<-.001f,"first-frame reset swallowed the contact or substep constants lost the injection");
    double volume=0;for(float h:first){require(std::isfinite(h)&&std::fabs(h)<=.030001f,"residual is finite and bounded");volume+=h;}
    require(std::fabs(volume)*(.03125*.03125)<.0001,"contact kernel adds negligible net volume");
    const auto retained=simulate(rig,true),removed=simulate(rig,false),replay=simulate(rig,true);
    require(retained==removed,"retained history reinjected an already consumed contact");
    require(retained==replay,"GPU contact replay differs");
    double fourth=0,energy=0;
    for(unsigned z=0;z<256;++z)for(unsigned x=0;x<256;++x){
        double px=x-127.5,pz=z-127.5,w=std::fabs(retained[z*256+x]);
        fourth+=w*std::cos(4*std::atan2(pz,px));energy+=w;
    }
    const double squareBias=std::fabs(fourth)/energy;
    require(energy>.001,"propagated ripple field is empty");
    require(squareBias<.1,"radial ripple has excessive four-fold grid bias");
    const auto stretched=simulate(rig,true,true,8,1);
    const double directedAspect=aspect(stretched,8);
    require(directedAspect>1.6,"moving contact lost its directional stretch");
    const auto rectangle=simulate(rig,true,false,4,1);
    require(std::fabs(aspect(rectangle,4)-1)<.1,"rectangular footprint distorts a radial contact");
    // A current transports the residual. The velocity snapshot is the same one the CPU
    // publishes; the residual never integrates its own flow.
    const auto stillWater=simulate(rig,false,false,8,24,0.f);
    const auto flowing=simulate(rig,false,false,8,24,.5f);
    const double stillCentre=centroidX(stillWater,8),flowCentre=centroidX(flowing,8);
    const double flowShift=flowCentre-stillCentre;
    require(std::fabs(stillCentre)<.05,"a contact on still water stays centred");
    require(flowShift>.05,"the residual is advected downstream by the current");
    // Same-tick multiplicity and batch overflow: 20 contacts arrive with 20 sequences;
    // one dispatch carries 16 and the remaining 4 must follow, not be skipped.
    {
        nativegpu::WaterRipple ripple(rig.device.p);auto f=frame();f.tick=1;f.time=1.f/120;
        for(unsigned i=0;i<20;++i){
            nativegpu::WaterRippleEvent e;e.tick=1;e.radius=.1f;e.displacement=-.002f;
            e.sequence=i+1;e.timelineEpoch=0;f.events.push_back(e);
        }
        unsigned injected=0;std::string error;
        rig.begin();require(ripple.update(rig.commands.p,0,f,injected,error),error.c_str());rig.submit();
        require(injected==16,"a full batch injects its capacity");
        // The history still contains all 20 contacts; only the dispatch capacity changed.
        f.tick=2;f.time=2.f/120;
        rig.begin();require(ripple.update(rig.commands.p,1,f,injected,error),error.c_str());rig.submit();
        require(injected==4,"events beyond the batch capacity are delivered on the next frame");
        rig.begin();require(ripple.update(rig.commands.p,2,f,injected,error),error.c_str());rig.submit();
        require(injected==0,"an already delivered batch is not injected twice");
    }
    // A new timeline epoch restarts sequence at 1. A cursor that only remembered the
    // sequence would treat the new contact as a duplicate and show nothing.
    {
        nativegpu::WaterRipple ripple(rig.device.p);auto f=frame();f.tick=1;f.time=1.f/120;
        nativegpu::WaterRippleEvent e;e.tick=1;e.radius=.15f;e.displacement=-.02f;
        e.sequence=1;e.timelineEpoch=0;f.events.push_back(e);
        unsigned injected=0;std::string error;
        rig.begin();require(ripple.update(rig.commands.p,0,f,injected,error),error.c_str());rig.submit();
        require(injected==1,"the first timeline delivers its contact");
        f.tick=2;f.time=2.f/120;
        rig.begin();require(ripple.update(rig.commands.p,1,f,injected,error),error.c_str());rig.submit();
        require(injected==0,"the same timeline does not redeliver");
        f.timelineEpoch=1;f.tick=3;f.time=3.f/120;f.events.clear();
        e.sequence=1;e.timelineEpoch=1;e.tick=3;f.events.push_back(e);
        rig.begin();require(ripple.update(rig.commands.p,2,f,injected,error),error.c_str());rig.submit();
        require(injected==1,"a new epoch restarts delivery instead of filtering the reset sequence");
    }
    drawBinding(rig);
    require(rig.debugErrors()==0,"D3D12 reported invalid ripple operations");
    std::cout<<"{\"success\":true,\"checks\":"<<checks<<",\"squareBias\":"<<squareBias
        <<",\"directedAspect\":"<<directedAspect<<",\"firstMinimum\":"<<minimum
        <<",\"flowShift\":"<<flowShift<<"}\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
