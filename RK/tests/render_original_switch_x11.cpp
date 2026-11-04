// X11 visual fixture using recorded NV12 frames. No NPU or FPGA invocation.
#include "top_dashboard.hpp"
#include "native_toolbar.hpp"
#include "surround_view.hpp"
#include "surround_info.hpp"
#include "x11_preview.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstdlib>

int main(int argc,char**argv){
    if(argc!=3)return 2;
    using namespace driver_demo;
    constexpr int W=1280,H=720;std::vector<uint8_t> source(W*H*3/2),output(source.size());
    std::ifstream input(argv[1],std::ios::binary);input.read(reinterpret_cast<char*>(source.data()),source.size());
    if(input.gcount()!=std::streamsize(source.size()))return 3;
    surround_ui::initialize(output.data());
    cv::Mat sy(H,W,CV_8UC1,source.data()),dy(H,W,CV_8UC1,output.data());
    cv::Mat suv(H/2,W/2,CV_8UC2,source.data()+W*H),duv(H/2,W/2,CV_8UC2,output.data()+W*H);
    Display* display=XOpenDisplay(nullptr);if(!display)return 4;
    Window parent=XCreateSimpleWindow(display,DefaultRootWindow(display),0,0,1600,1000,0,0,0);
    XSelectInput(display,parent,ExposureMask|StructureNotifyMask|ButtonPressMask|ButtonReleaseMask);
    signal_demo::MouseToolbar toolbar;toolbar.start(display,parent,1600,1000);
    TopDashboard dashboard;dashboard.start(display,parent);XMapWindow(display,parent);XSync(display,False);
    surround_ui::InfoPanels context;context.start(display,parent,toolbar.video);
    {
        ui::X11Preview video(display,toolbar.video);Explanation e;
        e.action="界面预览 / PREVIEW";e.reason="录像画面 · 状态卡尚未接入本帧推理";
        e.scope="仅验证布局、文字和按钮";e.severity=0;e.target="四视图素材：real_0796";
        e.signal={"当前未见灯","当前未见灯","当前未见灯"};
        e.signal_detail={"界面样例，不代表本帧推理","界面样例，不代表本帧推理","界面样例，不代表本帧推理"};
        e.lane_detail="等待实体板测";e.lane_pixels="车道绘制与规则沿用RC2";
        if(std::getenv("RK_VEHICLE_FIXTURE")){e.vehicle_state="MOVING";e.vehicle_label="模拟前进";e.vehicle_reason="箭头布局示例，非本帧推理";e.vehicle_arrow=1;e.vehicle_moving=true;}
        if(std::getenv("RK_ALERT_FIXTURE")){e.severity=3;e.action="侧后危险 / WARN";e.reason="示例：左侧及后方很近 · 禁止变道";
            e.target="界面状态示例，不代表本帧推理";e.near_levels={0,1,2,2};e.near_fresh={true,true,true,true};e.near_approaching[3]=true;}
        for(const auto& size:{std::array<int,2>{1024,768},{1280,804},{1600,1020}})
#ifdef ORIGINAL_UI
        for(int mode=0;mode<1;++mode){
#else
        for(int mode=0;mode<2;++mode){dashboard.set_layout(bool(mode));context.set_layout(bool(mode));
#endif
            XResizeWindow(display,parent,size[0],size[1]);XSync(display,False);
            while(XPending(display)){XEvent event;XNextEvent(display,&event);toolbar.event(event);dashboard.event(event);context.event(event);}
#ifdef ORIGINAL_UI
            surround_ui::initialize(output.data());auto tiles=surround_ui::Layout::tiles;
#else
            surround_ui::initialize(output.data(),0,bool(mode));auto tiles=surround_ui::Layout{0,bool(mode)}.destinations();
#endif
            for(unsigned role=0;role<4;++role){surround_ui::Rect src{int(role%2)*640+32,int(role/2)*360+32,576,324};auto dest=tiles[role];
                cv::resize(sy(cv::Rect(src.x,src.y,src.w,src.h)),dy(cv::Rect(dest.x,dest.y,dest.w,dest.h)),{dest.w,dest.h});
                cv::resize(suv(cv::Rect(src.x/2,src.y/2,src.w/2,src.h/2)),duv(cv::Rect(dest.x/2,dest.y/2,dest.w/2,dest.h/2)),{dest.w/2,dest.h/2});}
            dashboard.paint(toolbar.layout,e,"A YOLOv8n+HSV · 布局预览",{0,0,0,0},0,0,0);
            toolbar.draw(1,true,"LAYOUT PREVIEW | RECORDED FOUR VIEWS | NO INFERENCE RUN", "NEXT changes clip | LIVE restores FPGA front | PAUSE freezes video only");
            context.paint(toolbar.layout,{0,0,0,0},true,false,"SCENE real_0796 | NEXT",0,true,e);XSync(display,False);
            // Move the child panels before presenting: repaint exposed old
            // positions just as the next real video frame does in Preview.
            video.draw(output.data(),W,H,toolbar.layout.vw,toolbar.layout.vh);XSync(display,False);
            if(toolbar.layout.vw>=760&&toolbar.layout.vh>=420){
                for(int b=0;b<4;++b){auto r=context.button_rect(b);XEvent event{};event.xbutton.type=ButtonPress;
                    event.xbutton.window=context.native_window(1);event.xbutton.button=Button1;event.xbutton.x=r.x+r.w/2;event.xbutton.y=r.y+r.h/2;
                    assert(!context.event(event));event.xbutton.type=ButtonRelease;assert(context.event(event)==(b==0?'p':b==1?'n':b==2?'v':'k'));}
                auto left=context.button_rect(0),right=context.button_rect(1);XEvent event{};event.xbutton.type=ButtonPress;event.xbutton.window=context.native_window(1);event.xbutton.button=Button1;event.xbutton.x=left.x+2;event.xbutton.y=left.y+2;
                context.event(event);event.xbutton.type=ButtonRelease;event.xbutton.x=right.x+2;event.xbutton.y=right.y+2;assert(!context.event(event));
                context.paint(toolbar.layout,{0,0,0,0},true,false,"SCENE real_0796 | NEXT",0,true,e);XSync(display,False);
            }
#ifndef ORIGINAL_UI
            XEvent choice{};choice.xbutton.type=ButtonPress;choice.xbutton.window=dashboard.native_window();choice.xbutton.button=Button1;
            choice.xbutton.x=size[0]-300;choice.xbutton.y=15;assert(!dashboard.event(choice));choice.xbutton.type=ButtonRelease;assert(dashboard.event(choice)=='~');
            choice.xbutton.type=ButtonPress;dashboard.event(choice);choice.xbutton.type=ButtonRelease;choice.xbutton.x=0;assert(!dashboard.event(choice));
            dashboard.paint(toolbar.layout,e,"A YOLOv8n+HSV · 布局预览",{0,0,0,0},0,0,0);
#endif
            auto image=XGetImage(display,parent,0,0,unsigned(size[0]),unsigned(size[1]),AllPlanes,ZPixmap);assert(image);
            std::string path=std::string(argv[2])+"_"+std::to_string(mode)+"_"+std::to_string(size[0])+"x"+std::to_string(size[1])+".ppm";
            std::ofstream out(path,std::ios::binary);out<<"P6\n"<<size[0]<<' '<<size[1]<<"\n255\n";
            auto component=[](unsigned long value,unsigned long mask){if(!mask)return 0ul;while(!(mask&1)){mask>>=1;value>>=1;}return (value&mask)*255/mask;};
            for(int y=0;y<size[1];++y)for(int x=0;x<size[0];++x){auto p=XGetPixel(image,x,y);
                char rgb[]{char(component(p,image->red_mask)),char(component(p,image->green_mask)),char(component(p,image->blue_mask))};out.write(rgb,3);}
            XDestroyImage(image);
        }
    }
    context.close();dashboard.close();toolbar.close();XDestroyWindow(display,parent);XCloseDisplay(display);
    std::cout<<"PASS original-wording switch fixture: three sizes, both layouts, release-only switch, drag cancellation, recorded road views, cached generated ego car, native dashboard/toolbar, corner status/four release-only shortcuts and software NV12 presentation. Not physical performance.\n";
}
