// Static handoff/parking page. No receiver, codec, inference, screenshots or animation.
#include "brand_logo.hpp"
#include <X11/Xft/Xft.h>
#include <X11/Xatom.h>
#include <opencv2/core.hpp>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace {
std::atomic<bool> stopped{false};
void interrupt(int){stopped=true;}
struct State {
    std::string mode="STARTING",target="DRIVE",phase="正在准备驾驶界面",error;
    bool operator==(const State& b)const{return mode==b.mode&&target==b.target&&phase==b.phase&&error==b.error;}
    bool interactive()const{return mode=="PARK"||mode=="FAULT";}
};
const char* title(const std::string& m){return m=="PARK"?"驻车守护":m=="PANORAMA"?"360 环视":"驾驶辅助";}
struct Page {
    Display* d=nullptr;Window window=0;Atom wm_close=0;GC gc=0;Pixmap back=0;XftDraw* draw=nullptr;brand::Logo logo;
    XftFont *small=nullptr,*normal=nullptr,*large=nullptr;XftColor colors[5]{};int allocated=0;
    int width=1280,height=800,down=-1;bool dirty=true;State state;
    ~Page(){logo.close();if(draw)XftDrawDestroy(draw);if(back)XFreePixmap(d,back);
        for(auto f:{small,normal,large})if(f)XftFontClose(d,f);
        if(d){for(int i=0;i<allocated;++i)XftColorFree(d,DefaultVisual(d,DefaultScreen(d)),DefaultColormap(d,DefaultScreen(d)),&colors[i]);if(gc)XFreeGC(d,gc);if(window)XDestroyWindow(d,window);XCloseDisplay(d);}}
    void start(){
        d=XOpenDisplay(nullptr);if(!d)throw std::runtime_error("Mode page DISPLAY unavailable");int screen=DefaultScreen(d);
        width=std::max(480,std::min(1600,DisplayWidth(d,screen)-16));height=std::max(360,std::min(1030,DisplayHeight(d,screen)-75));
        window=XCreateSimpleWindow(d,DefaultRootWindow(d),0,0,unsigned(width),unsigned(height),0,0,0x0b1320);
        XStoreName(d,window,"Xinxing | Mode handoff and parking");
        wm_close=XInternAtom(d,"WM_DELETE_WINDOW",False);XSetWMProtocols(d,window,&wm_close,1);
        Atom type=XInternAtom(d,"_NET_WM_WINDOW_TYPE",False),dialog=XInternAtom(d,"_NET_WM_WINDOW_TYPE_DIALOG",False);
        XChangeProperty(d,window,type,XA_ATOM,32,PropModeReplace,reinterpret_cast<unsigned char*>(&dialog),1);
        Atom state_atom=XInternAtom(d,"_NET_WM_STATE",False),above=XInternAtom(d,"_NET_WM_STATE_ABOVE",False);
        XChangeProperty(d,window,state_atom,XA_ATOM,32,PropModeReplace,reinterpret_cast<unsigned char*>(&above),1);
        XSelectInput(d,window,ExposureMask|StructureNotifyMask|ButtonPressMask|ButtonReleaseMask|FocusChangeMask);
        gc=XCreateGC(d,window,0,nullptr);
        auto font=[&](int n){return XftFontOpenName(d,screen,("WenQuanYi Micro Hei:pixelsize="+std::to_string(n)).c_str());};
        small=font(14);normal=font(18);large=font(32);if(!small||!normal||!large)throw std::runtime_error("Mode page CJK font unavailable");
        for(const char* color:{"#eef5ff","#8ea7c3","#48d8b0","#ffca69","#ff6975"}){if(!XftColorAllocName(d,DefaultVisual(d,screen),DefaultColormap(d,screen),color,&colors[allocated]))throw std::runtime_error("Mode page palette unavailable");++allocated;}
        logo.start(d,window);XMapRaised(d,window);XMoveWindow(d,window,0,0);XSync(d,False);
    }
    void text(int x,int y,std::string s,int color=0,XftFont* font=nullptr){if(!font)font=normal;
        XGlyphInfo extent{};auto fits=[&]{XftTextExtentsUtf8(d,font,reinterpret_cast<const FcChar8*>(s.data()),int(s.size()),&extent);return extent.xOff<=width-x-28;};
        while(!s.empty()&&!fits()){size_t n=s.size()-1;while(n&&(static_cast<unsigned char>(s[n])&0xc0)==0x80)--n;s.resize(n);}
        XftDrawStringUtf8(draw,&colors[color],font,x,y,reinterpret_cast<const FcChar8*>(s.data()),int(s.size()));}
    void rect(int x,int y,int w,int h,unsigned long color){XSetForeground(d,gc,color);XFillRectangle(d,back,gc,x,y,unsigned(w),unsigned(h));}
    int hit(int x,int y)const{
        int cw=std::min(760,width-48),left=(width-cw)/2,top=std::max(90,(height-390)/2);
        if(!state.interactive()||y<top+280||y>=top+330)return -1;
        return x>=left+28&&x<left+cw/2-6?0:x>=left+cw/2+6&&x<left+cw-28?1:-1;
    }
    void paint(){
        if(draw)XftDrawDestroy(draw);
        if(back)XFreePixmap(d,back);
        back=XCreatePixmap(d,window,unsigned(width),unsigned(height),unsigned(DefaultDepth(d,DefaultScreen(d))));
        draw=XftDrawCreate(d,back,DefaultVisual(d,DefaultScreen(d)),DefaultColormap(d,DefaultScreen(d)));
        rect(0,0,width,height,0x0b1320);logo.paint(back,gc,28,22);text(154,44,"芯星 · 环视守护",0,normal);
        int cw=std::min(760,width-48),left=(width-cw)/2,top=std::max(90,(height-390)/2);
        rect(left,top,cw,365,0x17263a);rect(left,top,cw,3,state.mode=="FAULT"?0xff6975:0x48d8b0);
        text(left+28,top+42,state.interactive()?"CONTROL / 工作模式":"HANDOFF / 模式切换",1,small);
        text(left+28,top+96,state.mode=="PARK"?"驻车守护已开启":state.mode=="FAULT"?"切换未完成":"正在进入"+std::string(title(state.target)),state.mode=="FAULT"?4:0,large);
        text(left+28,top+143,state.phase,2,normal);
        text(left+28,top+188,state.mode=="PARK"?"请在电脑或手机打开 RK 局域网网页查看视频与告警。":"窗口正在交接，收到目标画面后自动进入。",1,small);
        text(left+28,top+216,state.mode=="PARK"?"驻车识别按当前配置运行；此页不解码视频。":"识别资源按模式交接，不同时启动两组推理。",1,small);
        if(!state.error.empty())text(left+28,top+250,"详细原因见网页状态与事件日志",4,small);
        if(state.interactive()){
            int bw=cw/2-34;rect(left+28,top+280,bw,50,down==0?0x286451:0x194739);
            rect(left+cw/2+6,top+280,bw,50,down==1?0x34506b:0x21394d);
            text(left+48,top+312,"返回驾驶",2,normal);text(left+cw/2+26,top+312,"360 环视",0,normal);
        }else{text(left+28,top+310,"等待设备确认 · 请勿重复点击切换",3,small);}
        text(28,height-24,"静态控制页 · 无视频解码 / 无模型推理",1,small);
        XCopyArea(d,back,window,gc,0,0,unsigned(width),unsigned(height),0,0);XFlush(d);dirty=false;
    }
};
void request(const std::filesystem::path& root,const char* mode){auto file=root/"mode_request.json";std::ofstream out(file.string()+".hub.tmp");out<<"{\"mode\":\""<<mode<<"\",\"source\":\"parking_hub\"}";out.close();if(!out)throw std::runtime_error("Cannot publish mode request");std::filesystem::rename(file.string()+".hub.tmp",file);}
}
int main(int argc,char** argv){try{
    if(argc!=3||std::string(argv[1])!="--state")throw std::runtime_error("Usage: rk_mode_hub --state DIR");
    std::filesystem::path root=argv[2];std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);Page page;page.start();
    auto old=std::filesystem::file_time_type::min();bool announced=false;
    while(!stopped){
        std::error_code error;auto stamp=std::filesystem::last_write_time(root/"display_mode.json",error);
        if(!error&&stamp!=old){cv::FileStorage file((root/"display_mode.json").string(),cv::FileStorage::READ);if(file.isOpened()){
            State next;file["mode"]>>next.mode;file["target"]>>next.target;file["phase"]>>next.phase;file["error"]>>next.error;
            if(!(next==page.state)){page.state=next;page.down=-1;page.dirty=true;}old=stamp;}}
        while(XPending(page.d)){XEvent e;XNextEvent(page.d,&e);
            if(e.type==ClientMessage&&Atom(e.xclient.data.l[0])==page.wm_close&&page.state.interactive()){
                request(root,"DRIVE");page.state.mode="SWITCHING";page.state.target="DRIVE";page.state.phase="正在返回驾驶";page.dirty=true;
            }
            if(e.type==Expose)page.dirty=true;
            if(e.type==ConfigureNotify){page.width=std::max(480,e.xconfigure.width);page.height=std::max(360,e.xconfigure.height);page.dirty=true;}
            if(e.type==FocusOut){page.down=-1;page.dirty=true;}
            if((e.type==ButtonPress||e.type==ButtonRelease)&&e.xbutton.button==Button1){int hit=page.hit(e.xbutton.x,e.xbutton.y);page.dirty=true;
                if(e.type==ButtonPress)page.down=hit;else {int before=page.down;page.down=-1;if(hit>=0&&hit==before){request(root,hit?"PANORAMA":"DRIVE");page.state.mode="SWITCHING";page.state.target=hit?"PANORAMA":"DRIVE";page.state.phase="模式请求已发送";}}}
        }
        if(page.dirty)page.paint();
        if(!announced){XSync(page.d,False);std::ofstream ready(root/"hub_ready.tmp");ready<<"{\"pid\":"<<getpid()<<"}";ready.close();if(!ready)throw std::runtime_error("Cannot publish mode page readiness");std::filesystem::rename(root/"hub_ready.tmp",root/"hub_ready.json");announced=true;}
        pollfd fd{ConnectionNumber(page.d),POLLIN,0};poll(&fd,1,200);
    }
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"MODE PAGE ERROR: %s\n",e.what());return 1;}}
