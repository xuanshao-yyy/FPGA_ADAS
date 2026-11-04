#pragma once
#include "brand_logo_asset.hpp"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cstdlib>
#include <stdexcept>

namespace brand {
// Upload once to an X pixmap. Header repaints only copy 108x30 pixels.
class Logo {
    Display* display=nullptr;Pixmap pixmap=0;
    static unsigned long component(unsigned value,unsigned long mask){
        unsigned shift=0;if(!mask)return 0;
        while(!(mask&1)){mask>>=1;++shift;}
        return ((value*mask+127)/255)<<shift;
    }
public:
    ~Logo(){close();}
    void close(){if(pixmap)XFreePixmap(display,pixmap);pixmap=0;display=nullptr;}
    void start(Display* d,Drawable target){
        close();display=d;int screen=DefaultScreen(d);
        auto image=XCreateImage(d,DefaultVisual(d,screen),unsigned(DefaultDepth(d,screen)),ZPixmap,0,nullptr,Width,Height,32,0);
        if(!image)throw std::runtime_error("Cannot create brand image");
        image->data=static_cast<char*>(std::calloc(size_t(image->bytes_per_line),Height));
        if(!image->data){XDestroyImage(image);throw std::runtime_error("Cannot allocate brand image");}
        for(int y=0;y<Height;++y)for(int x=0;x<Width;++x){auto p=RGB+(y*Width+x)*3;
            XPutPixel(image,x,y,component(p[0],image->red_mask)|component(p[1],image->green_mask)|component(p[2],image->blue_mask));}
        pixmap=XCreatePixmap(d,target,Width,Height,unsigned(DefaultDepth(d,screen)));
        auto gc=XCreateGC(d,pixmap,0,nullptr);XPutImage(d,pixmap,gc,image,0,0,0,0,Width,Height);XFreeGC(d,gc);XDestroyImage(image);
    }
    void paint(Drawable target,GC gc,int x,int y)const{if(pixmap)XCopyArea(display,pixmap,target,gc,0,0,Width,Height,x,y);}
};
}
