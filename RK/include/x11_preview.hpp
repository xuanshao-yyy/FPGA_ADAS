#pragma once
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <array>
#include <vector>
#include <chrono>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
namespace ui {
// Diagnostic software presentation. No DRM/KMS, video sink or DMA-BUF export.
// NV12 source pixels and all inference tensors remain unchanged.
class X11Preview {
    Display* display;Window window;GC gc;XImage* image=nullptr;
    XShmSegmentInfo shm{};bool shared=false,allow_shared=true,pending=false;
    inline static bool attach_error=false;
    static int handle_attach_error(Display*,XErrorEvent*){attach_error=true;return 0;}
    void release(){
        if(shared){XShmDetach(display,&shm);XSync(display,False);shmdt(shm.shmaddr);image->data=nullptr;shared=false;pending=false;}
        if(image){XDestroyImage(image);image=nullptr;}
    }
    bool create_shared(int width,int height){
        if(!allow_shared||!XShmQueryExtension(display))return false;
        int screen=DefaultScreen(display);
        image=XShmCreateImage(display,DefaultVisual(display,screen),unsigned(DefaultDepth(display,screen)),ZPixmap,nullptr,&shm,unsigned(width),unsigned(height));
        if(!image)return false;
        shm.shmid=shmget(IPC_PRIVATE,size_t(image->bytes_per_line)*height,IPC_CREAT|0600);
        if(shm.shmid<0){release();return false;}
        shm.shmaddr=static_cast<char*>(shmat(shm.shmid,nullptr,0));
        if(shm.shmaddr==reinterpret_cast<char*>(-1)){shmctl(shm.shmid,IPC_RMID,nullptr);release();return false;}
        image->data=shm.shmaddr;shm.readOnly=True;
        XSync(display,False);attach_error=false;auto previous=XSetErrorHandler(handle_attach_error);
        bool attached=XShmAttach(display,&shm);XSync(display,False);XSetErrorHandler(previous);
        if(!attached||attach_error){shmdt(shm.shmaddr);shmctl(shm.shmid,IPC_RMID,nullptr);image->data=nullptr;release();return false;}
        shared=true;shmctl(shm.shmid,IPC_RMID,nullptr); // Kernel removes after both attachments close.
        return true;
    }
    std::vector<int> xs,ys;
    int source_w=0,source_h=0;
    std::array<int,256> luminance{},red_v{},blue_u{},green_u{},green_v{};
    static int clip(int x){return std::clamp(x,0,255);}
    static unsigned long component(unsigned value,unsigned long mask){
        if(!mask)return 0;
        unsigned shift=0;while(!(mask&(1ul<<shift)))++shift;
        auto maximum=mask>>shift;return ((value*maximum+127)/255)<<shift;
    }
public:
    X11Preview(Display* d,Window w,bool enable_shared=true):display(d),window(w),gc(XCreateGC(d,w,0,nullptr)),allow_shared(enable_shared){
        for(int i=0;i<256;++i){luminance[i]=298*std::max(0,i-16);red_v[i]=409*(i-128);blue_u[i]=516*(i-128);green_u[i]=-100*(i-128);green_v[i]=-208*(i-128);}
    }
    ~X11Preview(){release();if(gc)XFreeGC(display,gc);}
    X11Preview(const X11Preview&)=delete;X11Preview& operator=(const X11Preview&)=delete;
    bool shared_memory()const{return shared;}
    double conversion_ms=0,submission_ms=0;
    void draw(const uint8_t* nv12,int sw,int sh,int width,int height){
        if(!nv12||sw<=0||sh<=0||(sw&1)||(sh&1)||width<=0||height<=0)throw std::runtime_error("Invalid software NV12 view");
        if(!image||image->width!=width||image->height!=height){
            release();
            if(!create_shared(width,height)){
            int screen=DefaultScreen(display);
            image=XCreateImage(display,DefaultVisual(display,screen),unsigned(DefaultDepth(display,screen)),ZPixmap,0,nullptr,unsigned(width),unsigned(height),32,0);
            if(!image)throw std::runtime_error("XCreateImage failed");
            image->data=static_cast<char*>(std::calloc(size_t(image->bytes_per_line),size_t(height)));
            if(!image->data)throw std::runtime_error("Cannot allocate X11 preview image");
            }
        }
        auto wait_begin=std::chrono::steady_clock::now();
        // Synchronize only before overwriting shared pixels. The server can consume
        // the last image during main-loop control, receive and composition work.
        if(shared&&pending){XSync(display,False);pending=false;}
        auto begin=std::chrono::steady_clock::now();
        if(source_w!=sw||source_h!=sh||xs.size()!=size_t(width)||ys.size()!=size_t(height)){
            source_w=sw;source_h=sh;xs.resize(width);ys.resize(height);
            for(int x=0;x<width;++x)xs[x]=x*sw/width;
            for(int y=0;y<height;++y)ys[y]=y*sh/height;
        }
        bool direct=image->bits_per_pixel==32&&image->byte_order==LSBFirst&&image->red_mask==0xff0000&&image->green_mask==0xff00&&image->blue_mask==0xff;
        for(int y=0;y<height;++y){int sy=ys[y];auto row=reinterpret_cast<uint32_t*>(image->data+y*image->bytes_per_line);
            const auto luma=nv12+sy*sw,chroma=nv12+sw*sh+(sy/2)*sw;
            int start_x=0;
#if defined(__aarch64__)
            // Eight pixels per NEON group; same BT.601 integer coefficients/rounding as scalar.
            if(direct&&width==sw){
                auto pack=[](int16x8_t c,int16x8_t d,int16x8_t e,int dc,int ec){
                    auto half=[&](int16x4_t cc,int16x4_t dd,int16x4_t ee){
                        auto v=vmlal_n_s16(vmull_n_s16(cc,298),dd,int16_t(dc));v=vmlal_n_s16(v,ee,int16_t(ec));
                        return vqshrun_n_s32(vaddq_s32(v,vdupq_n_s32(128)),8);
                    };
                    return vqmovn_u16(vcombine_u16(half(vget_low_s16(c),vget_low_s16(d),vget_low_s16(e)),half(vget_high_s16(c),vget_high_s16(d),vget_high_s16(e))));
                };
                for(;start_x+8<=width;start_x+=8){
                    auto yy=vld1_u8(luma+start_x),uv=vld1_u8(chroma+start_x);auto separate=vuzp_u8(uv,uv);
                    auto uu=vzip_u8(separate.val[0],separate.val[0]).val[0],vv=vzip_u8(separate.val[1],separate.val[1]).val[0];
                    auto c=vreinterpretq_s16_u16(vsubl_u8(vmax_u8(yy,vdup_n_u8(16)),vdup_n_u8(16)));
                    auto d=vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(uu)),vdupq_n_s16(128)),e=vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vv)),vdupq_n_s16(128));
                    uint8x8x4_t bgra{{pack(c,d,e,516,0),pack(c,d,e,-100,-208),pack(c,d,e,0,409),vdup_n_u8(0)}};
                    vst4_u8(reinterpret_cast<uint8_t*>(row+start_x),bgra);
                }
            }
#endif
            for(int x=start_x;x<width;++x){int sx=xs[x],c=luminance[luma[sx]],u=chroma[sx&~1],v=chroma[(sx&~1)+1];
                unsigned r=unsigned(clip((c+red_v[v]+128)>>8)),g=unsigned(clip((c+green_u[u]+green_v[v]+128)>>8)),b=unsigned(clip((c+blue_u[u]+128)>>8));
                if(direct)row[x]=(r<<16)|(g<<8)|b;
                else XPutPixel(image,x,y,component(r,image->red_mask)|component(g,image->green_mask)|component(b,image->blue_mask));
            }
        }
        auto converted=std::chrono::steady_clock::now();
        conversion_ms=std::chrono::duration<double,std::milli>(converted-begin).count();
        if(shared){XShmPutImage(display,window,gc,image,0,0,0,0,unsigned(width),unsigned(height),False);XFlush(display);pending=true;}
        else {XPutImage(display,window,gc,image,0,0,0,0,unsigned(width),unsigned(height));XFlush(display);}
        submission_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-converted+begin-wait_begin).count();
    }
};
}
