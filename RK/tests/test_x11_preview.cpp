#include "x11_preview.hpp"
#include <cassert>
#include <vector>
#include <iostream>
int main(){
    Display* display=XOpenDisplay(nullptr);assert(display);
    Window window=XCreateSimpleWindow(display,DefaultRootWindow(display),20,20,1280,720,0,0,0);XMapRaised(display,window);XSync(display,False);
    const int w=1280,h=720;std::vector<uint8_t> nv12(w*h*3/2);
    int yy[]={81,145,41,235},uu[]={90,54,240,128},vv[]={240,34,110,128};
    for(bool enable_shared:{false,true}){
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){int q=(y>=h/2)*2+(x>=w/2);nv12[y*w+x]=uint8_t(yy[q]);}
    for(int y=0;y<h/2;++y)for(int x=0;x<w;x+=2){int q=(y>=h/4)*2+(x>=w/2);nv12[w*h+y*w+x]=uint8_t(uu[q]);nv12[w*h+y*w+x+1]=uint8_t(vv[q]);}
        ui::X11Preview preview(display,window,enable_shared);
        for(auto size:{std::pair<int,int>{640,360},{800,450},{320,180}}){
            preview.draw(nv12.data(),w,h,size.first,size.second);XSync(display,False);
            XImage* image=XGetImage(display,window,0,0,unsigned(size.first),unsigned(size.second),AllPlanes,ZPixmap);assert(image);
            for(int q=0;q<4;++q){auto pixel=XGetPixel(image,(q%2?3:1)*size.first/4,(q/2?3:1)*size.second/4);
                unsigned r=(pixel&image->red_mask)>>16,g=(pixel&image->green_mask)>>8,b=pixel&image->blue_mask;
                if(q==0)assert(r>240&&g<15&&b<15);if(q==1)assert(g>240&&r<15&&b<15);if(q==2)assert(b>240&&r<15&&g<15);if(q==3)assert(r>240&&g>240&&b>240);
            }XDestroyImage(image);
        }
        uint32_t seed=31;for(auto& byte:nv12){seed=seed*1664525u+1013904223u;byte=uint8_t(seed>>24);}
        for(auto size:{std::pair<int,int>{640,360},{800,450},{637,359},{320,180},{1280,720}}){
            preview.draw(nv12.data(),w,h,size.first,size.second);XSync(display,False);
            XImage* image=XGetImage(display,window,0,0,unsigned(size.first),unsigned(size.second),AllPlanes,ZPixmap);assert(image);
            for(int y=0;y<size.second;++y)for(int x=0;x<size.first;++x){
                int sx=x*w/size.first,sy=y*h/size.second,uv=w*h+(sy/2)*w+(sx&~1);
                int c=std::max(0,int(nv12[sy*w+sx])-16),d=int(nv12[uv])-128,e=int(nv12[uv+1])-128;
                auto clip=[](int v){return unsigned(std::clamp(v,0,255));};
                unsigned expected=(clip((298*c+409*e+128)>>8)<<16)|(clip((298*c-100*d-208*e+128)>>8)<<8)|clip((298*c+516*d+128)>>8);
                assert((XGetPixel(image,x,y)&0xffffff)==expected);
            }XDestroyImage(image);
        }
        bool rejected=false;try{preview.draw(nv12.data(),1279,h,640,360);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    }
    XDestroyWindow(display,window);XSync(display,False);XCloseDisplay(display);
    std::cout<<"PASS native X11 NV12 BT.601 red/green/blue/white pixels at 3 sizes, resize/lifetime and invalid layout; every random pixel bit-exact at 5 sizes including ARM NEON width, both copy and MIT-SHM; no GStreamer/RGA/NPU or physical-RK claim\n";
}
