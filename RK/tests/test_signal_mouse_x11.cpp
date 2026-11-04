// Native X11/Xft widgets under ARM/QEMU + Xvfb, not physical RK/NPU tests.
#include "native_toolbar.hpp"
#include "signal_state.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>
int main(int argc,char**argv){
    using namespace signal_demo;
    Display* display=XOpenDisplay(nullptr);if(!display){std::cerr<<"No test DISPLAY\n";return 1;}
    Window parent=XCreateSimpleWindow(display,DefaultRootWindow(display),0,0,1280,804,0,0,0);
    XSelectInput(display,parent,ExposureMask|StructureNotifyMask|ButtonPressMask|ButtonReleaseMask);
    MouseToolbar toolbar;toolbar.start(display,parent,1280,804);XMapWindow(display,parent);XSync(display,False);
    std::vector<char> commands;
    auto pump=[&](){XSync(display,False);while(XPending(display)){XEvent e;XNextEvent(display,&e);char c=toolbar.event(e);if(c)commands.push_back(c);}toolbar.draw(1,true,"AUTO ROI  STRAIGHT  RED  RED_WAIT","AGE 72MS  UNIQUE VISIBLE ARROW - EGO LANE UNVERIFIED");XSync(display,False);};
    auto mouse=[&](int type,Window window,int x,int y){XEvent e{};e.xbutton.type=type;e.xbutton.display=display;e.xbutton.window=window;
        e.xbutton.root=DefaultRootWindow(display);e.xbutton.same_screen=True;e.xbutton.button=Button1;e.xbutton.x=x;e.xbutton.y=y;
        assert(XSendEvent(display,window,False,type==ButtonPress?ButtonPressMask:ButtonReleaseMask,&e));pump();};
    pump();
    for(const auto& size:{std::array<int,2>{1280,804},{800,534},{1000,900}}){
        XResizeWindow(display,parent,size[0],size[1]);pump();
        for(int i=0;i<int(MouseLayout::keys.size());++i){int x=(toolbar.layout.left(i)+toolbar.layout.right(i))/2,y=toolbar.layout.height-60;
            size_t before=commands.size();mouse(ButtonPress,parent,x,y);assert(commands.size()==before);
            XImage* pressed_image=XGetImage(display,parent,toolbar.layout.left(i)+1,toolbar.layout.height-MouseLayout::bar_height+9,1,1,AllPlanes,ZPixmap);assert(pressed_image);
            assert(XGetPixel(pressed_image,0,0)==WhitePixel(display,DefaultScreen(display)));XDestroyImage(pressed_image);
            mouse(ButtonRelease,parent,x,y);assert(commands.size()==before+1&&commands.back()==MouseLayout::keys[i]);
        }
        size_t before=commands.size();
        mouse(ButtonPress,parent,toolbar.layout.left(3)+8,toolbar.layout.height-60);
        mouse(ButtonRelease,parent,toolbar.layout.left(4)+8,toolbar.layout.height-60);assert(commands.size()==before);
        mouse(ButtonPress,toolbar.video,120,120);mouse(ButtonRelease,toolbar.video,150,150);assert(commands.size()==before);
        mouse(ButtonPress,parent,toolbar.layout.left(3)+8,toolbar.layout.height-60);toolbar.cancel();
        mouse(ButtonRelease,parent,toolbar.layout.left(3)+8,toolbar.layout.height-60);assert(commands.size()==before);
        XWindowAttributes a{};XGetWindowAttributes(display,toolbar.video,&a);
        assert(a.width==toolbar.layout.vw&&a.height==toolbar.layout.vh);
    }
    if(argc==2){
        XResizeWindow(display,parent,1280,804);pump();
        XImage* image=XGetImage(display,parent,0,720,1280,84,AllPlanes,ZPixmap);assert(image);
        std::ofstream out(argv[1],std::ios::binary);out<<"P6\n1280 84\n255\n";
        auto component=[](unsigned long p,unsigned long mask){if(!mask)return 0ul;while(!(mask&1)){mask>>=1;p>>=1;}return (p&mask)*255/mask;};
        for(int y=0;y<84;++y)for(int x=0;x<1280;++x){auto p=XGetPixel(image,x,y);char rgb[]{char(component(p,image->red_mask)),char(component(p,image->green_mask)),char(component(p,image->blue_mask))};out.write(rgb,3);}
        XDestroyImage(image);
    }
    auto paints=toolbar.paint_count();for(int i=0;i<1000;++i)toolbar.draw(1,true,"AUTO ROI  STRAIGHT  RED  RED_WAIT","AGE 72MS  UNIQUE VISIBLE ARROW - EGO LANE UNVERIFIED");
    assert(toolbar.paint_count()==paints);
    toolbar.close();XDestroyWindow(display,parent);XCloseDisplay(display);
    std::cout<<"PASS native X11: 36 button actions at 3 sizes; pressed border pixels, automatic status/RECHECK/SCENE, release-only, drag-off, video ROI separation and cancellation. No RK/GStreamer/NPU validation.\n";
}
