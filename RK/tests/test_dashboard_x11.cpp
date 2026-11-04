// Actual Xft rendering and click test. No receiver, NPU or simulated perception pass.
#include "top_dashboard.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>
int main(int argc,char**argv){
    using namespace driver_demo;
    Display* display=XOpenDisplay(nullptr);if(!display)return 2;
    Window parent=XCreateSimpleWindow(display,DefaultRootWindow(display),0,0,1600,1030,0,0,0);
    XSelectInput(display,parent,StructureNotifyMask|ExposureMask);XMapWindow(display,parent);
    TopDashboard panel;panel.start(display,parent);
    Explanation e;e.signal={"信号允许","信号禁止","当前未见灯"};e.signal_light={"GREEN","RED","UNKNOWN"};e.severity=3;e.action="停止 / STOP";
    e.reason="当前意图方向信号禁止";e.lane="当前偏左";e.lane_position_known=true;e.lane_offset=-.3;
    e.lane_styles={1,2};e.target="道路目标 ID22 · 高度占画面 41%";
    std::vector<char> commands;
    auto pump=[&](){XSync(display,False);while(XPending(display)){XEvent event;XNextEvent(display,&event);char command=panel.event(event);if(command)commands.push_back(command);}};
    for(const auto& size:{std::array<int,2>{640,444},{800,534},{1280,804},{1600,1030}}){
        XResizeWindow(display,parent,size[0],size[1]);signal_demo::MouseLayout mouse;mouse.resize(size[0],size[1]);
        panel.paint(mouse,e,"A YOLOv8+HSV",{12.4,3.1,3.1,3.1},2.3,68,30);pump();
        XWindowAttributes actual{};assert(XGetWindowAttributes(display,panel.native_window(),&actual));
        assert(actual.y+actual.height<=mouse.vy&&actual.width==mouse.pw);
        for(int side=0;side<2;++side){auto button=DashboardLayout{mouse.pw,mouse.ph}.lane_button(side);size_t before=commands.size();
            for(int type:{ButtonPress,ButtonRelease}){XEvent event{};event.xbutton.type=type;event.xbutton.display=display;event.xbutton.window=panel.native_window();
                event.xbutton.button=Button1;event.xbutton.x=button.x+button.w/2;event.xbutton.y=button.y+button.h/2;
                assert(XSendEvent(display,panel.native_window(),False,type==ButtonPress?ButtonPressMask:ButtonReleaseMask,&event));pump();
                if(type==ButtonPress)assert(commands.size()==before);}
            assert(commands.size()==before+1&&commands.back()==(side==0?'j':'m'));
        }
        // Pressing left then releasing right must not create a request.
        {
            XEvent e360{};e360.xbutton.window=panel.native_window();e360.xbutton.button=Button1;
            e360.xbutton.x=mouse.pw-70;e360.xbutton.y=15;
            e360.xbutton.type=ButtonPress;assert(!panel.event(e360));
            e360.xbutton.type=ButtonRelease;assert(panel.event(e360)=='u');
            e360.xbutton.type=ButtonPress;assert(!panel.event(e360));
            e360.xbutton.type=ButtonRelease;e360.xbutton.y=40;assert(!panel.event(e360));
        }
        {
            panel.paint(mouse,e,"A YOLOv8+HSV",{12.4,3.1,3.1,3.1},2.3,68,30,1);pump();
            XEvent sound{};sound.xbutton.window=panel.native_window();sound.xbutton.button=Button1;
            sound.xbutton.x=mouse.pw-195;sound.xbutton.y=15;
            sound.xbutton.type=ButtonPress;assert(!panel.event(sound));
            sound.xbutton.type=ButtonRelease;assert(panel.event(sound)=='!');
            sound.xbutton.type=ButtonPress;assert(!panel.event(sound));
            sound.xbutton.type=ButtonRelease;sound.xbutton.y=40;assert(!panel.event(sound));
        }
        auto left=DashboardLayout{mouse.pw,mouse.ph}.lane_button(0),right=DashboardLayout{mouse.pw,mouse.ph}.lane_button(1);
        XEvent event{};event.xbutton.type=ButtonPress;event.xbutton.window=panel.native_window();event.xbutton.button=Button1;event.xbutton.x=left.x+2;event.xbutton.y=left.y+2;
        panel.event(event);event.xbutton.type=ButtonRelease;event.xbutton.x=right.x+2;assert(!panel.event(event));
    }
    {
        signal_demo::MouseLayout mouse;mouse.resize(1600,1030);
        panel.paint(mouse,e,"A YOLOv8+HSV",{12.4,3.1,3.1,3.1},2.3,68,30);pump();
        panel.paint(mouse,e,"A YOLOv8+HSV",{12.4,3.1,3.1,3.1},2.3,68,30);pump();
        auto before=panel.card_paints();
        panel.paint(mouse,e,"A YOLOv8+HSV",{12.4,3.1,3.1,3.1},2.3,68,30);assert(panel.card_paints()==before);
        e.signal[0]="信号禁止";e.signal_light[0]="RED";panel.paint(mouse,e,"A YOLOv8+HSV",{12.4,3.1,3.1,3.1},2.3,68,30);
        assert(panel.card_paints()==before+1); // New STOP is immediate and touches just its own card.
        e.lane_styles[0]=2;panel.paint(mouse,e,"A YOLOv8+HSV",{12.4,3.1,3.1,3.1},2.3,68,30);
        assert(panel.card_paints()==before+2);
        e.signal_light[0]="YELLOW";panel.paint(mouse,e,"A YOLOv8+HSV",{12.4,3.1,3.1,3.1},2.3,68,30);
        assert(panel.card_paints()==before+3); // RED -> YELLOW repaints even while STOP is unchanged.
        e.signal[0]="信号允许";e.signal_light[0]="GREEN";
    }
    if(argc==2){signal_demo::MouseLayout mouse;mouse.resize(1600,1030);panel.paint(mouse,e,"A YOLOv8+HSV",{12.4,3.1,3.1,3.1},2.3,68,30);XSync(display,False);
        auto image=XGetImage(display,panel.native_window(),0,0,mouse.pw,mouse.ph,AllPlanes,ZPixmap);assert(image);
        std::ofstream out(argv[1],std::ios::binary);out<<"P6\n"<<mouse.pw<<' '<<mouse.ph<<"\n255\n";
        auto component=[](unsigned long value,unsigned long mask){if(!mask)return 0ul;while(!(mask&1)){mask>>=1;value>>=1;}return (value&mask)*255/mask;};
        for(int y=0;y<mouse.ph;++y)for(int x=0;x<mouse.pw;++x){auto value=XGetPixel(image,x,y);char rgb[]{char(component(value,image->red_mask)),char(component(value,image->green_mask)),char(component(value,image->blue_mask))};out.write(rgb,3);}
        XDestroyImage(image);
    }
    panel.close();XDestroyWindow(display,parent);XCloseDisplay(display);
    std::cout<<"PASS Xft dashboard: Chinese font, four sizes, panel/video separation, eight release-only lane clicks, drag-off cancellation, unchanged/partial-card redraw and immediate restrictions. Fixture states only; no accuracy or NPU validation.\n";
}
