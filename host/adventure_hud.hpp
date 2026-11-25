#pragma once
#include "../renderer/d3d12_renderer.h"
#include "../engine/native/game.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace host {
inline std::wstring hudWide(const std::string& s) {
    const int count=MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring out(count,L'\0');MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),count);return out;
}
// GDI supplies installed CJK glyphs; the native GPU draws the resulting scan runs.
// This keeps text, crosshair and objectives in the same capture as the 3D scene.
inline std::vector<nativegpu::OverlayLayer> adventureHud(const engine::Game& game,unsigned width,unsigned height,bool paused,bool tour,bool completionVisible,const std::wstring& notice) {
    using nativegpu::OverlayLayer;
    std::vector<OverlayLayer> layers(3);
    const float colors[3][4]={{.025f,.055f,.07f,1},{.20f,.78f,.74f,1},{.87f,.93f,.94f,1}};
    for(int i=0;i<3;++i)std::copy(colors[i],colors[i]+4,layers[i].color);
    const int w=static_cast<int>(width),h=static_cast<int>(height);
    const int card=std::min(620,w-48);
    layers[0].rectangles={{20,20,20+card,144},{20,h-112,w-20,h-20}};
    layers[1].rectangles={{20,20,24,144},{20,h-112,24,h-20}};
    const auto& world=game.gameplayRuntime().world;
    engine::gameplay::Handle actor;
    for(auto a:world.query("objective")){actor=a;break;}
    if(!world.valid(actor))for(auto a:world.query("camera")){actor=a;break;}
    const auto* objective=world.valid(actor)?world.get(actor,"objective"):nullptr;
    const auto* inventory=world.valid(actor)?world.get(actor,"inventory"):nullptr;
    const auto* hud=world.valid(actor)?world.get(actor,"hud"):nullptr;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=w;
    info.bmiHeader.biHeight=-h;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    HDC dc=CreateCompatibleDC(nullptr);void* bits=nullptr;
    HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!dc||!bitmap||!bits){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);return layers;}
    auto oldBitmap=SelectObject(dc,bitmap);std::memset(bits,0,static_cast<size_t>(w)*h*4);
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
    HFONT regular=CreateFontW(-17,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,NONANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    HFONT large=CreateFontW(-23,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,NONANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    auto oldFont=SelectObject(dc,regular);
    auto text=[&](const std::wstring& value,RECT rect,bool title=false){SelectObject(dc,title?large:regular);DrawTextW(dc,value.c_str(),static_cast<int>(value.size()),&rect,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOPREFIX);};
    text(L"雾镇重启  /  FOGTOWN",{40,30,card,63},true);
    if(objective){
        text(hudWide(objective->value("title",std::string())),{40,69,card-90,94});
        text(hudWide(objective->value("detail",std::string())),{40,98,card,140});
        std::wostringstream step;step<<objective->value("step",1)<<L" / "<<objective->value("total",5);
        text(step.str(),{card-65,70,card,94});
        const auto& target=objective->at("position");const auto p=game.playerPosition();
        const float dx=target[0].get<float>()-p.x,dz=target[2].get<float>()-p.z;
        const float distance=std::sqrt(dx*dx+dz*dz);
        const float angle=std::remainder(std::atan2(dx,-dz)-game.yaw(),6.2831853f);
        const wchar_t* direction=std::abs(angle)<.4f?L"前方":std::abs(angle)>2.4f?L"身后":angle>0?L"右侧":L"左侧";
        std::wostringstream marker;marker<<direction<<L"  ·  "<<static_cast<int>(distance)<<L" m";
        layers[0].rectangles.push_back({w/2-95,158,w/2+95,188});text(marker.str(),{w/2-78,162,w/2+90,186});
    }
    const int cx=w/2,cy=h/2;
    layers[1].rectangles.insert(layers[1].rectangles.end(),{{cx-8,cy-1,cx-3,cy+1},{cx+3,cy-1,cx+8,cy+1},{cx-1,cy-8,cx+1,cy-3},{cx-1,cy+3,cx+1,cy+8}});
    const auto prompt=game.interactionPrompt();
    if(!prompt.empty()){
        layers[0].rectangles.push_back({std::max(24,cx-250),cy+28,std::min(w-24,cx+250),cy+65});
        text(L"[E]  "+hudWide(prompt),{std::max(36,cx-230),cy+35,std::min(w-36,cx+235),cy+63});
    }
    std::wstring items=L"装备  ";
    if(inventory){const auto& values=inventory->at("items");if(values.value("warehouse_key",0))items+=L"仓库钥匙  ";if(values.value("repair_tool",0))items+=L"维修工具  ";if(values.value("service_key",0))items+=L"备用钥匙  ";}
    if(items==L"装备  ")items+=L"暂无";
    text(items,{40,h-103,w-40,h-80});
    if(!notice.empty())text(notice,{40,h-76,w-40,h-51});
    else if(hud&&hud->value("remaining",0)>0)text(hudWide(hud->value("text",std::string())),{40,h-76,w-40,h-51});
    text(tour?L"开发者导览 · F2 切换完整检查点 · F1 关闭导览 · F7 保存 · F12 恢复":L"WASD 移动 · Shift 跑 · 空格 跳 · E 交互 · Esc 暂停 · F7 保存 · F12 恢复",{40,h-45,w-40,h-22});
    for(const auto& b:game.gameplayRuntime().behaviors())if(b.script=="fogtown/quest"&&b.state.value("running",false)){
        const auto ticks=b.state.value("deadline",0ull);const auto remaining=ticks>game.gameplayRuntime().ticks()?ticks-game.gameplayRuntime().ticks():0;
        layers[0].rectangles.push_back({w-220,22,w-20,65});
        text(L"中继连接  "+std::to_wstring((remaining+119)/120)+L" 秒",{w-205,31,w-25,60},true);
    }
    if((game.complete()&&completionVisible)||paused){
        layers[0].rectangles.push_back({cx-260,cy-90,cx+260,cy+15});
        text(game.complete()&&completionVisible?L"信号已发出 · 雾镇重启":L"已暂停",{cx-235,cy-76,cx+240,cy-42},true);
        text(game.complete()&&completionVisible?L"小镇恢复照明。按 Esc 自由探索，或 F5 重新开始。":L"点击画面继续探索。Esc 释放鼠标，关闭窗口退出。",{cx-235,cy-35,cx+235,cy+6});
    }
    GdiFlush();
    const auto* pixels=static_cast<const unsigned*>(bits);
    for(int y=0;y<h;++y)for(int x=0;x<w;){
        if((pixels[y*w+x]&0xffffff)==0){++x;continue;}
        const int start=x;while(x<w&&(pixels[y*w+x]&0xffffff)!=0)++x;
        layers[2].rectangles.push_back({start,y,x,y+1});
    }
    SelectObject(dc,oldFont);SelectObject(dc,oldBitmap);DeleteObject(regular);DeleteObject(large);DeleteObject(bitmap);DeleteDC(dc);
    return layers;
}
}
