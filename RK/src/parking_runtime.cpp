#include "parking_runtime.hpp"
#include "parking_guard.hpp"
#include "detector_runtime.hpp"
#include "rk_dma.hpp"
#include <condition_variable>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace parking {
namespace {
std::string quote(const std::string& value){std::string out="\"";for(unsigned char c:value){if(c=='\\'||c=='\"'){out+='\\';out+=char(c);}else if(c>=32)out+=char(c);else out+=' ';}return out+'\"';}
Rules load(const std::string& path){
    cv::FileStorage f(path,cv::FileStorage::READ);if(!f.isOpened())throw std::runtime_error("Missing parking rules");Rules r;
    auto number=[&](const char* name,double& value){if(f[name].empty())throw std::runtime_error(std::string("Missing parking rule: ")+name);f[name]>>value;};
    int enabled=0;f["enabled"]>>enabled;r.enabled=enabled!=0;f["revision"]>>r.revision;
    number("sweep_hz",r.sweep_hz);number("confidence",r.confidence);number("person_yellow",r.person_yellow);number("person_red",r.person_red);
    number("vehicle_yellow",r.vehicle_yellow);number("vehicle_red",r.vehicle_red);number("yellow_y",r.yellow_y);number("red_y",r.red_y);
    number("confirm_s",r.confirm_s);number("hold_s",r.hold_s);number("cooldown_s",r.cooldown_s);
    if(!r.valid())throw std::runtime_error("Parking thresholds out of range");
    return r;
}
bool valid_image(const uint8_t* data,unsigned ch){
    int minimum=255,maximum=0;double sum=0,square=0;int n=0;
    for(int y=20;y<360;y+=48)for(int x=20;x<640;x+=72){size_t at=(size_t(y+int(ch/2)*360)*1280+x+int(ch%2)*640)*2;unsigned p=unsigned(data[at])|(unsigned(data[at+1])<<8);int v=(int((p>>11)&31)*255/31+int((p>>5)&63)*255/63+int(p&31)*255/31)/3;
        minimum=std::min(minimum,v);maximum=std::max(maximum,v);sum+=v;square+=v*v;++n;}
    return maximum-minimum>=12&&square/n-(sum/n)*(sum/n)>=16;
}
void rules_json(std::ostream& out,const Rules& r){
    out<<"{\"revision\":"<<quote(r.revision)<<",\"enabled\":"<<(r.enabled?"true":"false")<<",\"sweep_hz\":"<<r.sweep_hz<<",\"confidence\":"<<r.confidence
      <<",\"person_yellow\":"<<r.person_yellow<<",\"person_red\":"<<r.person_red<<",\"vehicle_yellow\":"<<r.vehicle_yellow<<",\"vehicle_red\":"<<r.vehicle_red
      <<",\"yellow_y\":"<<r.yellow_y<<",\"red_y\":"<<r.red_y<<",\"confirm_s\":"<<r.confirm_s<<",\"hold_s\":"<<r.hold_s<<",\"cooldown_s\":"<<r.cooldown_s<<'}';
}
}
struct Runtime::Impl {
    Options options;std::mutex mutex;std::condition_variable ready;std::shared_ptr<qv::Frame> pending;double pts=0;bool stopping=false;
    std::thread thread;int sock=-1;std::string session;uint64_t sequence=0,ipc_dropped=0;std::array<double,4> last_finished{};
    explicit Impl(const Options& o):options(o),session(std::to_string(getpid())+"-"+std::to_string(uint64_t(qv::now_seconds()*1000000))){thread=std::thread([this]{run();});}
    ~Impl(){{std::lock_guard<std::mutex> lock(mutex);stopping=true;}ready.notify_all();if(thread.joinable())thread.join();if(sock>=0)close(sock);}
    void send(const std::string& message){
        if(options.socket.empty())return;
        if(sock<0)sock=socket(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC,0);
        if(sock<0)return;
        sockaddr_un addr{};addr.sun_family=AF_UNIX;if(options.socket.size()>=sizeof(addr.sun_path)){++ipc_dropped;return;}
        std::memcpy(addr.sun_path,options.socket.c_str(),options.socket.size()+1);
        if(sendto(sock,message.data(),message.size(),MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0)++ipc_dropped;
    }
    void status(const std::string& error,const Rules* rules=nullptr){std::ostringstream out;out<<"{\"session\":"<<quote(session)<<",\"sequence\":"<<++sequence<<",\"finished_s\":"<<std::setprecision(15)<<qv::now_seconds()<<",\"ready\":false,\"error\":"<<quote(error);if(rules){out<<",\"rules\":";rules_json(out,*rules);}out<<",\"channels\":[]}";send(out.str());}
    void run(){
        try{
            cv::setNumThreads(1);Rules rules;Engine engine;std::unique_ptr<ai::Detector> detector;std::unique_ptr<qv::DmaBuffer> rgb;
            int rgb_format=0;std::filesystem::file_time_type modified{};double next=0,last_config=0,last_status=0;
            while(true){
                std::shared_ptr<qv::Frame> frame;double video_pts=0;
                {std::unique_lock<std::mutex> lock(mutex);ready.wait_for(lock,std::chrono::milliseconds(100),[&]{return stopping||pending;});if(stopping)break;frame=std::move(pending);video_pts=pts;}
                double now=qv::now_seconds();
                if(now-last_config>=.5){
                    auto value=std::filesystem::last_write_time(options.rules);if(value!=modified){rules=load(options.rules);engine.configure(rules);modified=value;}last_config=now;
                }
                if(!rules.enabled||!std::filesystem::exists(options.active)){
                    if(now-last_status>=1){status(rules.enabled?"等待驻车模式确认":"驻车识别已关闭",&rules);last_status=now;}continue;
                }
                if(!detector){
                    status("正在加载驻车 YOLOv8n",&rules);detector=std::make_unique<ai::Detector>(options.model,options.output,true,"parking_model_info.txt");
                    if(detector->width()!=640||detector->height()!=384||detector->class_count()!=8)throw std::runtime_error("Parking requires explicit 640x384 eight-class YOLOv8 model");
                    rgb=std::make_unique<qv::DmaBuffer>(options.heap,640*384*3);rgb_format=qv::verify_rga_rgb888(options.heap,options.rgb565);
                    rgb->begin_write();std::memset(rgb->data(),114,rgb->size());rgb->finish_write();
                }
                now=qv::now_seconds();if(!frame||now<next)continue;
                next=rules.sweep_hz>0?now+1/rules.sweep_hz:0;
                if(now-frame->received>.5){status("收到的驻车画面已过期");continue;}
                auto source=std::dynamic_pointer_cast<qv::DmaBuffer>(frame->memory);if(!source)throw std::runtime_error("Parking detector requires DMA input");
                std::array<std::vector<Target>,4> targets;std::array<bool,4> valid{};std::array<double,4> api{},post{},hz{};
                source->begin_read();for(unsigned ch=0;ch<4;++ch)valid[ch]=valid_image(source->data(),ch);source->finish_read();
                for(unsigned ch=0;ch<4;++ch){
                    std::vector<ai::Box> boxes;
                    if(valid[ch]){
                        qv::rga_blit_region(*source,1280,720,options.rgb565,*rgb,640,384,rgb_format,{int(ch%2)*640,int(ch/2)*360,640,360},{0,12,640,360});
                        rgb->begin_read();try{boxes=detector->infer(rgb->data(),float(rules.confidence),.45f,api[ch],post[ch]);}catch(...){rgb->finish_read();throw;}rgb->finish_read();
                        for(auto& b:boxes)b=normalized_detector_box(b);
                    }
                    double finished=qv::now_seconds();if(valid[ch]&&last_finished[ch]>0)hz[ch]=1/(finished-last_finished[ch]);last_finished[ch]=finished;
                    // Alarm confirmation follows distinct input observation times, not NPU waiting time.
                    targets[ch]=engine.update(ch,boxes,frame->received,valid[ch]);
                }
                std::ostringstream out;out<<std::setprecision(12)<<"{\"session\":"<<quote(session)<<",\"sequence\":"<<++sequence<<",\"finished_s\":"<<qv::now_seconds()
                  <<",\"received_s\":"<<frame->received<<",\"video_pts\":"<<video_pts<<",\"frame_id\":"<<frame->id<<",\"ready\":true,\"error\":\"\",\"ipc_dropped\":"<<ipc_dropped<<",\"coverage\":\"composite_frame_only\",\"rules\":";rules_json(out,rules);out<<",\"channels\":[";
                for(unsigned ch=0;ch<4;++ch){if(ch)out<<',';int level=0;for(const auto& t:targets[ch])level=std::max(level,t.level);
                    out<<"{\"channel\":"<<ch<<",\"valid\":"<<(valid[ch]?"true":"false")<<",\"level\":"<<level<<",\"hz\":"<<hz[ch]<<",\"api_ms\":"<<api[ch]<<",\"post_ms\":"<<post[ch]<<",\"boxes\":[";
                    for(size_t i=0;i<targets[ch].size();++i){if(i)out<<',';const auto& t=targets[ch][i];
                        out<<"{\"id\":"<<t.id<<",\"category\":"<<t.category<<",\"score\":"<<t.raw.score<<",\"level\":"<<t.level<<",\"desired\":"<<t.desired<<",\"area\":"<<t.area<<",\"foot\":"<<t.foot<<",\"pending_s\":"<<t.pending_s
                          <<",\"held\":"<<(t.held?"true":"false")<<",\"last_seen_s\":"<<t.last_seen<<",\"event\":"<<t.event<<",\"xyxy\":["<<t.box.x1<<','<<t.box.y1<<','<<t.box.x2<<','<<t.box.y2<<"]}";
                    }out<<"]}";
                }out<<"]}";send(out.str());
            }
        }catch(const std::exception& e){
            while(true){{std::unique_lock<std::mutex> lock(mutex);if(stopping)break;ready.wait_for(lock,std::chrono::seconds(1),[&]{return stopping;});if(stopping)break;pending.reset();}status(e.what());}
        }
    }
};
Runtime::Runtime(const Options& options):impl(std::make_unique<Impl>(options)){}
Runtime::~Runtime()=default;
void Runtime::submit(std::shared_ptr<qv::Frame> frame,double pts){{std::lock_guard<std::mutex> lock(impl->mutex);impl->pending=std::move(frame);impl->pts=pts;}impl->ready.notify_one();}
}
