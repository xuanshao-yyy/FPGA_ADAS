// Parking-only pipeline: QV03 -> DMA RGB565 -> RGA NV12 -> MPP H.264 -> fMP4.
// Optional latest-frame parking YOLO worker; no X11 or lane worker.
#include "udp.hpp"
#include "rk_dma.hpp"
#include "parking_runtime.hpp"
#include <gst/app/gstappsrc.h>
#include <atomic>
#include <algorithm>
#include <csignal>
#include <iostream>
#include <filesystem>
#include <thread>

namespace {std::atomic<bool> stop{false};void interrupt(int){stop=true;}}
int main(int argc,char** argv){
    GstElement *pipeline=nullptr,*input=nullptr;
    try{
        std::string bind="192.168.10.1",peer="192.168.10.2",output,heap="/dev/dma_heap/system-dma32";
        unsigned fps=15,bitrate=2500000;parking::Options guard_options;
        for(int i=1;i<argc;++i){std::string key=argv[i];
            if(key=="--help"){std::cout<<"--bind IP --peer IP --output FIFO --fps 15 --bitrate 2500000 --heap PATH --guard-model explicit_v8.rknn --guard-socket PATH --guard-rules JSON --guard-active PATH\n";return 0;}
            if(i+1>=argc)throw std::runtime_error("Missing value: "+key);
            std::string value=argv[++i];
            if(key=="--bind")bind=value;else if(key=="--peer")peer=value;else if(key=="--output")output=value;
            else if(key=="--heap")heap=value;else if(key=="--fps")fps=std::stoul(value);else if(key=="--bitrate")bitrate=std::stoul(value);
            else if(key=="--guard-model")guard_options.model=value;else if(key=="--guard-socket")guard_options.socket=value;
            else if(key=="--guard-rules")guard_options.rules=value;else if(key=="--guard-active")guard_options.active=value;
            else throw std::runtime_error("Unknown option: "+key);
        }
        if(output.empty()||fps<1||fps>30||bitrate<200000||bitrate>8000000)throw std::runtime_error("Invalid parking stream settings");
        gst_init(&argc,&argv);std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);std::signal(SIGPIPE,SIG_IGN);
        auto factory=gst_element_factory_find("mpph264enc");if(!factory)throw std::runtime_error("mpph264enc unavailable: no software encoding fallback");gst_object_unref(factory);
        GError* error=nullptr;
        pipeline=gst_parse_launch("appsrc name=input is-live=true format=time block=false max-bytes=1382400 ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! mpph264enc name=encoder ! h264parse config-interval=-1 ! video/x-h264,stream-format=avc,alignment=au ! mp4mux fragment-duration=200 streamable=true ! filesink name=output sync=false",&error);
        if(error){std::string msg=error->message;g_error_free(error);throw std::runtime_error(msg);}
        if(!pipeline)throw std::runtime_error("Cannot construct parking pipeline");
        auto encoder=gst_bin_get_by_name(GST_BIN(pipeline),"encoder");
        if(!g_object_class_find_property(G_OBJECT_GET_CLASS(encoder),"bps")||!g_object_class_find_property(G_OBJECT_GET_CLASS(encoder),"gop")){gst_object_unref(encoder);throw std::runtime_error("MPP encoder missing bps/gop properties");}
        g_object_set(encoder,"bps",bitrate,"gop",int(std::max(1u,(fps+4)/5)),nullptr);gst_object_unref(encoder);
        auto sink=gst_bin_get_by_name(GST_BIN(pipeline),"output");g_object_set(sink,"location",output.c_str(),nullptr);gst_object_unref(sink);
        input=gst_bin_get_by_name(GST_BIN(pipeline),"input");
        auto caps=gst_caps_new_simple("video/x-raw","format",G_TYPE_STRING,"NV12","width",G_TYPE_INT,1280,"height",G_TYPE_INT,720,"framerate",GST_TYPE_FRACTION,int(fps),1,nullptr);
        gst_app_src_set_caps(GST_APP_SRC(input),caps);gst_caps_unref(caps);
        const int rgb565=qv::verify_rga_rgb565(heap);
        std::unique_ptr<parking::Runtime> guard;
        if(!guard_options.socket.empty()){
            guard_options.heap=heap;guard_options.rgb565=rgb565;guard_options.output=std::filesystem::path(output).parent_path().string();
            guard=std::make_unique<parking::Runtime>(guard_options);
        }
        qv::FrameFactory source=[heap]{return std::make_unique<qv::Frame>(std::make_shared<qv::DmaBuffer>(heap,qv::FrameBytes));};
        qv::UdpReceiver receiver({bind,peer,5100,fps,"PARK_QV03"},source,6,300,true);receiver.start();
        std::array<std::shared_ptr<qv::DmaBuffer>,5> pool;
        for(auto& buffer:pool)buffer=std::make_shared<qv::DmaBuffer>(heap,1280*720*3/2);
        if(gst_element_set_state(pipeline,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE)throw std::runtime_error("Parking pipeline cannot start");
        auto bus=gst_element_get_bus(pipeline);double base=qv::now_seconds(),last=base;uint64_t pushed=0,dropped=0;
        while(!stop){
            if(auto msg=gst_bus_pop_filtered(bus,GstMessageType(GST_MESSAGE_ERROR|GST_MESSAGE_EOS))){
                std::string reason="Parking stream EOS";if(GST_MESSAGE_TYPE(msg)==GST_MESSAGE_ERROR){GError* e=nullptr;gchar* debug=nullptr;gst_message_parse_error(msg,&e,&debug);reason=e?e->message:"GStreamer error";if(e)g_error_free(e);g_free(debug);}gst_message_unref(msg);gst_object_unref(bus);throw std::runtime_error(reason);}
            if(!receiver.error().empty()){gst_object_unref(bus);throw std::runtime_error(receiver.error());}
            auto frame=receiver.latest.take();
            if(frame){auto it=std::find_if(pool.begin(),pool.end(),[](const auto& b){return b.use_count()==1;});
                if(it==pool.end()||gst_app_src_get_current_level_bytes(GST_APP_SRC(input))>=1280*720*3/2)++dropped;
                else{auto src=std::dynamic_pointer_cast<qv::DmaBuffer>(frame->memory);if(!src)throw std::runtime_error("Parking input is not DMA");
                    qv::rga_blit(*src,1280,720,rgb565,**it,1280,720,RK_FORMAT_YCbCr_420_SP,{0,0,1280,720});
                    auto buffer=qv::wrap_nv12_dma(*it,1280,720);
                    double video_pts=qv::now_seconds()-base;
                    GST_BUFFER_PTS(buffer)=GstClockTime(video_pts*GST_SECOND);GST_BUFFER_DURATION(buffer)=GST_SECOND/fps;
                    if(gst_app_src_push_buffer(GST_APP_SRC(input),buffer)!=GST_FLOW_OK)throw std::runtime_error("Parking encoder stopped accepting frames");
                    ++pushed;if(guard)guard->submit(frame,video_pts);}}
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
            double now=qv::now_seconds();if(now-last>=2){auto s=receiver.stats();std::cout<<"PARK rx="<<s.complete<<" encoded="<<pushed<<" dropped_raw="<<dropped<<" invalid="<<s.invalid<<" missing="<<s.missing<<std::endl;last=now;}
        }
        receiver.stop();gst_object_unref(bus);gst_app_src_end_of_stream(GST_APP_SRC(input));
        gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(input);gst_object_unref(pipeline);return 0;
    }catch(const std::exception& e){std::cerr<<"PARK ERROR: "<<e.what()<<std::endl;if(pipeline)gst_element_set_state(pipeline,GST_STATE_NULL);if(input)gst_object_unref(input);if(pipeline)gst_object_unref(pipeline);return 1;}
}
