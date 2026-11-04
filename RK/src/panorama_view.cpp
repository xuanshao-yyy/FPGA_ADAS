// FPGA QV04 panorama -> DMA RGB565 -> RGA NV12 -> native XVideo child.
// Display only: no image stitching, model inference, or CPU color conversion.
#include "udp.hpp"
#include "rk_dma.hpp"
#include "panorama_layout.hpp"
#include "brand_logo.hpp"
#include <gst/app/gstappsrc.h>
#include <gst/video/videooverlay.h>
#include <X11/Xft/Xft.h>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<bool> stopped{false};
void interrupt(int){stopped=true;}
void compose_panorama(qv::DmaBuffer& source,qv::DmaBuffer& target,int rgb565,bool crop){
    int height=crop?270:360,y=crop?45:0;
    qv::rga_blit_region(source,2560,360,rgb565,target,2560,height,RK_FORMAT_YCbCr_420_SP,{0,y,2560,height},{0,0,2560,height});
    for(const auto& r:panorama_display::SideCopies)
        qv::rga_blit_region(source,2560,360,rgb565,target,2560,height,RK_FORMAT_YCbCr_420_SP,{r.source_x,y,r.width,height},{r.destination_x,0,r.width,height});
}
void verify_panorama_sides(const std::string& heap,int rgb565){
    qv::DmaBuffer source(heap,2560*360*2),target(heap,2560*360*3/2);
    source.begin_write();
    for(int y=0;y<360;++y)for(int x=0;x<2560;++x){unsigned color=x>=500&&x<968?0xf800:x>=1592&&x<2060?0x07e0:0x001f;
        auto p=source.data()+(y*2560+x)*2;p[0]=uint8_t(color);p[1]=uint8_t(color>>8);}
    source.finish_write();
    for(bool crop:{false,true}){compose_panorama(source,target,rgb565,crop);target.begin_read();
        bool valid=std::abs(int(target.data()[100*2560+700])-145)<=5&&std::abs(int(target.data()[100*2560+1800])-81)<=5;
        for(int x:{100,1280,2440})valid=valid&&std::abs(int(target.data()[100*2560+x])-41)<=5;
        target.finish_read();if(!valid)throw std::runtime_error("RGA panorama side exchange failed");}
    std::cout<<"RGA panorama side self-check PASS: RC2 display left=CH2 right=CH1; front/rear preserved, full/crop\n";
}
void save(const std::string& dir,const std::string& name,const std::string& json){
    auto path=std::filesystem::path(dir)/name;std::ofstream file(path.string()+".tmp");
    file<<json;file.close();if(!file)throw std::runtime_error("Cannot publish panorama state");
    std::filesystem::rename(path.string()+".tmp",path);
}
struct View {
    brand::Logo logo;
    Display* d=nullptr;Window root=0,video=0;Atom close=0;GC gc=0;XftDraw* draw=nullptr;XftFont* font=nullptr;
    XftColor ink{},muted{},green{};int allocated=0,width=1500,height=480,down=-1;bool crop=false,mapped=false;
    ~View(){logo.close();if(draw)XftDrawDestroy(draw);if(font)XftFontClose(d,font);
        if(d){for(int i=0;i<allocated;++i)XftColorFree(d,DefaultVisual(d,DefaultScreen(d)),DefaultColormap(d,DefaultScreen(d)),i==0?&ink:i==1?&muted:&green);
            if(gc)XFreeGC(d,gc);
            if(root)XDestroyWindow(d,root);
            XCloseDisplay(d);}}
    void start(){
        d=XOpenDisplay(nullptr);if(!d)throw std::runtime_error("DISPLAY unavailable");
        int screen=DefaultScreen(d);width=std::min(1600,DisplayWidth(d,screen)-16);height=std::min(650,DisplayHeight(d,screen)-90);
        root=XCreateSimpleWindow(d,RootWindow(d,screen),0,0,width,height,0,0,0x101b26);
        video=XCreateSimpleWindow(d,root,0,80,width,360,0,0,0x000000);
        XStoreName(d,root,"FPGA 360 Panorama | QV04 | RK3568");
        XSelectInput(d,root,StructureNotifyMask|ExposureMask|ButtonPressMask|ButtonReleaseMask|FocusChangeMask);
        close=XInternAtom(d,"WM_DELETE_WINDOW",False);XSetWMProtocols(d,root,&close,1);
        gc=XCreateGC(d,root,0,nullptr);draw=XftDrawCreate(d,root,DefaultVisual(d,screen),DefaultColormap(d,screen));
        font=XftFontOpenName(d,screen,"WenQuanYi Micro Hei:pixelsize=17");
        if(!font)throw std::runtime_error("CJK font unavailable");
        for(auto color:{"#eef6ff","#96aabb","#48d7a0"}){
            if(!XftColorAllocName(d,DefaultVisual(d,screen),DefaultColormap(d,screen),color,allocated==0?&ink:allocated==1?&muted:&green))throw std::runtime_error("Xft color allocation failed");
            ++allocated;}
        logo.start(d,root);XMapRaised(d,root);XSync(d,False);layout();
    }
    void layout(){int vh=std::min(height-145,width*(crop?270:360)/2560);vh=std::max(16,vh);int vw=vh*2560/(crop?270:360);
        XMoveResizeWindow(d,video,(width-vw)/2,88+(height-145-vh)/2,vw,vh);}
    void text(int x,int y,const std::string& s,XftColor* color=nullptr){XftDrawStringUtf8(draw,color?color:&ink,font,x,y,reinterpret_cast<const FcChar8*>(s.data()),int(s.size()));}
    int hit(int x,int y)const {return y>=13&&y<51&&x>=18&&x<528?(x-18)/170:-1;}
    void paint(const std::string& status){
        XClearWindow(d,root);logo.paint(root,gc,width-brand::Width-20,18);
        const char* labels[]{"返回行驶","驻车监控",crop?"显示完整 360 行":"平齐裁剪 270 行"};
        for(int i=0;i<3;++i){XSetForeground(d,gc,down==i?0x2d4557:0x14392f);XFillRectangle(d,root,gc,18+i*170,13,158,38);text(30+i*170,39,labels[i],&green);}
        text(20,76,"360 环视 · 后 → 左 → 前 → 右 → 后  |  RK 调换左右，非标定鸟瞰",&muted);
        text(20,height-28,status,&muted);XFlush(d);
    }
    void visible(bool fresh){if(fresh!=mapped){if(fresh)XMapWindow(d,video);else XUnmapWindow(d,video);mapped=fresh;XFlush(d);}}
};
}
int main(int argc,char** argv){
    GstElement *pipeline=nullptr,*input=nullptr,*sink=nullptr;GstBus* bus=nullptr;
    View view; // Outlives pipeline callbacks, including exception cleanup.
    try{
        std::string bind="192.168.10.1",peer="192.168.10.2",state,heap="/dev/dma_heap/system-dma32";
        for(int i=1;i<argc;++i){std::string key=argv[i];if(key=="--help"){std::cout<<"--bind IP --peer IP --state DIR\n";return 0;}
            if(i+1>=argc)throw std::runtime_error("Missing value: "+key);
            std::string value=argv[++i];
            if(key=="--bind")bind=value;else if(key=="--peer")peer=value;else if(key=="--state")state=value;else throw std::runtime_error("Unknown option: "+key);}
        if(state.empty())throw std::runtime_error("--state required");
        std::filesystem::create_directories(state);std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);
        if(!XInitThreads())throw std::runtime_error("XInitThreads failed");
        gst_init(&argc,&argv);view.start();
        GError* error=nullptr;pipeline=gst_parse_launch("appsrc name=input is-live=true format=time block=false emit-signals=false max-bytes=1382400 ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! xvimagesink name=preview sync=false enable-last-sample=false",&error);
        if(error){std::string reason=error->message;g_error_free(error);throw std::runtime_error(reason);}
        if(!pipeline)throw std::runtime_error("Cannot construct XVideo pipeline");
        input=gst_bin_get_by_name(GST_BIN(pipeline),"input");sink=gst_bin_get_by_name(GST_BIN(pipeline),"preview");bus=gst_element_get_bus(pipeline);
        gst_bus_set_sync_handler(bus,[](GstBus*,GstMessage* msg,gpointer data){
            if(gst_is_video_overlay_prepare_window_handle_message(msg)){gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg)),static_cast<View*>(data)->video);gst_message_unref(msg);return GST_BUS_DROP;}return GST_BUS_PASS;},&view,nullptr);
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink),view.video);gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(sink),FALSE);
        auto caps=[&]{auto c=gst_caps_new_simple("video/x-raw","format",G_TYPE_STRING,"NV12","width",G_TYPE_INT,2560,"height",G_TYPE_INT,view.crop?270:360,"framerate",GST_TYPE_FRACTION,30,1,"pixel-aspect-ratio",GST_TYPE_FRACTION,1,1,"colorimetry",G_TYPE_STRING,"bt601",nullptr);gst_app_src_set_caps(GST_APP_SRC(input),c);gst_caps_unref(c);};caps();
        int rgb565=qv::verify_rga_rgb565(heap);
        verify_panorama_sides(heap,rgb565);
        qv::FrameFactory factory=[heap]{return std::make_unique<qv::Frame>(std::make_shared<qv::DmaBuffer>(heap,qv::FrameBytes));};
        qv::UdpReceiver receiver({bind,peer,5200,30,"PANORAMA_QV04"},factory,6,300,2);receiver.start();
        std::array<std::shared_ptr<qv::DmaBuffer>,5> pool;for(auto& b:pool)b=std::make_shared<qv::DmaBuffer>(heap,2560*360*3/2);
        if(gst_element_set_state(pipeline,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE)throw std::runtime_error("XVideo cannot start");
        double base=qv::now_seconds(),last=base-1,received=-100;uint64_t pushed=0,dropped=0;uint32_t fid=0;std::string next="DRIVE";bool redraw=true;
        while(!stopped){
            while(XPending(view.d)){XEvent e;XNextEvent(view.d,&e);
                if(e.type==ClientMessage&&Atom(e.xclient.data.l[0])==view.close)stopped=true;
                if(e.type==ConfigureNotify&&e.xconfigure.window==view.root){view.width=e.xconfigure.width;view.height=e.xconfigure.height;view.layout();redraw=true;}
                if(e.type==Expose)redraw=true;
                if(e.type==FocusOut){view.down=-1;redraw=true;}
                if((e.type==ButtonPress||e.type==ButtonRelease)&&e.xbutton.button==Button1){int hit=view.hit(e.xbutton.x,e.xbutton.y);redraw=true;
                    if(e.type==ButtonPress)view.down=hit;else {int old=view.down;view.down=-1;if(hit>=0&&old==hit){if(hit<2){next=hit==0?"DRIVE":"PARK";stopped=true;}else {view.crop=!view.crop;view.layout();caps();}}}}
            }
            if(auto msg=gst_bus_pop_filtered(bus,GstMessageType(GST_MESSAGE_ERROR|GST_MESSAGE_EOS))){std::string why="XVideo EOS";if(GST_MESSAGE_TYPE(msg)==GST_MESSAGE_ERROR){GError* err=nullptr;gchar* debug=nullptr;gst_message_parse_error(msg,&err,&debug);why=err?err->message:why;if(err)g_error_free(err);g_free(debug);}gst_message_unref(msg);throw std::runtime_error(why);}
            if(!receiver.error().empty())throw std::runtime_error(receiver.error());
            auto frame=receiver.latest.take();
            if(frame){auto it=std::find_if(pool.begin(),pool.end(),[](const auto& b){return b.use_count()==1;});
                if(it==pool.end()||gst_app_src_get_current_level_bytes(GST_APP_SRC(input))>=2560*360*3/2)++dropped;
                else {auto src=std::dynamic_pointer_cast<qv::DmaBuffer>(frame->memory);if(!src)throw std::runtime_error("QV04 input is not DMA");int h=view.crop?270:360;
                    compose_panorama(*src,**it,rgb565,view.crop);
                    auto buffer=qv::wrap_nv12_dma(*it,2560,h);GST_BUFFER_PTS(buffer)=GstClockTime((qv::now_seconds()-base)*GST_SECOND);GST_BUFFER_DURATION(buffer)=GST_SECOND/30;
                    if(gst_app_src_push_buffer(GST_APP_SRC(input),buffer)!=GST_FLOW_OK)throw std::runtime_error("XVideo stopped accepting frames");
                    received=frame->received;fid=frame->id;++pushed;}}
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
            double now=qv::now_seconds();bool fresh=now-received<.8;view.visible(fresh);
            if(now-last>=.5||redraw){auto s=receiver.stats();
                view.paint(std::string(fresh?"实时接收":"等待新画面 / 视频已过期")+"  QV04 2560×360  |  完整帧 "+std::to_string(s.complete)+"  |  已推送 "+std::to_string(pushed)+"  |  丢弃旧帧 "+std::to_string(dropped));
                save(state,"panorama_ready.json","{\"pid\":"+std::to_string(getpid())+",\"monotonic_s\":"+std::to_string(now)+",\"received_s\":"+std::to_string(received)+",\"frame_id\":"+std::to_string(fid)+",\"complete\":"+std::to_string(s.complete)+",\"ready\":"+(fresh?"true":"false")+",\"source\":\"QV04\"}");last=now;redraw=false;}
        }
        receiver.stop();gst_element_set_state(pipeline,GST_STATE_NULL);
        // Signal-driven shutdown belongs to supervisor; only mouse/window exit requests a mode.
        if(!std::filesystem::exists(std::filesystem::path(state)/"panorama_stopping"))save(state,"mode_request.json","{\"mode\":\""+next+"\",\"source\":\"panorama_ui\"}");
        gst_object_unref(bus);gst_object_unref(input);gst_object_unref(sink);gst_object_unref(pipeline);return 0;
    }catch(const std::exception& e){std::cerr<<"PANORAMA ERROR: "<<e.what()<<std::endl;
        if(pipeline)gst_element_set_state(pipeline,GST_STATE_NULL);
        if(bus)gst_object_unref(bus);
        if(input)gst_object_unref(input);
        if(sink)gst_object_unref(sink);
        if(pipeline)gst_object_unref(pipeline);
        return 1;}
}
