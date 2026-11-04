#pragma once
#include "signal_mouse.hpp"
#include "dashboard_view.hpp"
#include "cockpit_theme.hpp"
#include <X11/Xft/Xft.h>
#include <chrono>
#include <stdexcept>
#include <cstdint>

namespace signal_demo {
class MouseToolbar {
    Display* display=nullptr;Window parent=0;
    GC gc=nullptr;Pixmap back=0;XftDraw* xft=nullptr;XftFont *normal=nullptr,*small=nullptr;int back_width=0;
    std::array<XftColor,14> inks{};unsigned allocated=0;uint64_t paints_=0;
    unsigned long background=0,button=0,selected=0,stop=0,white=0;
    int last_direction=-1,last_lane_direction=-1;
    bool last_automatic=false,last_lane_enabled=false,last_paused=false,last_alarm=false;
    std::string last_status,last_detail;
    std::chrono::steady_clock::time_point last_paint{};
    bool dirty=true;
    unsigned long color(const char* name,unsigned long fallback){XColor exact{},screen{};
        return XAllocNamedColor(display,DefaultColormap(display,DefaultScreen(display)),name,&screen,&exact)?screen.pixel:fallback;}
    void text(int x,int y,const std::string& value,int limit,int ink,bool caption=false){
        auto font=caption?small:normal;
        auto fitted=driver_demo::fit_label(value,limit,[&](const std::string& s){XGlyphInfo extent{};XftTextExtentsUtf8(display,font,reinterpret_cast<const FcChar8*>(s.data()),int(s.size()),&extent);return extent.xOff;});
        XftDrawStringUtf8(xft,&inks[ink],font,x,y,reinterpret_cast<const FcChar8*>(fitted.data()),int(fitted.size()));
    }
public:
    Window video=0;
    MouseLayout layout;
    MouseGesture gesture;
    void start(Display* d,Window w,int width,int height){
        display=d;parent=w;layout.resize(width,height);
        white=WhitePixel(d,DefaultScreen(d));background=color("#101b26",0);
        button=color(cockpit::Palette[1],0);selected=color(cockpit::Palette[9],white);stop=color(cockpit::Palette[7],0);
        gc=XCreateGC(d,parent,0,nullptr);
        normal=XftFontOpenName(d,DefaultScreen(d),"WenQuanYi Micro Hei:pixelsize=15");small=XftFontOpenName(d,DefaultScreen(d),"WenQuanYi Micro Hei:pixelsize=12");
        if(!normal||!small)throw std::runtime_error("Toolbar requires the existing CJK font");
        for(const auto* name:cockpit::Palette){if(!XftColorAllocName(d,DefaultVisual(d,DefaultScreen(d)),DefaultColormap(d,DefaultScreen(d)),name,&inks[allocated]))throw std::runtime_error("Toolbar color allocation failed");++allocated;}
        XSetWindowBackground(d,parent,background);
        video=XCreateSimpleWindow(d,parent,layout.vx,layout.vy,layout.vw,layout.vh,0,0,0);
        XSelectInput(d,video,ButtonPressMask|ButtonReleaseMask);
        XMapWindow(d,video);
        // Explicit user position prevents the desktop WM's smart placement on the right.
        XSizeHints hints{};hints.flags=PMinSize|USPosition|PWinGravity;hints.x=0;hints.y=0;
        hints.win_gravity=NorthWestGravity;hints.min_width=640;hints.min_height=444;
        XSetWMNormalHints(d,parent,&hints);
    }
    uint64_t paint_count()const{return paints_;}
    void close(){if(xft)XftDrawDestroy(xft);xft=nullptr;if(back)XFreePixmap(display,back);back=0;back_width=0;if(gc)XFreeGC(display,gc);gc=nullptr;
        for(auto f:{normal,small})if(f)XftFontClose(display,f);
        normal=small=nullptr;
        if(display)for(unsigned i=0;i<allocated;++i)XftColorFree(display,DefaultVisual(display,DefaultScreen(display)),DefaultColormap(display,DefaultScreen(display)),&inks[i]);
        allocated=0;}
    void cancel(){gesture.cancel();}
    // Returns a command only on a matching left-button release, never on press.
    char event(const XEvent& e){
        if(e.type==Expose&&e.xexpose.window==parent)dirty=true;
        if(e.type==ConfigureNotify&&e.xconfigure.window==parent){
            layout.resize(e.xconfigure.width,e.xconfigure.height);gesture.cancel();
            XMoveResizeWindow(display,video,layout.vx,layout.vy,layout.vw,layout.vh);XClearWindow(display,parent);dirty=true;
        }
        if((e.type==ButtonPress||e.type==ButtonRelease)&&e.xbutton.button==Button1){
            if(e.type==ButtonPress){
                gesture.press(e.xbutton.window==parent?layout.hit(e.xbutton.x,e.xbutton.y):-1);
                dirty=true;
                XSetInputFocus(display,parent,RevertToParent,CurrentTime);
            }else {dirty=true;return gesture.release(e.xbutton.window==parent?layout.hit(e.xbutton.x,e.xbutton.y):-1);}
        }
        return 0;
    }
    void draw(int direction,bool automatic=false,const std::string& status="",const std::string& detail="",bool lane_enabled=false,bool paused=false,bool alarm=false,int lane_direction=-1){
        if(lane_direction!=last_lane_direction)dirty=true;
        if(!dirty&&direction==last_direction&&automatic==last_automatic&&lane_enabled==last_lane_enabled&&paused==last_paused&&alarm==last_alarm&&status==last_status&&detail==last_detail)return;
        auto paint_now=std::chrono::steady_clock::now();
        if(!dirty&&direction==last_direction&&automatic==last_automatic&&lane_enabled==last_lane_enabled&&paused==last_paused&&alarm==last_alarm&&status==last_status&&paint_now-last_paint<std::chrono::milliseconds(250))return; // age-only updates, never delay commands/state changes
        last_paint=paint_now;
        last_direction=direction;last_lane_direction=lane_direction;last_automatic=automatic;last_lane_enabled=lane_enabled;last_paused=paused;last_alarm=alarm;last_status=status;last_detail=detail;dirty=false;
        ++paints_;
        // Compose the entire bar off-screen. Never expose the clear-and-redraw
        // sequence on the visible parent while age/status text updates.
        if(!back||back_width!=layout.width){if(xft)XftDrawDestroy(xft);if(back)XFreePixmap(display,back);
            back=XCreatePixmap(display,parent,unsigned(layout.width),MouseLayout::bar_height,unsigned(DefaultDepth(display,DefaultScreen(display))));back_width=layout.width;
            xft=XftDrawCreate(display,back,DefaultVisual(display,DefaultScreen(display)),DefaultColormap(display,DefaultScreen(display)));}
        int y=0;
        XSetForeground(display,gc,background);XFillRectangle(display,back,gc,0,y,layout.width,MouseLayout::bar_height);
        for(int i=0;i<int(MouseLayout::keys.size());++i){int x=layout.left(i),width=std::max(1,layout.right(i)-x);
            char command=MouseLayout::keys[i];bool active=i==direction||(command=='j'&&lane_direction==0)||(command=='m'&&lane_direction==2);
            XSetForeground(display,gc,command=='s'?stop:active?selected:command=='q'?background:button);
            XFillRectangle(display,back,gc,x,y+8,width,36);
            XSetForeground(display,gc,inks[active?4:8].pixel);XDrawRectangle(display,back,gc,x,y+8,unsigned(width-1),35);
            if(gesture.down==i){XSetForeground(display,gc,white);XDrawRectangle(display,back,gc,x+1,y+9,width-3,33);}
            const char* compact[]{"路左","直行","路右","左变","右变","取消","核查","通行","暂停","下一","实时","退出"};
            const char* keys[]{"A","W","D","J","M","L","C","SPACE","P","N","V","ESC"};
            const char* label=command=='p'&&paused?(width<84?"继续":"继续播放"):width<84?compact[i]:MouseLayout::labels[i];
            XGlyphInfo extent{};XftTextExtentsUtf8(display,normal,reinterpret_cast<const FcChar8*>(label),int(std::strlen(label)),&extent);
            text(x+std::max(4,(width-int(extent.xOff))/2),y+26,label,width-8,command=='s'?6:active?4:2);
            XftTextExtentsUtf8(display,small,reinterpret_cast<const FcChar8*>(keys[i]),int(std::strlen(keys[i])),&extent);
            text(x+std::max(4,(width-int(extent.xOff))/2),y+40,keys[i],width-8,3,true);
        }
        XSetForeground(display,gc,inks[8].pixel);XDrawLine(display,back,gc,8,51,layout.width-8,51);
        if(alarm){XSetForeground(display,gc,inks[6].pixel);XFillRectangle(display,back,gc,8,60,3,15);}
        text(18,72,status=="红黄灯停车"?"信号停车":status.empty()?"模拟辅助":status,140,alarm?6:2);
        text(174,72,detail.empty()?"模拟辅助提示 · 不控制车辆":detail,layout.width-186,3,true);
        XCopyArea(display,back,parent,gc,0,0,unsigned(layout.width),MouseLayout::bar_height,0,layout.height-MouseLayout::bar_height);
        XFlush(display);
    }
};
}
