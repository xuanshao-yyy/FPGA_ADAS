#pragma once
#include "explanation_state.hpp"
#include "signal_mouse.hpp"
#include <X11/Xft/Xft.h>
#include <cstdio>

namespace driver_demo {
// Separate X11 child. No video conversion, screenshot copying or NN inference.
class ExplanationPanel {
    Display* display=nullptr;Window window=0;GC gc=0;Pixmap back=0;XftDraw* draw=nullptr;
    XftFont *small=nullptr,*normal=nullptr,*large=nullptr;
    std::array<XftColor,8> colors{};unsigned allocated=0;
    int width=0,height=0,x=-1,y=-1,last_severity=-1;bool dirty=true;
    std::chrono::steady_clock::time_point last_paint{};
    void rect(int xx,int yy,int ww,int hh,int color){if(ww<=0||hh<=0)return;XSetForeground(display,gc,colors[color].pixel);XFillRectangle(display,back,gc,xx,yy,unsigned(ww),unsigned(hh));}
    void text(int xx,int yy,std::string value,int color=2,XftFont* font=nullptr,int limit=0){
        if(!font)font=normal;
        if(!limit)limit=width-xx-14;
        auto fits=[&](){XGlyphInfo ext{};XftTextExtentsUtf8(display,font,reinterpret_cast<const FcChar8*>(value.data()),int(value.size()),&ext);return ext.xOff<=limit;};
        bool cut=false;while(!value.empty()&&!fits()){size_t n=value.size()-1;while(n&&(static_cast<unsigned char>(value[n])&0xc0)==0x80)--n;value.resize(n);cut=true;}
        if(cut&&value.size()>3){size_t n=value.size()-1;while(n&&(static_cast<unsigned char>(value[n])&0xc0)==0x80)--n;value.resize(n);value+="…";}
        XftDrawStringUtf8(draw,&colors[color],font,xx,yy,reinterpret_cast<const FcChar8*>(value.data()),int(value.size()));
    }
public:
    ~ExplanationPanel(){close();}
    void close(){
        if(draw)XftDrawDestroy(draw);
        draw=nullptr;
        if(back)XFreePixmap(display,back);
        back=0;
        if(gc)XFreeGC(display,gc);
        gc=0;
        for(auto f:{small,normal,large})if(f)XftFontClose(display,f);
        small=normal=large=nullptr;
        if(display)for(unsigned i=0;i<allocated;++i)XftColorFree(display,DefaultVisual(display,DefaultScreen(display)),DefaultColormap(display,DefaultScreen(display)),&colors[i]);
        allocated=0;if(window)XDestroyWindow(display,window);window=0;display=nullptr;
    }
    void start(Display* d,Window parent){
        display=d;window=XCreateSimpleWindow(d,parent,0,0,1,1,0,0,0);XStoreName(d,window,"RK Detection Explanation");
        XSelectInput(d,window,ExposureMask);gc=XCreateGC(d,window,0,nullptr);
        auto load=[&](int size){std::string pattern="WenQuanYi Micro Hei:pixelsize="+std::to_string(size);return XftFontOpenName(d,DefaultScreen(d),pattern.c_str());};
        small=load(13);normal=load(16);large=load(26);
        if(!small||!normal||!large)throw std::runtime_error("Cannot load explanation panel fonts");
        const char* palette[]{"#0b1320","#17263a","#eef5ff","#8ea7c3","#48d8b0","#ffca69","#ff6975","#351c2b"};
        for(const char* color:palette){if(!XftColorAllocName(d,DefaultVisual(d,DefaultScreen(d)),DefaultColormap(d,DefaultScreen(d)),color,&colors[allocated]))throw std::runtime_error("Cannot allocate panel color");++allocated;}
        XMapWindow(d,window);
    }
    void event(const XEvent& e){if(e.type==Expose&&e.xexpose.window==window)dirty=true;}
    void paint(const signal_demo::MouseLayout& layout,const Explanation& e,const std::string& model,
               const std::array<double,4>& rates,double lane,double cpu,double fps){
        if(width!=layout.pw||height!=layout.ph||x!=layout.px||y!=layout.py){
            width=layout.pw;height=layout.ph;x=layout.px;y=layout.py;
            XMoveResizeWindow(display,window,x,y,unsigned(width),unsigned(height));
            if(draw)XftDrawDestroy(draw);
            if(back)XFreePixmap(display,back);
            back=XCreatePixmap(display,window,unsigned(width),unsigned(height),unsigned(DefaultDepth(display,DefaultScreen(display))));
            draw=XftDrawCreate(display,back,DefaultVisual(display,DefaultScreen(display)),DefaultColormap(display,DefaultScreen(display)));dirty=true;
        }
        auto now=std::chrono::steady_clock::now();
        if(!dirty&&e.severity<=last_severity&&now-last_paint<std::chrono::milliseconds(100))return;
        dirty=false;last_paint=now;last_severity=e.severity;
        rect(0,0,width,height,0);int accent=e.severity>=3?6:e.severity>=1?5:4;
        const bool compact=height<400;
        if(compact){
            int split=width/2;text(14,24,"检测解释",2);text(110,24,model,3,small,width-124);
            rect(10,36,split-16,76,e.severity>=3?7:1);rect(10,36,4,76,accent);
            text(23,66,e.action,accent,large,split-38);text(23,94,e.reason,2,small,split-38);
            text(split+8,55,"左 "+e.signal[0]+" · 直 "+e.signal[1],2,small,width-split-20);
            text(split+8,79,"右 "+e.signal[2]+" · "+e.lane,3,small,width-split-20);
            text(split+8,102,e.target,3,small,width-split-20);
            if(height>145){text(14,137,e.markings+" · "+e.lane_request,3,small);text(14,160,e.scope+" · "+e.risk,3,small);}
        }else{
            text(16,30,"检测解释",2,large);text(16,52,model,3,small);
            rect(10,66,width-20,98,e.severity>=3?7:1);rect(10,66,4,98,accent);
            text(24,103,e.action,accent,large);text(24,132,e.reason,2,small);text(24,151,e.scope,3,small);
            int yy=188;text(16,yy,"交通信号 · 按方向解释",2);yy+=26;
            const char* directions[]{"左转","直行","右转"};
            for(unsigned i=0;i<3;++i){int color=e.signal[i]=="信号禁止"?6:e.signal[i]=="信号允许"?4:3;
                text(20,yy,directions[i],3,small);text(90,yy,e.signal[i],color);yy+=25;}
            yy+=13;text(16,yy,"车道与变道",2);yy+=25;text(20,yy,e.lane,2);yy+=23;text(20,yy,e.markings,3,small);yy+=23;text(20,yy,e.lane_request,5,small);
            yy+=33;text(16,yy,"前方目标与图像风险",2);yy+=25;text(20,yy,e.target,2,small);yy+=23;text(20,yy,e.risk,3,small);yy+=23;text(20,yy,e.source,3,small);
            if(yy+77<height){char row[160];yy+=34;std::snprintf(row,sizeof(row),"前视 %.1f Hz   车道 %.1f Hz",rates[0],lane);text(16,yy,row,4,small);
                yy+=22;std::snprintf(row,sizeof(row),"CH1/2/3  %.1f / %.1f / %.1f Hz",rates[1],rates[2],rates[3]);text(16,yy,row,3,small);
                yy+=22;std::snprintf(row,sizeof(row),"显示 %.1f Hz   全机CPU %.0f%%",fps,cpu);text(16,yy,row,3,small);}
            text(16,height-14,"未验证侧后安全 · 不输出真实米数",3,small);
        }
        XCopyArea(display,back,window,gc,0,0,unsigned(width),unsigned(height),0,0);XFlush(display);
    }
};
}
