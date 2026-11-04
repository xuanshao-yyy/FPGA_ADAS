// Real X11 regression: moving native panels must expose the RK video color key.
#include "surround_info.hpp"
#include "native_toolbar.hpp"
#include <X11/Xutil.h>
#include <cassert>
#include <iostream>
int main(){
    Display* d=XOpenDisplay(nullptr);assert(d);
    Window owner=XCreateSimpleWindow(d,DefaultRootWindow(d),0,0,1600,1020,0,0,0);
    XMapWindow(d,owner);
    signal_demo::MouseToolbar toolbar;toolbar.start(d,owner,1600,1020);
    surround_ui::prepare_rk_video_background(d,toolbar.video);
    surround_ui::InfoPanels panels;panels.start(d,owner,toolbar.video);
    auto pixel=[&](int x,int y){XSync(d,False);auto image=XGetImage(d,toolbar.video,x,y,1,1,AllPlanes,ZPixmap);assert(image);auto value=XGetPixel(image,0,0);XDestroyImage(image);return value;};
    driver_demo::Explanation evidence;
    for(int cycle=0;cycle<3;++cycle){
        panels.set_layout(false);panels.paint(toolbar.layout,{},true,false,"SCENE lanes",0,true,evidence);
        assert(pixel(800,80)==0x010203ul); // Old upper-right front-layout panel footprint.
        assert(pixel(800,260)==0x010203ul); // Old lower-right front-layout panel footprint.
        assert(pixel(40,80)!=0x010203ul); // Current left panel remains opaque/readable.
        panels.set_layout(true);panels.paint(toolbar.layout,{},true,false,"SCENE lanes",0,true,evidence);
        assert(pixel(50,80)==0x010203ul); // Original left panel moved away over front video.
        assert(pixel(1200,225)==0x010203ul); // Original right panel moved away into gap.
        assert(pixel(800,80)!=0x010203ul); // Current front-layout panel remains readable.
        auto paints=panels.paint_count();
        for(int frame=0;frame<60;++frame)panels.paint(toolbar.layout,{},true,false,"SCENE lanes",0,true,evidence);
        assert(panels.paint_count()==paints); // No per-frame repaint added.
    }
    panels.paint(toolbar.layout,{},true,false,"SCENE lanes",0,false,evidence);
    assert(pixel(800,80)==0x010203ul); // Hiding also restores key, not black.
    panels.close();toolbar.close();XDestroyWindow(d,owner);XCloseDisplay(d);
    std::cout<<"PASS real X11 key background: 3 round trips, exposed footprints, live panels, hide and no steady-frame repaint. No physical NV12 scanout claim.\n";
}
