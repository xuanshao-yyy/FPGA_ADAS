#pragma once
#include "dashboard_view.hpp"
#include "signal_mouse.hpp"
#include "vehicle_simulation.hpp"
#include "cockpit_theme.hpp"
#include "brand_logo.hpp"
#include <X11/Xft/Xft.h>
#include <cstdio>
#include <stdexcept>

namespace driver_demo {
// Native status band above the existing XVideo child; no full-frame CPU conversion.
class TopDashboard {
    Display* display=nullptr;Window window=0,parent=0;GC gc=0;Pixmap back=0;XftDraw* draw=nullptr;
    XftFont *small=nullptr,*normal=nullptr,*large=nullptr;
    std::array<XftColor,14> colors{};unsigned allocated=0;
    int width=0,height=0,x=-1,y=-1,down=-1,last_severity=-1,painted_audio=-1;
    brand::Logo logo;
    bool dirty=true,front_priority=false;Explanation painted;
    std::string painted_model;
    std::array<double,4> painted_rates{};
    double painted_lane=-1,painted_cpu=-1,painted_fps=-1;
    uint64_t card_paints_=0;
    std::chrono::steady_clock::time_point last_paint{};
    void rect(int xx,int yy,int ww,int hh,int color){
        if(ww<=0||hh<=0)return;
        XSetForeground(display,gc,colors[color].pixel);XFillRectangle(display,back,gc,xx,yy,unsigned(ww),unsigned(hh));
    }
    void border(const DashboardRect& r,int color){
        if(r.w<2||r.h<2)return;
        XSetForeground(display,gc,colors[color].pixel);XDrawRectangle(display,back,gc,r.x,r.y,unsigned(r.w-1),unsigned(r.h-1));
    }
    void text(int xx,int yy,std::string value,int color=2,XftFont* font=nullptr,int limit=0){
        if(!font)font=normal;
        if(!limit)limit=width-xx-8;
        if(limit<=0)return;
        auto measure=[&](const std::string& s){XGlyphInfo ext{};XftTextExtentsUtf8(display,font,reinterpret_cast<const FcChar8*>(s.data()),int(s.size()),&ext);return ext.xOff;};
        value=fit_label(value,limit,measure);
        XftDrawStringUtf8(draw,&colors[color],font,xx,yy,reinterpret_cast<const FcChar8*>(value.data()),int(value.size()));
    }
    void arrows(const DashboardRect& card,const Explanation& e){
        const int cy=card.y+45,start=card.x+card.w-90;
        for(int direction=0;direction<3;++direction){int ink=e.vehicle_arrow==direction?(e.vehicle_state=="STILL"?11:e.vehicle_state=="SLOW"?5:4):e.vehicle_alarm&&(e.lane_requested?e.lane_intent:e.intent)==direction?6:8;
            auto shape=arrow_points(direction);XPoint points[7];for(unsigned i=0;i<shape.size();++i)points[i]={short(start+direction*29+shape[i][0]),short(cy+shape[i][1])};
            XSetForeground(display,gc,colors[ink].pixel);XFillPolygon(display,back,gc,points,7,Complex,CoordModeOrigin);
        }
    }
    void road_icon(const DashboardRect& c,const Explanation& e){
        if(c.w<340)return;
        int xx=c.x+c.w-106,yy=c.y+30;
        rect(xx,yy,92,30,0);rect(xx+15,yy+3,2,24,3);rect(xx+75,yy+3,2,24,3);
        for(int k=0;k<3;++k)rect(xx+45,yy+3+k*9,1,5,8);
        if(e.lane_position_known){int center=xx+46+int(std::clamp(e.lane_offset,-1.2,1.2)*25);
            rect(center-6,yy+7,12,17,e.lane_position_held?5:11);rect(center-4,yy+10,8,4,0);}
        else text(xx+41,yy+22,"?",3,normal,20);
    }
public:
    ~TopDashboard(){close();}
    Window native_window()const{return window;}
    void set_layout(bool value){if(front_priority!=value){front_priority=value;dirty=true;down=-1;}}
    uint64_t card_paints()const{return card_paints_;}
    void close(){
        logo.close();
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
    void start(Display* d,Window owner){
        display=d;parent=owner;window=XCreateSimpleWindow(d,parent,0,0,1,1,0,0,0);
        XStoreName(d,window,"RK Top Driving Dashboard");
        XSelectInput(d,window,ExposureMask|ButtonPressMask|ButtonReleaseMask);gc=XCreateGC(d,window,0,nullptr);
        auto load=[&](int size){std::string pattern="WenQuanYi Micro Hei:pixelsize="+std::to_string(size);return XftFontOpenName(d,DefaultScreen(d),pattern.c_str());};
        small=load(13);normal=load(16);large=load(26);
        if(!small||!normal||!large||!XftCharExists(d,normal,0x8f66))throw std::runtime_error("Dashboard requires a CJK font");
        for(const char* color:cockpit::Palette){if(!XftColorAllocName(d,DefaultVisual(d,DefaultScreen(d)),DefaultColormap(d,DefaultScreen(d)),color,&colors[allocated]))throw std::runtime_error("Cannot allocate dashboard color");++allocated;}
        logo.start(d,window);XMapWindow(d,window);
    }
    char event(const XEvent& e){
        if(e.type==Expose&&e.xexpose.window==window)dirty=true;
        if(e.type==FocusOut){down=-1;dirty=true;return 0;}
        if((e.type==ButtonPress||e.type==ButtonRelease)&&e.xbutton.window==window&&e.xbutton.button==Button1){
            int hit=DashboardLayout{width,height}.lane_hit(e.xbutton.x,e.xbutton.y);
            if(width>=640&&e.xbutton.x>=width-368&&e.xbutton.x<width-252&&e.xbutton.y>=3&&e.xbutton.y<28)hit=3;
            if(width>=640&&painted_audio>=0&&e.xbutton.x>=width-244&&e.xbutton.x<width-142&&e.xbutton.y>=3&&e.xbutton.y<28)hit=4;
            if(width>=640&&e.xbutton.x>=width-128&&e.xbutton.x<width-12&&e.xbutton.y>=3&&e.xbutton.y<28)hit=2;
            dirty=true;
            if(e.type==ButtonPress){down=hit;XSetInputFocus(display,parent,RevertToParent,CurrentTime);}
            else {int old=down;down=-1;if(hit>=0&&old==hit)return hit==4?'!':hit==3?'~':hit==2?'u':hit==0?'j':'m';}
        }
        return 0;
    }
    void paint(const signal_demo::MouseLayout& layout,const Explanation& e,const std::string& model,
               const std::array<double,4>& rates,double lane,double cpu,double fps,int audio_state=-1){
        if(width!=layout.pw||height!=layout.ph||x!=layout.px||y!=layout.py){
            width=layout.pw;height=layout.ph;x=layout.px;y=layout.py;down=-1;
            XMoveResizeWindow(display,window,x,y,unsigned(width),unsigned(height));
            if(draw)XftDrawDestroy(draw);
            if(back)XFreePixmap(display,back);
            back=XCreatePixmap(display,window,unsigned(width),unsigned(height),unsigned(DefaultDepth(display,DefaultScreen(display))));
            draw=XftDrawCreate(display,back,DefaultVisual(display,DefaultScreen(display)),DefaultColormap(display,DefaultScreen(display)));dirty=true;
        }
        // Compare existing values first: no formatted metrics, concatenated
        // signatures or temporary card strings on every preview event tick.
        bool header_changed=dirty||audio_state!=painted_audio||model!=painted_model||e.still!=painted.still||e.severity!=painted.severity||(e.severity>=2&&e.reason!=painted.reason);
        bool metrics_changed=dirty||rates!=painted_rates||lane!=painted_lane||cpu!=painted_cpu||fps!=painted_fps;
        bool lane_state=e.lane!=painted.lane||e.lane_position_known!=painted.lane_position_known||
            e.lane_position_held!=painted.lane_position_held||e.lane_requested!=painted.lane_requested||e.lane_intent!=painted.lane_intent||e.lane_styles!=painted.lane_styles;
        bool vehicle_changed=e.vehicle_state!=painted.vehicle_state||e.vehicle_label!=painted.vehicle_label||e.vehicle_arrow!=painted.vehicle_arrow||e.vehicle_alarm!=painted.vehicle_alarm;
        bool semantic_changed=vehicle_changed||e.signal!=painted.signal||e.signal_light!=painted.signal_light||e.intent!=painted.intent||e.still!=painted.still||
            e.action!=painted.action||e.severity!=painted.severity||lane_state;
        std::array<bool,3> repaint{{
            dirty||e.signal!=painted.signal||e.signal_light!=painted.signal_light||e.signal_detail!=painted.signal_detail||e.signal_hint!=painted.signal_hint||e.intent!=painted.intent,
            dirty||lane_state||e.lane_detail!=painted.lane_detail||e.lane_request!=painted.lane_request||e.lane_pixels!=painted.lane_pixels||
                e.intent!=painted.intent||int(e.lane_offset*25)!=int(painted.lane_offset*25),
            dirty||vehicle_changed||e.action!=painted.action||e.reason!=painted.reason||e.target!=painted.target||e.scope!=painted.scope||e.severity!=painted.severity}};
        auto now=std::chrono::steady_clock::now();
        if(!dirty&&!header_changed&&!metrics_changed&&!repaint[0]&&!repaint[1]&&!repaint[2])return;
        bool urgent=e.severity>last_severity;
        for(unsigned i=0;i<3;++i)urgent|=signal_badge(e.signal[i],e.signal_light[i]).color==6&&signal_badge(painted.signal[i],painted.signal_light[i]).color!=6;
        for(unsigned i=0;i<2;++i)urgent|=e.lane_styles[i]==1&&painted.lane_styles[i]!=1;
        // Only age/pixel/count drift is coalesced. Changed decisions, markings,
        // intent, mouse interaction and new restrictions paint immediately.
        if(!dirty&&!urgent&&!semantic_changed&&!header_changed&&now-last_paint<std::chrono::milliseconds(250))return;
        bool full=dirty;dirty=false;last_paint=now;last_severity=e.severity;
        painted=e;painted_model=model;painted_rates=rates;painted_lane=lane;painted_cpu=cpu;painted_fps=fps;painted_audio=audio_state;
        DashboardLayout geometry{width,height};bool compact=geometry.compact();
        if(full)rect(0,0,width,height,0);
        if(header_changed){rect(0,0,width,31,0);logo.paint(back,gc,8,0);text(128,22,"驾驶辅助",2,normal,88);
            text(226,21,e.severity>=2?e.reason:e.still?"静帧检测 / STILL": "实时感知 / LIVE",e.severity>=3?6:e.severity==2?5:3,small,width-(width>=640?(audio_state>=0?486:376):238));
            if(width>=640){rect(width-368,3,116,25,down==3?8:1);border({width-368,3,116,25},8);text(width-352,21,"切换布局",4,small,92);}
            if(width>=640&&audio_state>=0){rect(width-244,3,102,25,down==4?8:1);border({width-244,3,102,25},8);text(width-236,21,audio_state==0?"声音关闭":audio_state==1?"声音开启":"音频待查",audio_state==2?5:3,small,90);}
            if(width>=640){rect(width-128,3,116,25,down==2?8:1);border({width-128,3,116,25},8);text(width-112,21,"360 环视",4,small,92);}}
        const char* headings[]{"交通信号 / SIGNAL","车道与变道 / LANE","综合驾驶提示 / ADVISORY"};
        for(int i=0;i<3;++i)if(repaint[i]){++card_paints_;auto c=geometry.card(i);rect(c.x,c.y,c.w,c.h,i==2&&e.vehicle_alarm?7:1);rect(c.x,c.y,c.w,2,i==2&&e.severity>=3?6:i==2&&e.severity==2?5:8);text(c.x+12,c.y+20,headings[i],3,small,c.w-22);}
        if(repaint[0]){
        auto signal_card=geometry.card(0);int tile_width=std::max(1,(signal_card.w-32)/3);
        const char* directions[]{"左转","直行","右转"};
        for(int i=0;i<3;++i){auto badge=signal_badge(e.signal[i],e.signal_light[i]);int xx=signal_card.x+10+i*(tile_width+6),yy=signal_card.y+30,hh=compact?38:64;
            if(!e.signal_hint[i].empty()&&badge.color==5&&e.signal_light[i]=="UNKNOWN")badge={e.signal_hint[i].c_str(),11};
            rect(xx,yy,tile_width,hh,0);rect(xx,yy,2,hh,badge.color==6?6:badge.color==4?4:badge.color==5?5:8);
            text(xx+6,yy+15,directions[i],3,small,tile_width-10);
            text(xx+6,yy+(compact?32:45),badge.label,badge.color,compact?small:tile_width<90?normal:large,tile_width-10);
            if(e.intent==i)border({xx,yy,tile_width,hh},8);
        }
        if(!compact)text(signal_card.x+12,signal_card.y+115,e.signal_detail[std::clamp(e.intent,0,2)],2,small,signal_card.w-22);
        text(signal_card.x+12,signal_card.y+signal_card.h-10,std::string("意图：")+directions[std::clamp(e.intent,0,2)]+" · 仅表示信号条件",3,small,signal_card.w-22);
        }
        if(repaint[1]){
        auto lane_card=geometry.card(1);if(!compact)road_icon(lane_card,e);
        text(lane_card.x+12,lane_card.y+(compact?44:55),lane_title(e),e.lane_position_known?(e.lane_position_held?5:11):3,compact?normal:large,lane_card.w-(lane_card.w>=340?122:24));
        for(int side=0;side<2;++side){auto button=geometry.lane_button(side);auto badge=marking_badge(e.lane_styles[side]);
            bool requested=e.lane_requested&&e.lane_intent==(side==0?0:2);
            rect(button.x,button.y,button.w,button.h,requested?(badge.color==6?7:9):0);
            text(button.x+6,button.y+18,std::string(side==0?"左 ":"右 ")+badge.label,badge.color,small,button.w-12);
            if(down==side||(e.lane_requested&&e.lane_intent==(side==0?0:2)))border(button,2);
        }
        if(!compact)text(lane_card.x+12,lane_card.y+116,e.lane_pixels,3,small,lane_card.w-22);
        text(lane_card.x+12,lane_card.y+lane_card.h-10,e.lane_requested?e.lane_request:"点左/右标线请求变道 · 与信号共同判断",e.lane_requested?5:3,small,lane_card.w-22);
        }
        if(repaint[2]){
        auto risk_card=geometry.card(2);int accent=e.severity>=3?6:e.severity==2?5:e.vehicle_moving?4:e.manual_stop?5:2;
        std::string action=e.vehicle_label=="红黄灯停车"?"信号停车":e.vehicle_label;arrows(risk_card,e);
        text(risk_card.x+12,risk_card.y+(compact?44:55),action,accent,compact?normal:large,risk_card.w-116);
        text(risk_card.x+12,risk_card.y+(compact?63:79),(e.manual_stop||e.vehicle_alarm?e.vehicle_reason:e.reason),2,small,risk_card.w-24);
        text(risk_card.x+12,risk_card.y+(compact?81:101),e.target,3,small,risk_card.w-24);
        if(!compact)text(risk_card.x+12,risk_card.y+123,e.still?"静帧条件提示 · 不模拟连续运动":"模拟辅助提示 · 图像近远，非实测米数",3,small,risk_card.w-24);
        }
        if(metrics_changed){char metrics[256];std::snprintf(metrics,sizeof(metrics),"前视 %.1f Hz   车道 %.1f Hz     左 / 右 / 后  %.1f / %.1f / %.1f Hz     显示 %.1f Hz     CPU %.0f%%",rates[0],lane,rates[2],rates[1],rates[3],fps,cpu);
            rect(0,height-25,width,25,0);text(12,height-8,metrics,3,small,width-24);}
        if(full)XCopyArea(display,back,window,gc,0,0,unsigned(width),unsigned(height),0,0);
        else {
            if(header_changed)XCopyArea(display,back,window,gc,0,0,unsigned(width),31,0,0);
            for(int i=0;i<3;++i)if(repaint[i]){auto r=geometry.card(i);XCopyArea(display,back,window,gc,r.x,r.y,unsigned(r.w),unsigned(r.h),r.x,r.y);}
            if(metrics_changed)XCopyArea(display,back,window,gc,0,height-25,unsigned(width),25,0,height-25);
        }
        XFlush(display);
    }
};
}
