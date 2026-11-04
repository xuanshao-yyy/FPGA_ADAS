#pragma once
#include "dashboard_view.hpp"
#include "signal_mouse.hpp"
#include "surround_view.hpp"
#include "cockpit_theme.hpp"
#include <X11/Xft/Xft.h>
#include <cstdio>

namespace surround_ui {
// rkximagesink scans NV12 below Xorg using RGB(1,2,3) as its color key.
// X11 must repaint this same key when a child panel moves away, not black.
inline void prepare_rk_video_background(Display* display,Window video){
    XSetWindowBackground(display,video,0x010203);XClearWindow(display,video);
}
// Small native windows occupy only the two matte corners of the surround view.
// Cached pixmaps: no per-frame video conversion or new perception inference.
class InfoPanels {
    struct Panel {Window window=0;Pixmap back=0;XftDraw* draw=nullptr;int x=-1,y=-1,w=0,h=0;bool dirty=true;};
    Display* display=nullptr;Window owner=0,video=0;GC gc=0;
    std::array<Panel,2> panels{};std::array<XftColor,9> colors{};unsigned allocated=0;
    std::array<int,4> last_near{};std::array<bool,4> last_fresh{},last_fast{};
    XftFont *normal=nullptr,*small=nullptr;int font_size=0,down=-1;bool visible=false;
    uint64_t paints_=0;bool front_priority=false;
    std::array<unsigned,4> last_order{{0,1,2,3}};
    bool last_file=false,last_paused=false,last_lane_requested=false;int last_lane_intent=1;std::string last_scene,last_detail;
    void fill(Panel& p,int x,int y,int w,int h,int color){XSetForeground(display,gc,colors[color].pixel);XFillRectangle(display,p.back,gc,x,y,unsigned(std::max(1,w)),unsigned(std::max(1,h)));}
    void text(Panel& p,int x,int y,std::string value,int color=2,bool caption=false){
        auto font=caption?small:normal;
        value=driver_demo::fit_label(value,p.w-x-12,[&](const std::string& s){XGlyphInfo extent{};XftTextExtentsUtf8(display,font,reinterpret_cast<const FcChar8*>(s.data()),int(s.size()),&extent);return extent.xOff;});
        XftDrawStringUtf8(p.draw,&colors[color],font,x,y,reinterpret_cast<const FcChar8*>(value.data()),int(value.size()));
    }
    driver_demo::DashboardRect button(int i)const{
        const auto& p=panels[1];int width=(p.w-34)/2;
        return {12+(i%2)*(width+10),int(p.h*.42)+(i/2)*(int(p.h*.18)+10),width,int(p.h*.18)};
    }
public:
    ~InfoPanels(){close();}
    void set_layout(bool value){if(front_priority!=value){front_priority=value;down=-1;for(auto& p:panels)p.dirty=true;}}
    Window native_window(unsigned panel)const{return panels.at(panel).window;}
    uint64_t paint_count()const{return paints_;}
    driver_demo::DashboardRect button_rect(int i)const{return button(i);}
    void close(){
        if(!display)return;
        for(auto& p:panels){if(p.draw)XftDrawDestroy(p.draw);if(p.back)XFreePixmap(display,p.back);if(p.window)XDestroyWindow(display,p.window);p={};}
        if(gc)XFreeGC(display,gc);
        gc=0;
        if(normal)XftFontClose(display,normal);
        if(small)XftFontClose(display,small);
        normal=small=nullptr;
        for(unsigned i=0;i<allocated;++i)XftColorFree(display,DefaultVisual(display,DefaultScreen(display)),DefaultColormap(display,DefaultScreen(display)),&colors[i]);
        allocated=0;display=nullptr;
    }
    void start(Display* d,Window parent,Window video_window){
        display=d;owner=parent;video=video_window;gc=XCreateGC(d,parent,0,nullptr);
        for(const char* name:{cockpit::Palette[0],cockpit::Palette[8],cockpit::Palette[2],cockpit::Palette[3],cockpit::Palette[4],cockpit::Palette[5],cockpit::Palette[6],cockpit::Palette[7],cockpit::Palette[10]}){
            if(!XftColorAllocName(d,DefaultVisual(d,DefaultScreen(d)),DefaultColormap(d,DefaultScreen(d)),name,&colors[allocated]))throw std::runtime_error("Cannot allocate surround info color");
            ++allocated;}
        for(auto& p:panels){p.window=XCreateSimpleWindow(d,video,0,0,1,1,0,0,colors[0].pixel);
            XSelectInput(d,p.window,ExposureMask|ButtonPressMask|ButtonReleaseMask);}
    }
    char event(const XEvent& e){
        if(e.type==Expose)for(auto& p:panels)if(e.xexpose.window==p.window)p.dirty=true;
        if(e.type==FocusOut&&e.xfocus.detail!=NotifyInferior){down=-1;panels[1].dirty=true;}
        if(!visible||(e.type!=ButtonPress&&e.type!=ButtonRelease)||e.xbutton.window!=panels[1].window||e.xbutton.button!=Button1)return 0;
        int hit=-1;for(int i=0;i<4;++i)if(button(i).contains(e.xbutton.x,e.xbutton.y))hit=i;
        panels[1].dirty=true;
        if(e.type==ButtonPress){down=hit;XSetInputFocus(display,owner,RevertToParent,CurrentTime);return 0;}
        int before=down;down=-1;
        if(before>=0&&before==hit)return hit==0?'p':hit==1?'n':hit==2?'v':'k';
        return 0;
    }
    void paint(const signal_demo::MouseLayout& layout,const std::array<double,4>&,
               bool file,bool paused,const std::string& detail,unsigned front=0,bool enabled=true,const driver_demo::Explanation& evidence={}){
        bool show=enabled&&layout.vw>=760&&layout.vh>=420;
        if(show!=visible){visible=show;for(auto& p:panels){if(show)XMapRaised(display,p.window);else XUnmapWindow(display,p.window);p.dirty=true;}}
        if(!show)return;
        auto order=Layout{front}.channels();
        double scale=double(layout.vw)/1280;
        int size=std::clamp(int(16*scale),11,18);
        if(size!=font_size){
            if(normal)XftFontClose(display,normal);
            if(small)XftFontClose(display,small);
            auto load=[&](int px){auto pattern="WenQuanYi Micro Hei:pixelsize="+std::to_string(px);return XftFontOpenName(display,DefaultScreen(display),pattern.c_str());};
            normal=load(size);small=load(std::max(10,size-2));font_size=size;
            if(!normal||!small)throw std::runtime_error("Surround info requires a CJK font");
            for(auto& p:panels)p.dirty=true;
        }
        for(unsigned i=0;i<2;++i){auto& p=panels[i];int x=int((i?952:24)*scale),y=int(62*scale),w=int(304*scale),h=int(236*scale);
            if(front_priority){x=int(736*scale);y=int((i?236:12)*scale);w=int(520*scale);h=int((i?206:204)*scale);}
            if(p.x!=x||p.y!=y||p.w!=w||p.h!=h){p.x=x;p.y=y;p.w=w;p.h=h;down=-1;
                XMoveResizeWindow(display,p.window,x,y,unsigned(w),unsigned(h));
                if(p.draw)XftDrawDestroy(p.draw);
                if(p.back)XFreePixmap(display,p.back);
                p.back=XCreatePixmap(display,p.window,unsigned(w),unsigned(h),unsigned(DefaultDepth(display,DefaultScreen(display))));
                p.draw=XftDrawCreate(display,p.back,DefaultVisual(display,DefaultScreen(display)),DefaultColormap(display,DefaultScreen(display)));p.dirty=true;}
        }
        if(order!=last_order)panels[0].dirty=true;
        if(evidence.near_levels!=last_near||evidence.near_fresh!=last_fresh||evidence.near_approaching!=last_fast)panels[0].dirty=true;
        if(evidence.lane_requested!=last_lane_requested||evidence.lane_intent!=last_lane_intent)panels[0].dirty=true;
        last_lane_requested=evidence.lane_requested;last_lane_intent=evidence.lane_intent;
        if(detail!=last_detail){auto end=detail.find(" |"),begin=detail.rfind("SCENE ",0)==0?size_t(6):size_t(0);
            last_scene=detail.substr(begin,end==std::string::npos?end:end-begin);last_detail=detail;panels[1].dirty=true;}
        if(file!=last_file||paused!=last_paused)panels[1].dirty=true;
        last_order=order;last_file=file;last_paused=paused;
        last_near=evidence.near_levels;last_fresh=evidence.near_fresh;last_fast=evidence.near_approaching;
        if(!panels[0].dirty&&!panels[1].dirty)return;
        for(unsigned i=0;i<2;++i){auto& p=panels[i];if(!p.dirty)continue;p.dirty=false;++paints_;
            fill(p,0,0,p.w,p.h,0);fill(p,12,font_size+22,p.w-24,1,1);
            text(p,12,font_size+10,i?"画面与模式":"周边感知",2);
            if(!i){
                const char* names[]{"前视","左侧","右侧","后视"};
                for(unsigned role=0;role<4;++role){int y=int(p.h*(.29+role*.16));char value[64];
                    unsigned ch=order[role];int level=evidence.near_levels[ch];bool fresh=evidence.near_fresh[ch];
                    bool active=driver_demo::proximity_alert_active(evidence,ch);
                    if(active&&fresh&&(level||evidence.near_approaching[ch]))fill(p,6,y-font_size, p.w-12,font_size+7,level>=2?7:8);
                    const char* state=!fresh?"等待更新":!active&&(level||evidence.near_approaching[ch])?"近距观察":level>=2?"近距危险":evidence.near_approaching[ch]?"快速接近":level==1?"目标靠近":"保持观察";
                    std::snprintf(value,sizeof(value),"%s / CH%u",names[role],ch);text(p,12,y,value,3,true);
                    text(p,int(p.w*.40),y,state,active&&fresh&&(level||evidence.near_approaching[ch])?(level>=2?6:5):fresh?2:3,true);
                }
                text(p,12,p.h-10,"侧方按变道请求告警 · 图像近远非米数",3,true);
            }else {
                text(p,12,int(p.h*.26),file?(paused?"静帧检测":"本地素材回放"):"FPGA 实时输入",paused?5:4);
                text(p,12,int(p.h*.35),file?last_scene:"前视优先 · 四路同时接收",3,true);
                const char* labels[]{file?(paused?"继续播放":"暂停画面"):"暂停仅限素材","下一素材","前视实时","驻车网页"};
                for(int b=0;b<4;++b){auto r=button(b);fill(p,r.x,r.y,r.w,r.h,1);text(p,r.x+8,r.y+r.h/2+font_size/2-2,labels[b],b==0&&!file?3:b==2?4:2,true);
                    if(down==b){XSetForeground(display,gc,colors[4].pixel);XDrawRectangle(display,p.back,gc,r.x,r.y,unsigned(r.w-1),unsigned(r.h-1));}}
                text(p,12,p.h-10,"控制请求仍受当前感知条件约束",3,true);
            }
            XCopyArea(display,p.back,p.window,gc,0,0,unsigned(p.w),unsigned(p.h),0,0);
        }
        XFlush(display);
    }
};
}
