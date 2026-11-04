#include <functional>
#include "rk_dma.hpp"
#include "input_source.hpp"
#include "ai_worker.hpp"
#include "light_rgb_runtime.hpp"
#include "ai_schedule.hpp"
#include "lane_runtime.hpp"
#include "visual.hpp"
#include "signal_control.hpp"
#include "scene_playlist.hpp"
#include "signal_mouse.hpp"
#include "native_toolbar.hpp"
#include "audio_alert.hpp"
#include "top_dashboard.hpp"
#include "surround_view.hpp"
#include "surround_info.hpp"
#include "product_io.hpp"
#include "x11_preview.hpp"
#include <gst/app/gstappsrc.h>
#include <gst/video/videooverlay.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <set>
#include <array>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <ctime>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stop_requested=0;
void signal_stop(int){stop_requested=1;}
constexpr int W=1280,H=720,TW=640,TH=360,NV12=RK_FORMAT_YCbCr_420_SP;
const char* names[]={"HDMI IN1","HDMI IN2","HDMI IN3","CAM1"};
struct Options {
    std::string heap="/dev/dma_heap/system-dma32",bind_a="192.168.10.1",bind_b="192.168.20.1";
    std::string peer_a="192.168.10.2",peer_b="192.168.20.2";
    std::array<bool,4> channels{{true,true,true,true}};
    std::array<bool,4> ai_channels{{true,true,true,true}};
    std::string model_dir="/userdata/auto_drive/rknn/v1",model,output_root="results",ai_mode="separate",probe;
    std::array<std::string,4> files{};
    double file_fps=30;unsigned file_max_mib=256;bool file_loop=true;
    double ai_fps=0;float confidence=.25f,iou=.45f;
    unsigned max_age_ms=500,warmup=2;
    unsigned fps=30,batch_us=300,seconds=60;
    unsigned cv_threads=0; // 0 preserves the OpenCV library default.
    bool check=false,self_test=false,no_ai=false,headless=false,diagnose=false,parity=false,lane_parity_only=false,lane_enabled=true;
    ai::LaneOptions lane;
    ai::TrackingOptions tracking;
    ai::LiveOptions live;
    std::string signal_config,playlist_path,product_dir,risk_profile;unsigned scene_index=0;std::vector<scene_player::Scene> scenes;
    bool content_refresh=true,surround_view=true,view_batch=true;
    bool sparse_lane_overlay=true,practical_signal=true,alert_sound=true;
    std::string preview_sink="rk";
    unsigned signal_stop_hold_ms=0,signal_box_hold_ms=0;
    bool lane_rules=false;
    unsigned lane_position_hold_ms=1000;
};
unsigned number(const std::string& s,unsigned max){
    size_t end=0;auto n=std::stoul(s,&end);
    if(end!=s.size() || s.empty() || s[0]=='-' || n>max)throw std::runtime_error("invalid number: "+s);
    return unsigned(n);
}
Options parse(int argc,char**argv){
    Options o;
    for(int i=1;i<argc;++i){std::string a=argv[i];
        auto value=[&](){if(++i>=argc)throw std::runtime_error("missing value: "+a);return std::string(argv[i]);};
        if(a=="--file0" || a=="--file1" || a=="--file2" || a=="--file3")o.files[number(a.substr(6),3)]=value();
        else if(a=="--file-fps"){auto raw=value();size_t end=0;o.file_fps=std::stod(raw,&end);if(end!=raw.size()||!std::isfinite(o.file_fps)||o.file_fps<1||o.file_fps>120)throw std::runtime_error("file-fps must be in [1,120]");}
        else if(a=="--file-max-mib")o.file_max_mib=number(value(),8192);
        else if(a=="--file-loop"){auto v=value();if(v!="on"&&v!="off")throw std::runtime_error("file-loop must be on/off");o.file_loop=v=="on";}
        else if(a=="--heap")o.heap=value();
        else if(a=="--bind-a")o.bind_a=value();else if(a=="--bind-b")o.bind_b=value();
        else if(a=="--peer-a")o.peer_a=value();else if(a=="--peer-b")o.peer_b=value();
        else if(a=="--seconds")o.seconds=number(value(),86400);
        else if(a=="--alert-sound"){auto v=value();if(v!="on"&&v!="off")throw std::runtime_error("alert-sound must be on/off");o.alert_sound=v=="on";}
        else if(a=="--preview-sink"){o.preview_sink=value();if(o.preview_sink!="rk"&&o.preview_sink!="x11"&&o.preview_sink!="gl"&&o.preview_sink!="xv")throw std::runtime_error("preview-sink must be rk/x11/gl/xv");}
        else if(a=="--preview-fps"){o.fps=number(value(),60);if(!o.fps)throw std::runtime_error("fps must be positive");}
        else if(a=="--rx-batch-us")o.batch_us=number(value(),1000);
        else if(a=="--cv-threads")o.cv_threads=number(value(),4);
        else if(a=="--check")o.check=true;else if(a=="--self-test")o.self_test=true;
        else if(a=="--no-ai")o.no_ai=true;else if(a=="--headless")o.headless=true;
        else if(a=="--model-dir")o.model_dir=value();else if(a=="--model")o.model=value();
        else if(a=="--output-root")o.output_root=value();
        else if(a=="--product-dir")o.product_dir=value();
        else if(a=="--risk-profile")o.risk_profile=value();
        else if(a=="--diagnose")o.diagnose=true;
        else if(a=="--probe-image"){o.probe=value();o.check=true;}
        else if(a=="--ai-mode"){o.ai_mode=value();if(o.ai_mode!="separate" && o.ai_mode!="mosaic" && o.ai_mode!="mosaic4")throw std::runtime_error("ai-mode must be separate, mosaic or mosaic4");}
        else if(a=="--front-model")o.live.front_model=value();
        else if(a=="--light-rgb-model"){auto v=value();o.live.light_rgb_model=v=="none"?"":v;}
        else if(a=="--light-rgb-max"){o.live.light_rgb_max=number(value(),16);if(!o.live.light_rgb_max)throw std::runtime_error("RGB budget must be 1..16");}

        else if(a=="--tensor-output"){auto v=value();if(v!="float"&&v!="native")throw std::runtime_error("tensor-output must be float/native");o.live.native_output=o.lane.native_output=v=="native";}
        else if(a=="--lane-hybrid"){auto v=value();if(v!="on"&&v!="off")throw std::runtime_error("lane-hybrid must be on/off");o.live.lane_hybrid=v=="on";}
        else if(a=="--lane-deadline-ms")o.live.lane_deadline_ms=number(value(),2000);
        else if(a=="--hybrid-semantic-ms")o.live.hybrid_semantic_ms=number(value(),2000);
        else if(a=="--lane-post"){auto v=value();if(v!="sync"&&v!="async")throw std::runtime_error("lane-post must be sync/async");o.live.lane_post_async=v=="async";}
        else if(a=="--lane"){auto v=value();if(v!="on"&&v!="off")throw std::runtime_error("lane must be on/off");o.lane_enabled=v=="on";}
        else if(a=="--check-lane-parity"){o.lane_parity_only=true;o.check=true;}
        else if(a=="--parity-check"){o.parity=true;o.check=true;}
        else if(a=="--max-age-ms")o.max_age_ms=number(value(),10000);
        else if(a=="--warmup")o.warmup=number(value(),20);
        else if(a=="--ai-fps")o.ai_fps=ai::parse_fps(value());
        else if(a=="--front-channel")o.live.front=number(value(),3);
        else if(a=="--light-candidates")o.live.light_candidates=true;
        else if(a=="--playlist")o.playlist_path=value();
        else if(a=="--scene-index")o.scene_index=number(value(),10000);
        else if(a=="--signal-config")o.signal_config=value();
        else if(a=="--signal-association"){auto v=value();if(v!="arrow"&&v!="practical")throw std::runtime_error("signal-association must be arrow/practical");o.practical_signal=v=="practical";}
        else if(a=="--lane-overlay"){auto v=value();if(v!="dense"&&v!="sparse")throw std::runtime_error("lane-overlay must be dense/sparse");o.sparse_lane_overlay=v=="sparse";}
        else if(a=="--signal-shape-kernel"){auto v=value();if(v!="dense"&&v!="packed")throw std::runtime_error("signal-shape-kernel must be dense/packed");o.live.packed_signal_shape=v=="packed";}
        else if(a=="--signal-mode"){auto v=value();if(v!="manual"&&v!="auto")throw std::runtime_error("signal-mode must be manual/auto");o.live.automatic_signal=v=="auto";}
        else if(a=="--signal-stop-hold-ms")o.signal_stop_hold_ms=number(value(),2000);
        else if(a=="--signal-box-hold-ms")o.signal_box_hold_ms=number(value(),1000);
        else if(a=="--lane-rules")o.lane_rules=true;
        else if(a=="--lane-position-hold-ms")o.lane_position_hold_ms=number(value(),1500);
        else if(a=="--display-refresh"){auto v=value();if(v!="baseline"&&v!="content")throw std::runtime_error("display-refresh must be baseline/content");o.content_refresh=v=="content";}
        else if(a=="--view-layout"){auto v=value();if(v!="grid"&&v!="surround")throw std::runtime_error("view-layout must be grid/surround");o.surround_view=v=="surround";}
        else if(a=="--view-submit"){auto v=value();if(v!="batch"&&v!="serial")throw std::runtime_error("view-submit must be batch/serial");o.view_batch=v=="batch";}
        else if(a=="--save-every")o.live.save_every=number(value(),100000);
        else if(a=="--lane-config")o.lane.camera=value();
        else if(a=="--lane-occlusion"){auto v=value();if(v!="on"&&v!="off")throw std::runtime_error("lane-occlusion must be on/off");o.lane.occlusion=v=="on";}
        else if(a=="--shallow-weight" || a=="--deep-weight" || a=="--other-weight"){
            auto raw=value();size_t end=0;double v=std::stod(raw,&end);if(end!=raw.size()||!std::isfinite(v)||v<.1||v>100)throw std::runtime_error("weight must be in [.1,100]");
            if(a=="--shallow-weight")o.live.shallow_weight=v;else if(a=="--deep-weight")o.live.deep_weight=v;else o.live.other_weight=v;
        }
        else if(a=="--lane-model")o.lane.model=value();
        else if(a=="--lane-fps")o.lane.fps=ai::parse_fps(value());
        else if(a=="--lane-max-age-ms")o.lane.max_age_ms=number(value(),10000);
        else if(a=="--tracking" || a=="--lane-geometry"){
            auto v=value();if(v!="on"&&v!="off")throw std::runtime_error(a+" must be on or off");
            if(a=="--tracking")o.tracking.enabled=v=="on";else o.lane.geometry=v=="on";
        }
        else if(a=="--ego-x"){
            auto raw=value();size_t end=0;double v=std::stod(raw,&end);
            if(end!=raw.size()||!std::isfinite(v)||v<=0||v>=1)throw std::runtime_error(a+" must be in (0,1)");
            o.lane.ego_x=float(v);
        }
        else if(a=="--conf" || a=="--light-conf" || a=="--iou" || a=="--lane-conf" || a=="--light-rgb-probability" || a=="--light-rgb-margin"){
            auto raw=value();size_t end=0;double v=std::stod(raw,&end);
            if(end!=raw.size() || !std::isfinite(v) || v<=0 || v>1)throw std::runtime_error("invalid value: "+a);
            if(a=="--light-conf"){if(v<.1)throw std::runtime_error("light-conf must be >=.1 (decoder candidate floor)");o.tracking.light_confidence=float(v);}
            else if(a=="--light-rgb-probability")o.live.light_rgb_probability=float(v);
            else if(a=="--light-rgb-margin")o.live.light_rgb_margin=float(v);
            else if(a=="--conf")o.confidence=float(v);else if(a=="--lane-conf")o.lane.confidence=float(v);else o.iou=float(v);
        }
        else if(a=="--channels" || a=="--ai-channels"){
            auto& selected=a=="--channels"?o.channels:a=="--ai-channels"?o.ai_channels:o.lane.channels;
            selected.fill(false);std::istringstream ss(value());std::string v;bool any=false;
            while(std::getline(ss,v,',')){selected[number(v,3)]=true;any=true;}
            if(!any)throw std::runtime_error("empty channels");
        }else if(a=="--help"){
            std::cout<<"rk_signal_control: BEST/DEADLINE plus manual signal association and simulated keyboard commands\n"
                "  --file0 PATH [--file1 PATH --file2 PATH --file3 PATH] (omitted channels use FPGA)\n"
                "  --playlist PATH --scene-index 0 (NEXT in same window, LIVE restores CH0 FPGA)\n"
                "  --file-fps 30 --file-loop on|off --file-max-mib 256 (aggregate local file size guard)\n"
                "  Files: packed 1280x720 RGB565 LE; convert MP4 on PC with prepare_video.py.\n"
                "  --front-channel 0 (0=IN1,1=IN2,2=IN3,3=CAM1)\n"
                "  --ai-mode separate|mosaic|mosaic4 (mosaic keeps front independent; mosaic4 all four)\n"
                "  --ai-fps 0 --lane-fps 0 (no fixed recognition rate caps)\n"
                "  --shallow-weight 4 --deep-weight 3 --other-weight 1 (service-time shares)\n"
                "  --model models/yolov8n_bdd8_i8.rknn\n"
                "  --front-model models/yolov8n_bdd8_640x384_i8.rknn (optional, incompatible with mosaic4)\n"
                "  --tensor-output float|native --lane-post sync|async --lane on|off --parity-check\n"
                "  --lane-model models/small4_pad1_i8.rknn --lane-config camera_live.json\n"
                "  --lane-hybrid on|off --lane-deadline-ms 0|600 --hybrid-semantic-ms 750\n"
                "  CPU propagation does not count as new model semantics; position is camera-axis relative.\n"
                "  --lane-occlusion on|off --lane-geometry on|off --ego-x .5\n"
                "  --tracking on|off (full ByteTrack + variable source-time Kalman adapter)\n"
                "  --channels 0,1,2,3 --ai-channels 0,1,2,3 --seconds 60 (0=until stopped)\n"
                "  --conf .25 --iou .45 --lane-conf .5 --max-age-ms 500 --lane-max-age-ms 750\n"
                "  --light-conf .15 (only traffic_light display/tracking/candidate threshold; [.1,1])\n"
                "  --check --headless --probe-image samples/reference_rgb640.bin\n"
                "  --check-lane-parity (lane-only gate; skips detector parity)\n"
                "  --diagnose (first real detector/lane inputs and masks)\n"
                "  --save-every 0 (off) or N (each Nth per-channel detection / front deep frame)\n"
                "  --light-candidates (front ROI colors, no ego-signal association/control)\n"
                "  --light-rgb-model models/light_rgb_128_i8_kl256.rknn (none disables); INT8 three-color ROI + pixel consensus\n"
                "  --light-rgb-probability 0.6 --light-rgb-margin 0.15 --light-rgb-max 4 (1..16 fresh ROIs/frame)\n"
                "  --signal-config configs/red_arrow.json (manual direction/ROI mapping, SIMULATION ONLY)\n"
                "  --signal-mode manual|auto (auto uses same-frame YOLO ROI + color + arrow-group candidates, not verified ego-lane relevance)\n"
                "  --signal-association practical|arrow (default practical: stable layout fallback for simulation advisory)\n"
                "  --lane-overlay sparse|dense (default sparse: exact cached nonzero mask blending)\n"
                "  --signal-shape-kernel packed|dense (default packed: exact binary IoU matching)\n"
                "  --signal-stop-hold-ms 0..2000 (confirmed stop decision grace; never extends GREEN)\n"
                "  --signal-box-hold-ms 0..1000 (visual-only dashed cache, not fresh color evidence)\n"
                "  --lane-position-hold-ms 0..1500 (display only; default 1000)\n"
                "  --lane-rules (combined signal/lane rules; J/M lane requests, L cancels; no automatic blind-spot clearance)\n"
                "  --preview-sink rk|x11|gl|xv (rk=original hardware; x11=CPU diagnostic window, no rkximagesink)\n"
                "  --display-refresh content|baseline (same video pixels; content skips redundant full-panel clears)\n"
                "  --view-layout surround|grid (display only; default surround, not a calibrated bird's-eye view)\n"
                "  --view-submit batch|serial (default batch with logged serial RGA fallback if unsupported)\n"
                "  --product-dir PATH --risk-profile PATH (optional LAN product bridge and calibrated side/rear risks)\n"
                "  Mouse toolbar: road left/straight/right, lane left/right, cancel lane, recheck, pause, next, live, exit.\n"
                "  --alert-sound on|off (default on): asynchronous two-beep violation cue, no normal red-light beep; audio device required.\n"
                "  Keys: A/W/D road direction, J/M lane request, L cancel, C recheck, P pause/resume, N next, V live, Q/Esc exit; SPACE/S are manual test controls. Drag ROI retained.\n"
                "  --no-ai --self-test --preview-fps 30 --rx-batch-us 300\n"
                "  --cv-threads 0..4 (0=library default; CPU parallelism, not an inference-rate cap)\n"
                "  --bind-a 192.168.10.1 --peer-a 192.168.10.2\n"
                "  --bind-b 192.168.20.1 --peer-b 192.168.20.2\n"
                "  --heap /dev/dma_heap/system-dma32 --output-root results\n"
                "QV02 RGB565 LE, R2 1440-byte or Jumbo 8192-byte payloads, UDP5000..5003\n";
            std::exit(0);
        }else throw std::runtime_error("unknown argument: "+a);
    }
    if(!o.playlist_path.empty()){
        if(o.live.front!=0 || !o.channels[0] || o.check || o.self_test)throw std::runtime_error("Playlist requires active front CH0 and normal run");
        o.scenes=scene_player::load(o.playlist_path,o.file_max_mib);
        if(o.scene_index>=o.scenes.size())throw std::runtime_error("Scene index outside playlist");
        const auto& s=o.scenes[o.scene_index];o.files[0]=s.file;o.file_fps=s.fps;o.signal_config=s.config;
    }
    uint64_t file_bytes=0;
    for(unsigned k=0;k<4;++k)if(!o.files[k].empty()){
        if(!o.channels[k]||o.self_test||o.check)throw std::runtime_error("File input requires active channel and normal run, not self-test/check");
        file_bytes+=qv::MemoryVideo::validated_size(o.files[k]);
    }
    if(file_bytes>uint64_t(o.file_max_mib)*1024*1024)throw std::runtime_error("Local clips exceed aggregate --file-max-mib; large clips use mapped disk input, check storage and I/O before raising this guard");
    o.live.full_mosaic=o.ai_mode=="mosaic4";
    if(o.live.lane_deadline_ms && o.live.lane_deadline_ms<100)throw std::runtime_error("lane deadline must be 0 or >=100ms");
    if(o.live.lane_hybrid && (!o.lane_enabled||o.lane.model.empty()||!o.live.lane_post_async||!o.lane.geometry||!o.live.lane_deadline_ms||
       o.live.hybrid_semantic_ms<=o.live.lane_deadline_ms||o.live.hybrid_semantic_ms>o.lane.max_age_ms))
        throw std::runtime_error("Hybrid requires lane/geometry, async post, positive deadline < semantic TTL <= lane-max-age-ms");
    if(o.live.full_mosaic && !o.live.front_model.empty())throw std::runtime_error("mosaic4 requires square model only; omit front-model");
    if(!o.lane_enabled)o.lane.model.clear();
    if(!o.signal_config.empty() && (o.check||o.self_test))throw std::runtime_error("Signal simulation requires normal input, not startup/color-bar checks");
    if(!o.live.light_rgb_model.empty()&&!o.live.automatic_signal&&!o.check)throw std::runtime_error("RGB model requires automatic signal mode");
    if(o.live.automatic_signal&&(o.signal_config.empty()||o.no_ai||!o.tracking.enabled))throw std::runtime_error("Automatic signal ROI requires signal-config, AI and tracking");
    if((o.signal_stop_hold_ms||o.signal_box_hold_ms)&&o.signal_config.empty())throw std::runtime_error("Signal hold options require signal-config");
    if(o.lane_position_hold_ms>1500)throw std::runtime_error("Lane position hold must be 0..1500ms");
    if(o.lane_rules&&(o.signal_config.empty()||o.no_ai||o.lane.model.empty()))throw std::runtime_error("Lane rules require signal-config, AI and lane model");
    if(o.lane_parity_only && (o.no_ai || o.lane.model.empty()))throw std::runtime_error("check-lane-parity requires enabled lane model");
    if(o.ai_fps!=0 || o.lane.fps!=0)throw std::runtime_error("v9 unlimited scheduling requires ai-fps=0 and lane-fps=0");
    o.lane.channels.fill(false);o.lane.channels[o.live.front]=true;
    if(!o.no_ai && (!o.channels[o.live.front] || !o.ai_channels[o.live.front]))throw std::runtime_error("Front channel must be active for recognition");
    if(o.self_test && !o.no_ai)throw std::runtime_error("Use --self-test --no-ai for display-only color bars; they do not validate detection accuracy");
    if(!o.max_age_ms)throw std::runtime_error("max-age-ms must be positive");
    if(o.no_ai && !o.probe.empty())throw std::runtime_error("probe-image requires AI");
    if(!o.lane.max_age_ms)throw std::runtime_error("lane-max-age-ms must be positive");
    if(!o.lane.model.empty()){
        if(o.no_ai)throw std::runtime_error("Lane mode requires AI; omit --no-ai");
        if(!std::filesystem::is_regular_file(o.lane.model))throw std::runtime_error("Lane model not found: "+o.lane.model);
        bool any=false;for(unsigned i=0;i<4;++i){o.lane.channels[i]=o.lane.channels[i] && o.channels[i];any|=o.lane.channels[i];}
        if(!any)throw std::runtime_error("No lane channels selected among active receivers");
    }
    bool selected=false;
    for(unsigned i=0;i<4;++i){o.ai_channels[i]=o.ai_channels[i] && o.channels[i];selected=selected || o.ai_channels[i];}
    if(!o.no_ai && !selected)throw std::runtime_error("No AI channels selected among active receivers");
    return o;
}
std::shared_ptr<qv::Frame> test_frame(const Options&o,unsigned channel){
    auto storage=std::make_shared<qv::DmaBuffer>(o.heap,qv::FrameBytes);
    auto frame=std::make_shared<qv::Frame>(storage);
    constexpr uint16_t colors[]={0xffff,0xffe0,0x07ff,0x07e0,0xf81f,0xf800,0x001f,0};
    storage->begin_write();
    for(int y=0;y<H;++y)for(int x=0;x<W;++x){auto word=colors[(x/160+channel)%8];
        frame->pixels[(y*W+x)*2]=uint8_t(word);frame->pixels[(y*W+x)*2+1]=uint8_t(word>>8);
    }storage->finish_write();return frame;
}
struct Preview {
    driver_demo::TopDashboard explanations;std::string model_label;std::unique_ptr<audio_alert::Cue> alert_sound;
    surround_ui::InfoPanels surround_info;bool front_file_active=false;
    std::array<double,4> ai_rates{};double lane_rate=0,machine_cpu=0,display_rate=0;
    std::function<void(char)> playback;bool paused=false;std::string playback_detail;bool gl_backend=false;std::atomic<uint64_t> gl_presented{0};GstClockTime gl_last_pts=GST_CLOCK_TIME_NONE;
    std::function<void()> park_request;
    std::function<void()> panorama_request;
    Display* display=nullptr;Window window=0;Atom close_atom=0;
    GstElement* pipeline=nullptr;GstElement* source=nullptr;GstElement* sink=nullptr;GstBus* bus=nullptr;
    signal_demo::Control* control=nullptr;unsigned control_channel=0;bool surround_view=true,front_priority=false,layout_ready=true;unsigned layout_revision=0;
    signal_demo::MouseToolbar mouse;
    std::set<KeySym> pressed;bool dragging=false,initial_position_applied=false;int drag_x=0,drag_y=0;
    ~Preview(){
        alert_sound.reset();
        surround_info.close();
        explanations.close();
        software.reset();
        if(bus)gst_bus_set_sync_handler(bus,nullptr,nullptr,nullptr);
        if(pipeline)gst_element_set_state(pipeline,GST_STATE_NULL);
        if(source)gst_object_unref(source);
        if(sink)gst_object_unref(sink);
        if(bus)gst_object_unref(bus);
        if(pipeline)gst_object_unref(pipeline);
        mouse.close();
        if(window)XDestroyWindow(display,window);
        if(display)XCloseDisplay(display);
    }
    std::unique_ptr<ui::X11Preview> software;uint64_t software_presented=0;
    void start(unsigned fps,const std::string& mode){
        if(mode=="gl"||mode=="xv")XInitThreads();
        display=XOpenDisplay(nullptr);
        if(!display)throw std::runtime_error("Cannot open X11 DISPLAY. Run in the RK3568 desktop terminal (normally DISPLAY=:0).");
        int screen=DefaultScreen(display),dw=DisplayWidth(display,screen),dh=DisplayHeight(display,screen);
        int ww=std::min(1600,dw),hh=std::min(1020,std::max(444,dh-50));
        window=XCreateSimpleWindow(display,RootWindow(display,screen),0,0,ww,hh,0,0,0);
        const char* variant=std::getenv("RK_MODEL_LABEL");
        std::string title="RK SIGNAL CONTROL | ";
        title+=variant?variant:"DIRECT MODEL CONFIG";
        model_label=variant?variant:"DIRECT MODEL CONFIG";
        title+=surround_view?" | FOUR VIEWS AROUND EGO | SIMULATION ONLY":" | FOUR VIEW GRID | SIMULATION ONLY";
        XStoreName(display,window,title.c_str());
        XSelectInput(display,window,StructureNotifyMask|ExposureMask|KeyPressMask|KeyReleaseMask|ButtonPressMask|ButtonReleaseMask|FocusChangeMask);
        mouse.start(display,window,ww,hh);
        if(mode=="rk")surround_ui::prepare_rk_video_background(display,mouse.video);
        explanations.start(display,window);
        surround_info.start(display,window,mouse.video);
        Bool repeat_supported=False;XkbSetDetectableAutoRepeat(display,True,&repeat_supported);
        close_atom=XInternAtom(display,"WM_DELETE_WINDOW",False);XSetWMProtocols(display,window,&close_atom,1);
        XMapRaised(display,window);XSync(display,False);
        if(mode=="x11"){software=std::make_unique<ui::X11Preview>(display,mouse.video);std::cout<<"Preview backend=X11 software window; CPU NV12 conversion/copy, no rkximagesink or DRM overlay; not a hardware-display performance baseline\n";return;}
        gl_backend=mode=="gl";
        std::cout<<"Preview backend="<<(gl_backend?"OpenGL EGL/GLES window, NV12 DMA input":mode=="xv"?"XVideo NV12 window, GLAMOR driver conversion/scaling":"rkximagesink DMA")<<"; actual import/visibility requires board verification\n";
        GError* error=nullptr;
        std::string spec="appsrc name=input ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! ";
        spec+=(gl_backend?"glimagesink":mode=="xv"?"xvimagesink":"rkximagesink");spec+=" name=preview sync=false enable-last-sample=false";
        pipeline=gst_parse_launch(spec.c_str(),&error);
        if(error){std::string msg=error->message;g_error_free(error);throw std::runtime_error(msg);}
        if(!pipeline)throw std::runtime_error("Cannot create preview pipeline");
        source=gst_bin_get_by_name(GST_BIN(pipeline),"input");sink=gst_bin_get_by_name(GST_BIN(pipeline),"preview");
        g_object_set(source,"is-live",TRUE,"format",GST_FORMAT_TIME,"block",FALSE,"emit-signals",FALSE,
                     "max-bytes",guint64(W*H*3),nullptr);
        auto caps=gst_caps_new_simple("video/x-raw","format",G_TYPE_STRING,"NV12","width",G_TYPE_INT,W,
              "height",G_TYPE_INT,H,"framerate",GST_TYPE_FRACTION,int(fps),1,"pixel-aspect-ratio",GST_TYPE_FRACTION,1,1,"colorimetry",G_TYPE_STRING,"bt601",nullptr);
        if(gl_backend)gst_caps_set_features(caps,0,gst_caps_features_new("memory:DMABuf",nullptr));
        gst_app_src_set_caps(GST_APP_SRC(source),caps);gst_caps_unref(caps);
        if(gl_backend)g_signal_connect(sink,"client-draw",G_CALLBACK(+[](GstElement*,gpointer,GstSample* sample,gpointer user)->gboolean{
            auto self=static_cast<Preview*>(user);auto buffer=sample?gst_sample_get_buffer(sample):nullptr;
            if(buffer&&GST_BUFFER_PTS_IS_VALID(buffer)&&GST_BUFFER_PTS(buffer)!=self->gl_last_pts){self->gl_last_pts=GST_BUFFER_PTS(buffer);++self->gl_presented;}
            return FALSE; // Let the default shader draw; count unique PTS, not expose redraws.
        }),this);
        bus=gst_element_get_bus(pipeline);
        gst_bus_set_sync_handler(bus,[](GstBus*,GstMessage*m,gpointer user){
            auto self=static_cast<Preview*>(user);
            if(gst_is_video_overlay_prepare_window_handle_message(m)){
                gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(m)),self->mouse.video);
                gst_message_unref(m);return GST_BUS_DROP;
            }return GST_BUS_PASS;
        },this,nullptr);
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(sink),mouse.video);
        gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(sink),FALSE);
        gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(sink),0,0,mouse.layout.vw,mouse.layout.vh);
        if(gst_element_set_state(pipeline,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE)throw std::runtime_error("Preview could not enter PLAYING");
    }
    void choose_layout(){
        if(!surround_view)return;
        front_priority=!front_priority;++layout_revision;layout_ready=false;dragging=false;mouse.cancel();
        explanations.set_layout(front_priority);surround_info.set_layout(front_priority);
        std::cout<<"View arrangement="<<(front_priority?"front":"original")<<std::endl;
    }
    void dispatch(char command,const char* origin="界面"){
        if(command=='!'){if(alert_sound)alert_sound->toggle();return;}
        if(command=='~'){choose_layout();return;}
        if(command=='u'){if(control)control->record_operation(command,qv::now_seconds(),origin);if(panorama_request)panorama_request();else std::cerr<<"360 requires start_product.sh / product service\n";return;}
        if(command=='k'){if(control)control->record_operation(command,qv::now_seconds(),origin);if(park_request)park_request();else std::cerr<<"PARK requires start_product.sh / product service\n";return;}
        if((command=='p'||command=='n'||command=='v')&&playback){playback(command);if(control)control->record_operation(command,qv::now_seconds(),origin);return;}
        if(!control)return;
        const double stamp=qv::now_seconds();control->key(command,stamp,origin);
    }
    bool events(){
        while(XPending(display)){XEvent e;XNextEvent(display,&e);
            char shortcut=surround_info.event(e);if(shortcut)dispatch(shortcut);
            char lane_click=explanations.event(e);
            if(lane_click=='u'||lane_click=='~'||lane_click=='!')dispatch(lane_click);
            else if(lane_click&&control)dispatch(lane_click);
            if(e.type==ClientMessage && Atom(e.xclient.data.l[0])==close_atom)return false;
            if(e.type==DestroyNotify)return false;
            if(e.type==MapNotify&&e.xmap.window==window&&!initial_position_applied){
                initial_position_applied=true;
                XMoveWindow(display,window,0,0);
                XEvent move{};move.xclient.type=ClientMessage;move.xclient.display=display;move.xclient.window=window;
                move.xclient.message_type=XInternAtom(display,"_NET_MOVERESIZE_WINDOW",False);move.xclient.format=32;
                move.xclient.data.l[0]=NorthWestGravity|(1<<8)|(1<<9)|(2<<12); // x/y only, user request.
                XSendEvent(display,DefaultRootWindow(display),False,SubstructureRedirectMask|SubstructureNotifyMask,&move);
                XFlush(display); // Once at startup; later manual dragging remains available.
            }
            if(e.type==ConfigureNotify&&e.xconfigure.window==window)dragging=false;
            if(e.type==FocusOut&&e.xfocus.detail!=NotifyInferior){pressed.clear();dragging=false;mouse.cancel();if(control)control->focus_lost(qv::now_seconds());}
            char clicked=mouse.event(e);
            if(sink&&e.type==ConfigureNotify&&e.xconfigure.window==window){
                // Sink event handling is off so it does not steal our mouse input.
                // Explicitly keep its target rectangle in sync with the resized child.
                XSync(display,False);
                gst_video_overlay_set_render_rectangle(GST_VIDEO_OVERLAY(sink),0,0,mouse.layout.vw,mouse.layout.vh);
                gst_video_overlay_expose(GST_VIDEO_OVERLAY(sink));
            }
            if(clicked){dragging=false;if(clicked=='q')return false;
                dispatch(clicked);}
            if(e.type==KeyRelease)pressed.erase(XLookupKeysym(&e.xkey,0));
            if(e.type==KeyPress){auto key=XLookupKeysym(&e.xkey,0);
                if(!pressed.insert(key).second)continue;
                if(key==XK_Tab){choose_layout();continue;}
                if(key==XK_q||key==XK_Q||key==XK_Escape)return false;
                if(control){char command=key==XK_space?' ':key>=XK_a&&key<=XK_z?char(key):key>=XK_A&&key<=XK_Z?char(key-XK_A+'a'):0;
                    if(command)dispatch(command,"键盘");}
            }
            if(control && !control->automatic() && (e.type==ButtonPress||e.type==ButtonRelease) && e.xbutton.window==mouse.video && e.xbutton.button==Button1){
                if(surround_view&&!layout_ready){dragging=false;continue;}
                int x=e.xbutton.x*W/std::max(1,mouse.layout.vw),y=e.xbutton.y*H/std::max(1,mouse.layout.vh);
                if(surround_view){unsigned channel=0;int cx=0,cy=0;
                    if(!surround_ui::Layout{control_channel,front_priority}.canonical(x,y,channel,cx,cy)||channel!=control_channel){dragging=false;continue;}
                    x=cx;y=cy;
                }
                int ox=int(control_channel%2)*640+ui::ContentX,oy=int(control_channel/2)*360+ui::ContentY;
                bool inside=x>=ox&&x<ox+ui::ContentW&&y>=oy&&y<oy+ui::ContentH;
                if(e.type==ButtonPress){dragging=inside;drag_x=x;drag_y=y;}
                else {if(dragging&&inside)control->set_roi((drag_x-ox)/.45f,(drag_y-oy)/.45f,(x-ox)/.45f,(y-oy)/.45f,qv::now_seconds());dragging=false;}
            }
        }
        while(bus){auto m=gst_bus_pop(bus);if(!m)break;
            if(GST_MESSAGE_TYPE(m)==GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(m)==GST_MESSAGE_WARNING){
                bool fatal=GST_MESSAGE_TYPE(m)==GST_MESSAGE_ERROR;GError* e=nullptr;gchar* debug=nullptr;
                if(fatal)gst_message_parse_error(m,&e,&debug);else gst_message_parse_warning(m,&e,&debug);
                std::string msg=e?e->message:"GStreamer error";if(debug)msg+=" | "+std::string(debug);
                if(e)g_error_free(e);
                g_free(debug);gst_message_unref(m);
                if(fatal)throw std::runtime_error(msg);
                std::cerr<<"GST warning: "<<msg<<"\n";
            }else gst_message_unref(m);
        }return true;
    }
    void paint_controls(const driver_demo::Explanation& explanation){
        if(alert_sound)alert_sound->observe(explanation.audible_alarm,qv::now_seconds());
        mouse.draw(control?int(control->selected()):-1,control&&control->automatic(),explanation.vehicle_label,
            explanation.lane_requested?explanation.lane_request:explanation.vehicle_reason,
            control&&control->has_lane_rules(),paused,explanation.ui_alarm,explanation.lane_requested?explanation.lane_intent:-1);
        explanations.paint(mouse.layout,explanation,model_label,ai_rates,lane_rate,machine_cpu,display_rate,alert_sound?alert_sound->ui_state():0);
        surround_info.paint(mouse.layout,ai_rates,front_file_active,paused,playback_detail,control_channel,surround_view,explanation);
    }
    uint64_t rendered(){
        if(gl_backend)return gl_presented.load(); // Unique PTS reaching GL draw, not physical scanout.
        if(software)return software_presented; // X11 submissions, not independently measured physical scanout.
        if(!g_object_class_find_property(G_OBJECT_GET_CLASS(sink),"stats"))return 0;
        GstStructure* st=nullptr;g_object_get(sink,"stats",&st,nullptr);guint64 n=0;
        if(st){gst_structure_get_uint64(st,"rendered",&n);gst_structure_free(st);}return n;
    }
    void push(std::shared_ptr<qv::DmaBuffer>b,unsigned fps){
        if(software){b->begin_read();try{software->draw(b->data(),W,H,mouse.layout.vw,mouse.layout.vh);}catch(...){b->finish_read();throw;}b->finish_read();if(!software_presented)std::cout<<"X11 transport="<<(software->shared_memory()?"MIT-SHM (sync before reuse; server/main work overlap)":"XPutImage fallback")<<"\n";++software_presented;layout_ready=true;return;}
        auto buffer=qv::wrap_nv12_dma(std::move(b),W,H);
        auto clock=gst_element_get_clock(pipeline);
        if(clock){GST_BUFFER_PTS(buffer)=gst_clock_get_time(clock)-gst_element_get_base_time(pipeline);gst_object_unref(clock);}
        GST_BUFFER_DURATION(buffer)=GST_SECOND/fps;
        auto flow=gst_app_src_push_buffer(GST_APP_SRC(source),buffer);
        if(flow!=GST_FLOW_OK)throw std::runtime_error("appsrc push failed: "+std::string(gst_flow_get_name(flow)));
        layout_ready=true;
    }
};
struct Cpu {uint64_t total=0,idle=0;};
Cpu cpu(){std::ifstream f("/proc/stat");std::string tag;uint64_t a=0,b=0,c=0,d=0,e=0,g=0,h=0,i=0;f>>tag>>a>>b>>c>>d>>e>>g>>h>>i;return {a+b+c+d+e+g+h+i,d+e};}
std::string sensor(const char* path){std::ifstream f(path);std::string value;if(f>>value)return value;return "";}
std::string rss_kb(){std::ifstream f("/proc/self/status");std::string line;while(std::getline(f,line))if(line.rfind("VmRSS:",0)==0){std::istringstream row(line.substr(6));std::string value;row>>value;return value;}return "";}
std::string create_output(const std::string& root){
    namespace fs=std::filesystem;fs::create_directories(root);
    std::time_t now=std::time(nullptr);std::tm local{};localtime_r(&now,&local);
    std::ostringstream name;name<<"run_"<<std::put_time(&local,"%Y%m%d_%H%M%S")<<'_'<<getpid();
    auto path=fs::absolute(fs::path(root)/name.str());
    if(!fs::create_directory(path))throw std::runtime_error("Output directory already exists: "+path.string());
    return path.string();
}
}
int main(int argc,char**argv){std::string output;try{
    Options o=parse(argc,argv);XInitThreads();gst_init(nullptr,nullptr);
    product::Bridge product_bridge(o.product_dir,o.risk_profile);double product_observed=-1;bool product_display_ready=false;
    product_bridge.front_source_changed(o.files[0].empty()?"FPGA_CH0":o.files[0]);
    if(o.cv_threads)cv::setNumThreads(int(o.cv_threads));
    output=create_output(o.output_root);std::cout<<"Results: "<<output<<"\n";
    if(!o.headless&&o.preview_sink=="rk"){auto factory=gst_element_factory_find("rkximagesink");
        if(!factory)throw std::runtime_error("Missing rkximagesink plugin; use --headless for model/network verification");
        gst_object_unref(factory);}
    int rgb_format=qv::verify_rga_rgb565(o.heap);
    std::string model=o.no_ai?"AI_DISABLED":ai::select_model(o.model_dir,o.model);
    std::cout<<"Selected model: "<<model<<"\n";
    std::ofstream config(output+"/config.txt");
    config<<"opencv_threads="<<cv::getNumThreads()<<"\n";
    config<<"alert_sound_requested="<<o.alert_sound<<"\nalert_sound_active_only_with_preview=true\n";
    config<<"model="<<model<<"\nchannels=";for(unsigned k=0;k<4;++k)if(o.channels[k])config<<k<<',';
    config<<"\nai_channels=";for(unsigned k=0;k<4;++k)if(o.ai_channels[k])config<<k<<',';
    config<<"\nai_mode="<<o.ai_mode<<"\nai_target_hz="<<o.ai_fps<<"\nai_target_unit="<<(o.ai_mode!="separate"?"composites_per_second":"per_channel_per_second")
          <<"\nai_rate_limit_disabled="<<(o.ai_fps==0)
          <<"\nconf="<<o.confidence<<"\nlight_conf_override="<<o.tracking.light_confidence<<"\niou="<<o.iou<<"\nmax_result_age_ms="<<o.max_age_ms
          <<"\ninference_feed=complete_frame_observer_independent_of_preview\nseconds="<<o.seconds<<"\nheadless="<<o.headless<<"\npreview_sink="<<o.preview_sink<<"\ndisplay_counter_semantics="<<(o.preview_sink=="x11"?"x11_submitted_not_physical_scanout":o.preview_sink=="gl"?"gl_unique_pts_draw_not_physical_scanout":"sink_reported_not_physical_scanout")<<"\nai_disabled="<<o.no_ai<<"\ndiagnose="<<o.diagnose<<"\nprobe="<<o.probe<<"\n"
          <<"RGB565 source=1280x720 little endian; side model RGB=640x640; front_height="<<(o.live.front_model.empty()?640:384)<<"; "
          <<(o.live.full_mosaic?"all-four content320x180/top70, fresh outputs only; cached quadrants expire":o.ai_mode=="mosaic"?"other3 content320x180/top70; independent front content640x360":"all independent content640x360")<<"\n"
          <<
            "Receive-to-result age begins at complete UDP assembly, not camera exposure. No ground truth / accuracy metrics.\n";
    config<<"lane_model="<<o.lane.model<<"\nlane_target_hz_per_channel="<<o.lane.fps<<"\nlane_conf="<<o.lane.confidence
          <<"\nlane_max_age_ms="<<o.lane.max_age_ms<<"\nlane_channels=";
    for(unsigned k=0;k<4;++k)if(!o.lane.model.empty() && o.lane.channels[k])config<<k<<',';
    config<<"\nlane_geometry=front single1280x720 to640x384 top12; never detection mosaic\n";
    config<<"front_channel="<<o.live.front<<"\nscheduler=measured_service_time_weighted_fair, latest-only\nshallow_weight="<<o.live.shallow_weight
        <<"\ndeep_weight="<<o.live.deep_weight<<"\nother_weight="<<o.live.other_weight<<"\nlane_camera="<<o.lane.camera<<"\nsame_frame_occlusion="<<o.lane.occlusion<<"\nsave_every_deep="<<o.live.save_every<<'\n';
    for(unsigned k=0;k<4;++k)config<<"input_source"<<k<<'='<<(o.files[k].empty()?"fpga_udp":qv::MemoryVideo::validated_size(o.files[k])>512ull*1024*1024?"mapped_rgb565":"preloaded_rgb565")<<"\ninput_file"<<k<<'='<<o.files[k]<<'\n';
    config<<"file_source_fps="<<o.file_fps<<"\nfile_loop="<<o.file_loop<<"\nfile_max_mib="<<o.file_max_mib<<"\nlocal_source_overhead_in_process_cpu=true\n";
    config<<"native_output="<<o.live.native_output<<"\nfront_model="<<o.live.front_model<<"\nfull_mosaic="<<o.live.full_mosaic
          <<"\nlane_post_async="<<o.live.lane_post_async<<"\nowned_lane_snapshot=true\n"
          <<"lane_hybrid="<<o.live.lane_hybrid<<"\nlane_deadline_ms="<<o.live.lane_deadline_ms
          <<"\nhybrid_semantic_ms="<<o.live.hybrid_semantic_ms<<"\nposition_reference=camera_axis_relative_not_metric\n";
    config<<"tracking_enabled="<<o.tracking.enabled<<"\ntracking_algorithm=ByteTrack_Kalman_LAPJV_variable_dt\nlegacy_unused_track_alpha="<<o.tracking.alpha
        <<"\nlane_geometry_enabled="<<o.lane.geometry<<"\nego_x_assumed="<<o.lane.ego_x<<"\n";
    config<<"light_rgb_model="<<o.live.light_rgb_model<<"\nlight_rgb_probability="<<o.live.light_rgb_probability<<"\nlight_rgb_margin="<<o.live.light_rgb_margin<<"\nlight_rgb_max="<<o.live.light_rgb_max<<"\nlight_rgb_mode=RGB_PIXEL_CONSENSUS\n";
    config<<"signal_association_policy="<<(o.practical_signal?"practical_layout_fallback":"arrow_only")<<"\nlane_overlay="<<(o.sparse_lane_overlay?"sparse_exact_cache":"dense_reference")<<'\n';
    config<<"signal_shape_kernel="<<(o.live.packed_signal_shape?"packed_binary_iou":"dense_cv_reference")<<'\n';
    config<<"view_layout="<<(o.surround_view?"surround":"grid")<<"\nview_is_calibrated_bev=false\nview_only_resize=true\n";
    config<<"view_submit_requested="<<(o.view_batch?"batch":"serial")<<"\nview_sprite_cpu_blends_per_frame=0\nview_extra_dma_canvas_bytes="<<(o.surround_view&&!o.headless?W*H*3/2:0)<<'\n';
    config<<"view_role_channels=";for(auto ch:surround_ui::Layout{o.live.front}.channels())config<<ch<<',';
    config<<"\nview_role_order=front,left,right,rear (verify physical wiring; names do not calibrate cameras)\n";
    config<<"signal_episode_exit_ms="<<signal_demo::SignalPresence::ExitMs<<"\nsignal_episode_min_absent_observations="<<signal_demo::SignalPresence::MinimumAbsentSamples
          <<"\nsignal_episode_max_frame_gap_ms="<<signal_demo::SignalPresence::MaximumFrameGapMs<<"\nsignal_episode_exit_implies_green=false\n";
    config<<"light_candidates="<<o.live.light_candidates<<"\nlight_control_enabled=false\n"
          <<"signal_simulation_enabled="<<!o.signal_config.empty()<<"\nsignal_config="<<o.signal_config
          <<"\nsignal_mode="<<(o.live.automatic_signal?"auto":"manual")<<"\nsignal_stop_hold_ms="<<o.signal_stop_hold_ms<<"\nsignal_box_hold_ms="<<o.signal_box_hold_ms<<"\nlane_rules="<<o.lane_rules<<"\nlane_position_hold_ms="<<o.lane_position_hold_ms<<"\nlane_prediction_horizon_ms=250\nlane_view_filter=kalman_display_only\nblind_spot_clearance_verified=false\ndisplay_refresh="<<(o.content_refresh?"content":"baseline")<<"\nsignal_association="<<(o.live.automatic_signal?"automatic_visible_arrow_group_not_ego_lane_verification":"manual_calibrated_roi_not_automatic_arrow_or_lane_inference")<<"\nactuator_control=false\n";
    std::unique_ptr<signal_demo::Control> signal;
    if(!o.signal_config.empty()){
        auto cfg=signal_demo::load(o.signal_config);
        cfg.automatic=o.live.automatic_signal;cfg.practical_association=o.practical_signal;
        cfg.settings.stop_hold_ms=o.signal_stop_hold_ms;cfg.box_hold_ms=o.signal_box_hold_ms;
        auto effective=signal_demo::effective_config(cfg);
        config<<"signal_min_samples="<<effective.settings.minimum_samples<<"\nsignal_confirm_green_ms="<<effective.settings.confirm_green_ms
              <<"\nsignal_confirm_red_ms="<<effective.settings.confirm_red_ms<<"\nsignal_max_gap_ms="<<effective.settings.max_gap_ms
              <<"\nsignal_max_age_ms="<<effective.settings.max_age_ms<<"\nsignal_missing_grace_ms="<<(o.practical_signal?300:0)<<'\n';
        cfg.lane_rules=o.lane_rules;cfg.lane_position_hold_ms=o.lane_position_hold_ms;
        if(o.files[o.live.front].empty())cfg.loop_frames=0;
        std::filesystem::copy_file(o.signal_config,output+"/signal_config_used.json");
        signal=std::make_unique<signal_demo::Control>(cfg,output);
    }
    if(!o.lane.model.empty())std::filesystem::copy_file(o.lane.camera,output+"/camera_used.json");
    if(o.check){if(!o.no_ai){int input_format=qv::verify_rga_rgb888(o.heap,rgb_format);
            if(o.ai_mode!="separate")qv::verify_rga_mosaic(o.heap,rgb_format,input_format);
            if(!o.lane_parity_only){
            ai::verify_model(model,output,o.warmup,o.probe,o.live.native_output,o.parity);
            if(!o.live.front_model.empty()){
                std::filesystem::create_directories(output+"/front_check");
                ai::verify_model(o.live.front_model,output+"/front_check",o.warmup,o.probe,o.live.native_output,o.parity);
            }
            }
            if(!o.live.light_rgb_model.empty()){
                ai::LightRGB rgb(o.live.light_rgb_model,output,o.live.light_rgb_probability,o.live.light_rgb_margin);
                std::vector<uint8_t> black(qv::FrameBytes,0);auto checked=rgb.infer(black.data(),ai::Box{100,100,164,164,1,7},"UNKNOWN");
                if(checked.color!="UNKNOWN")throw std::runtime_error("RGB dark-input consensus rejection failed");
                std::cout<<"PASS INT8 RGB model attributes and dark-input rejection; not road accuracy validation.\n";
            }
            if(!o.lane.model.empty())ai::verify_lane(o.lane,output,o.warmup,o.probe,o.parity || o.lane_parity_only);}
        std::cout<<"PASS: startup checks. UDP reception, live accuracy and desktop presentation remain untested.\n";return 0;}
    std::signal(SIGINT,signal_stop);std::signal(SIGTERM,signal_stop);
    std::array<std::unique_ptr<qv::InputSource>,4> receivers;
    std::array<std::string,4> input_names;
    std::array<std::shared_ptr<qv::Frame>,4> held;
    auto feed_enabled=std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<ai::Worker> worker;
    if(!o.no_ai){worker=std::make_shared<ai::Worker>(model,o.heap,rgb_format,o.ai_channels,o.ai_fps,o.confidence,o.iou,o.max_age_ms,output,o.warmup,o.ai_mode!="separate",o.diagnose,o.lane,o.tracking,o.live);worker->start();}
    auto make_frame=[heap=o.heap](){return std::make_unique<qv::Frame>(std::make_shared<qv::DmaBuffer>(heap,qv::FrameBytes));};
    for(unsigned i=0;i<4;++i){if(!o.channels[i])continue;
        if(o.self_test)held[i]=test_frame(o,i);
        else {qv::Endpoint ep{i<2?o.bind_a:o.bind_b,i<2?o.peer_a:o.peer_b,uint16_t(5000+i),i==3?15u:30u,names[i]};
            input_names[i]=o.files[i].empty()?names[i]:"FILE CH"+std::to_string(i);names[i]=input_names[i].c_str();
            receivers[i]=std::make_unique<qv::InputSource>(ep,o.files[i],o.file_fps,o.file_loop,make_frame,8,o.batch_us);
            if(worker)receivers[i]->observe([worker,feed_enabled,i](const std::shared_ptr<qv::Frame>& frame){if(feed_enabled->load())worker->publish(i,frame);});
            receivers[i]->start();}
    }
    std::shared_ptr<qv::DmaBuffer> black;
    std::array<std::shared_ptr<qv::DmaBuffer>,5> canvases;
    std::array<std::shared_ptr<qv::DmaBuffer>,5> view_canvases;
    std::array<unsigned,5> canvas_revisions{};unsigned packed_revision=0;
    bool view_batch_allowed=o.view_batch;
    std::array<im_rect,4> view_sources{},view_destinations{};
    for(unsigned role=0;role<4;++role){auto s=surround_ui::Layout{o.live.front}.source(role),d=surround_ui::Layout::tiles[role];
        view_sources[role]={s.x,s.y,s.w,s.h};view_destinations[role]={d.x,d.y,d.w,d.h};}
    std::unique_ptr<Preview> preview;
    bool playback_paused=false,next_video=false;char source_switch=0;double pause_started=0,paused_seconds=0;unsigned pause_count=0;
    std::ofstream playback_log(output+"/playback.csv");
    playback_log<<"now_s,event,paused,source_frame_id,received_s\n";
    if(!o.headless){black=std::make_shared<qv::DmaBuffer>(o.heap,TW*TH*3/2);qv::initialize_nv12(*black,TW,TH);
        for(unsigned i=0;i<canvases.size();++i){if(o.surround_view&&i)break;
            canvases[i]=std::make_shared<qv::DmaBuffer>(o.heap,W*H*3/2);qv::initialize_nv12(*canvases[i],W,H);}
        if(o.surround_view)for(auto& b:view_canvases){b=std::make_shared<qv::DmaBuffer>(o.heap,W*H*3/2);
            b->begin_write();surround_ui::initialize(b->data(),o.live.front);b->finish_write();}
        preview=std::make_unique<Preview>();preview->control=signal.get();preview->control_channel=o.live.front;preview->surround_view=o.surround_view;preview->front_file_active=!o.files[o.live.front].empty();preview->start(o.fps,o.preview_sink);
        preview->alert_sound=std::make_unique<audio_alert::Cue>(output,o.alert_sound);
        preview->playback_detail=o.scenes.empty()?"PAUSE: review | NEXT: test player | LIVE: FPGA":("SCENE "+o.scenes[o.scene_index].id+" | NEXT: next clip | LIVE: FPGA CH0");
        preview->park_request=[&](){product_bridge.request_park();};
        preview->panorama_request=[&](){product_bridge.request_panorama();};
        preview->playback=[&](char command){
            const double stamp=qv::now_seconds();bool have_file=false;for(const auto& r:receivers)have_file|=r&&r->local();
            if(command=='v'){if(!receivers[0] || !receivers[0]->local())return;source_switch='v';return;}
            if(command=='n'&&!o.scenes.empty()){source_switch='n';return;}
            if(command=='n'){if(signal)signal->log("NEXT_NO_PLAYLIST",stamp);return;}
            if(!have_file)return;
            if(signal)signal->log("PLAYBACK_PAUSE_OR_RESUME",stamp);
            if(command=='n'){next_video=true;stop_requested=true;}
            else {
                playback_paused=!playback_paused;preview->paused=playback_paused;
                feed_enabled->store(false);
                for(unsigned i=0;i<4;++i)if(receivers[i]){receivers[i]->pause(playback_paused,true,held[i]?held[i]->source:0,held[i]?held[i]->received:0);if(receivers[i]->local())receivers[i]->take();}
                if(worker){worker->stop();worker->reset_scene(o.live.front);}
                if(signal)signal->set_still_image(playback_paused,qv::now_seconds());
                if(worker)worker->start();
                feed_enabled->store(true);
                if(playback_paused){pause_started=stamp;++pause_count;}
                else paused_seconds+=stamp-pause_started;
            }
            const auto& f=held[o.live.front];
            playback_log<<std::fixed<<std::setprecision(6)<<stamp<<','<<(command=='n'?"NEXT":playback_paused?"PAUSE":"RESUME")<<','<<playback_paused<<','<<(f?f->id:0)<<','<<(f?f->received:0)<<'\n';playback_log.flush();
        };
    }
    std::cout<<"Display layout="<<(o.surround_view?"surround (front top, left/right, rear bottom; ego sprite illustrative only)":"grid (CH0/CH1 top, CH2/CH3 bottom)")<<"; inference unchanged.\n"
        <<"DMA source pool=8/channel, canvas pool=5, RX coalesce="<<o.batch_us<<"us; preview="<<o.fps<<"Hz\n"
        <<"Process CPU: 100%=one core; machine CPU: 100%=all cores.\n";
    std::cout<<"AI mode="<<o.ai_mode<<" target=";
    if(o.ai_fps==0)std::cout<<"UNLIMITED (new frames only)";else std::cout<<o.ai_fps<<(o.ai_mode!="separate"?" composites/s":" Hz/channel");
    std::cout<<(o.live.full_mosaic?"; all four content320x180; small-object recall needs validation.\n":o.ai_mode=="mosaic"?"; front640x360 plus other3 mosaic320x180.\n":"; independent content640x360.\n");
    if(!o.lane.model.empty())std::cout<<"Lane=ON, single full-view per inference, target="<<o.lane.fps
        <<" Hz/channel (0=unlimited); one shared serial NPU worker; front shallow/deep measured-time scheduling.\n";
    std::cout<<"Tracking="<<(o.tracking.enabled?"ON: ByteTrack Kalman/LAPJV, variable receive-time interval":"OFF")
        <<"; lane geometry="<<(o.lane.geometry?"ON: uncalibrated quadratic lane candidate":"OFF")<<"\n";
    feed_enabled->store(true);
    auto start=std::chrono::steady_clock::now(),next=start,last=start;auto oldcpu=cpu();auto oldclock=std::clock();
    uint64_t pushed=0,skipped=0,previous_rendered=preview?preview->rendered():0,previous_inferences=0;double rga_ms=0,header_ms=0,overlay_ms=0,control_ms=0,present_ms=0,convert_ms=0,submit_ms=0,pack_ms=0;uint64_t total_presented=0;double total_present_ms=0,total_pack_ms=0;
    std::ofstream metrics(output+"/metrics.csv");
    std::ofstream telemetry(output+"/telemetry.csv");
    if(!telemetry)throw std::runtime_error("Cannot open telemetry.csv");
    telemetry<<"elapsed_s,rss_kb,cpu_khz,npu_hz,ddr_hz,thermal0_mc\n";
    if(!metrics)throw std::runtime_error("Cannot open metrics.csv");
    metrics<<"elapsed_s,channel,rx_fps,ai_fps,display_fps,process_cpu_one_core_pct,machine_cpu_pct,rga_canvas_ms,ai_preprocess_ms,rknn_api_ms,postprocess_ms,ai_pipeline_ms,receive_to_result_ms,rx_frame_age_ms,kernel_drop_total,missing_packets_total,invalid_total,foreign_total,pool_empty_total,ai_queue_replaced_total,ai_stale_input_total,ai_late_result_total,ai_mode,inference_hz,max_class_score,last_result_boxes,display_header_ms,display_overlay_ms,signal_control_ms_per_s,display_skipped,display_present_ms,x11_conversion_ms,x11_submission_ms,playback_paused,front_source_index,front_still_image_test,display_pack_ms,view_layout\n";
    std::array<ai::Stats,4> old_ai{};std::array<double,4> recent_ai_fps{};
    std::array<ai::Stats,4> old_lane{};std::array<double,4> recent_lane_fps{};
    ai::HybridStats old_hybrid{};
    std::array<qv::Stats,4> oldstats{};
    std::array<ui::LaneOverlayCache,4> lane_overlay_caches;
    for(unsigned i=0;i<4;++i)if(receivers[i])oldstats[i]=receivers[i]->stats();
    std::ofstream scene_events(output+"/scene_events.csv");
    scene_events<<"now_s,event,scene,file,fps,switch_ms,pid,ch1_frames,ch2_frames,ch3_frames\n";
    auto record_scene=[&](const char* event,double cost){
        const auto id=o.files[0].empty()?"FPGA_LIVE":o.scenes.empty()?"FILE":o.scenes[o.scene_index].id;
        scene_events<<std::fixed<<std::setprecision(6)<<qv::now_seconds()<<','<<event<<','<<id<<','<<o.files[0]<<','<<(o.files[0].empty()?0:o.file_fps)<<','<<cost<<','<<getpid();
        for(unsigned i=1;i<4;++i)scene_events<<','<<(receivers[i]?receivers[i]->stats().complete:0);
        scene_events<<'\n';scene_events.flush();if(!scene_events)throw std::runtime_error("Scene log write failed");
        std::cout<<"SCENE "<<event<<' '<<id<<" pid="<<getpid()<<" switch_ms="<<cost<<std::endl;
    };
    record_scene("START",0);
    while(!stop_requested && (!preview || preview->events())){
        if(source_switch){
            const char command=source_switch;source_switch=0;const double began=qv::now_seconds();
            unsigned selected=o.scene_index;if(command=='n')selected=(selected+1)%o.scenes.size();
            const std::string file=command=='v'?"":o.scenes[selected].file;
            const double fps=command=='v'?o.file_fps:o.scenes[selected].fps;
            const std::string cfgpath=command=='v'?o.signal_config:o.scenes[selected].config;
            bool committed=false;
            try{
                auto cfg=signal_demo::load(cfgpath);cfg.automatic=o.live.automatic_signal;cfg.practical_association=o.practical_signal;
                cfg.settings.stop_hold_ms=o.signal_stop_hold_ms;cfg.box_hold_ms=o.signal_box_hold_ms;
                cfg.lane_rules=o.lane_rules;cfg.lane_position_hold_ms=o.lane_position_hold_ms;if(file.empty())cfg.loop_frames=0;
                qv::Endpoint ep{o.bind_a,o.peer_a,5000,30,"HDMI IN1"};
                // Open/preload before stopping the old source; large clips are mapped.
                auto replacement=std::make_unique<qv::InputSource>(ep,file,fps,o.file_loop,make_frame,8,o.batch_us);
                committed=true;feed_enabled->store(false);receivers[0]->stop();if(worker)worker->stop();
                if(playback_paused)paused_seconds+=qv::now_seconds()-pause_started;
                playback_paused=false;if(preview)preview->paused=false;held[0].reset();
                receivers[0]=std::move(replacement);o.files[0]=file;o.file_fps=fps;o.signal_config=cfgpath;o.scene_index=selected;
                input_names[0]=file.empty()?"HDMI IN1":"FILE CH0";names[0]=input_names[0].c_str();
                if(worker){worker->reset_scene(0);receivers[0]->observe([worker,feed_enabled](const std::shared_ptr<qv::Frame>& f){if(feed_enabled->load())worker->publish(0,f);});}
                const double floor=qv::now_seconds();if(signal)signal->reset_scene(cfg,floor);
                product_bridge.front_source_changed(file.empty()?"FPGA_CH0":file);
                if(worker)worker->start();
                feed_enabled->store(true);receivers[0]->start();oldstats[0]={};
                if(preview){preview->front_file_active=!file.empty();preview->playback_detail=file.empty()?"FPGA LIVE CH0 | NEXT: test clips":("SCENE "+o.scenes[selected].id+" | NEXT: next clip | LIVE: FPGA CH0");}
                record_scene(command=='v'?"LIVE":"NEXT",(qv::now_seconds()-began)*1000);
            }catch(const std::exception& e){
                // Recoverable only before the old source was stopped; otherwise fail explicitly.
                if(committed)throw;
                std::cerr<<"SCENE load rejected; retained current input: "<<e.what()<<std::endl;
                record_scene("LOAD_REJECTED",(qv::now_seconds()-began)*1000);
            }
        }
        auto now=std::chrono::steady_clock::now();if(o.seconds && now-start>=std::chrono::seconds(o.seconds))break;
        if(worker && !worker->error().empty())throw std::runtime_error("AI: "+worker->error());
        for(unsigned i=0;i<4;++i)if(receivers[i]){
            if(!receivers[i]->error().empty())throw std::runtime_error(std::string(names[i])+": "+receivers[i]->error());
            auto frame=receivers[i]->take();if(frame){
                if(signal && i==o.live.front && signal->automatic())signal->observe(nullptr,frame->id,frame->source,frame->received,qv::now_seconds(),!o.files[i].empty());
                else if(signal && i==o.live.front){auto memory=std::static_pointer_cast<qv::DmaBuffer>(frame->memory);memory->begin_read();
                    try{signal->observe(memory->data(),frame->id,frame->source,frame->received,qv::now_seconds(),!o.files[i].empty());}
                    catch(...){memory->finish_read();throw;}memory->finish_read();}
                held[i]=std::move(frame);}
        }
        if(auto command=product_bridge.take_command(qv::now_seconds())){if(preview)preview->dispatch(command,"网页");product_bridge.ack_command(qv::now_seconds(),bool(preview));}
        auto control_start=qv::now_seconds();
        std::array<ai::Result,4> result_snapshot{};std::array<std::shared_ptr<const ai::LaneMask>,4> lane_snapshot{};
        if(worker)for(unsigned ch=0;ch<4;++ch){if(o.ai_channels[ch])result_snapshot[ch]=worker->result(ch);if(o.lane.channels[ch])lane_snapshot[ch]=worker->lane_result(ch);}
        if(signal&&o.lane_rules&&worker){auto mask=lane_snapshot[o.live.front];if(mask)signal->observe_lane(*mask,qv::now_seconds());}
        if(signal&&worker){const auto& front_result=result_snapshot[o.live.front];auto stamp=qv::now_seconds();
            if(signal->automatic())signal->observe_automatic(front_result,stamp);
            signal->observe_objects(front_result,stamp);}
        if(product_bridge.enabled()&&worker){auto stamp=qv::now_seconds();
            if(stamp-product_observed>=.05){for(unsigned ch=0;ch<4;++ch)product_bridge.observe(ch,result_snapshot[ch],stamp,playback_paused&&ch==0);product_observed=stamp;}
            if(signal){std::array<product::ViewRisk,4> views;for(unsigned ch=0;ch<4;++ch)views[ch]=product_bridge.risk.decision(ch,stamp);signal->set_surround(views);}
        }
        driver_demo::Explanation explanation;if(signal)signal->tick(qv::now_seconds(),&explanation);
        if(product_bridge.enabled())product_bridge.tick(qv::now_seconds(),explanation,recent_ai_fps,held);
        control_ms+=(qv::now_seconds()-control_start)*1000;
        if(preview)preview->paint_controls(explanation);
        if(preview){auto available=std::find_if(canvases.begin(),canvases.end(),[](const auto&b){return b&&b.use_count()==1;});
        auto view_available=std::find_if(view_canvases.begin(),view_canvases.end(),[](const auto&b){return b&&b.use_count()==1;});
        guint64 queued=0;if(preview->source)g_object_get(preview->source,"current-level-bytes",&queued,nullptr);
        if(available==canvases.end() || (o.surround_view&&view_available==view_canvases.end()) || queued>=W*H*3u){++skipped;}
        else {
            auto begin=std::chrono::steady_clock::now();auto& target=**available;
            for(unsigned i=0;i<4;++i){int x=(i%2)*TW,y=(i/2)*TH;
                bool fresh=held[i] && (o.self_test || qv::now_seconds()-held[i]->received<1.0);
                if(!o.content_refresh||!fresh)qv::rga_blit(*black,TW,TH,NV12,target,W,H,NV12,{x,y,TW,TH});
                if(fresh){auto b=std::static_pointer_cast<qv::DmaBuffer>(held[i]->memory);
                    qv::rga_blit(*b,W,H,rgb_format,target,W,H,NV12,{x+ui::ContentX,y+ui::ContentY,ui::ContentW,ui::ContentH});}
            }
            rga_ms+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            target.begin_write();
            auto header_start=qv::now_seconds();
            if(o.content_refresh&&!o.surround_view)ui::clear_panel_headers(target.data());
            header_ms+=(qv::now_seconds()-header_start)*1000;auto overlay_start=qv::now_seconds();
            for(unsigned i=0;i<4;++i){bool fresh=held[i] && (o.self_test || qv::now_seconds()-held[i]->received<1.0);
                const auto& sampled=result_snapshot[i];const ai::Result empty_result{};
                const auto& result=held[i]&&!ui::result_in_frame_order(sampled,held[i]->id)?empty_result:sampled;
                if(worker && !o.lane.model.empty() && o.lane.channels[i]){
                    auto mask=lane_snapshot[i];
                    if(held[i] && mask && !ui::result_in_frame_order(*mask,held[i]->id))mask.reset();
                    if(mask && mask->cpu_propagated && (qv::now_seconds()-mask->model_received)*1000>o.live.hybrid_semantic_ms)mask.reset();
                    if(o.sparse_lane_overlay)lane_overlay_caches[i].bind(mask);
                    ui::lane_overlay(target.data(),i,fresh,mask.get(),recent_lane_fps[i],o.lane.max_age_ms,!(signal&&o.lane_rules&&i==o.live.front),i!=o.live.front,o.sparse_lane_overlay?&lane_overlay_caches[i]:nullptr);}
                ui::panel(target.data(),i,names[i],o.channels[i],fresh,worker && o.ai_channels[i],result,recent_ai_fps[i],o.max_age_ms,!(signal&&signal->automatic()&&i==o.live.front),i!=o.live.front,!o.surround_view);
                if(o.surround_view&&!fresh)ui::text(target.data(),int(i%2)*640+ui::ContentX+180,int(i/2)*360+ui::ContentY+150,
                    o.channels[i]?"WAITING INPUT":"DISABLED",ui::White,2,int(i%2)*640+ui::ContentX+ui::ContentW);
            }
            if(signal)signal->draw(target.data(),o.live.front,qv::now_seconds());
            overlay_ms+=(qv::now_seconds()-overlay_start)*1000;
            target.finish_write();auto presented=*available;
            if(o.surround_view){auto pack_begin=qv::now_seconds();
                if(packed_revision!=preview->layout_revision){
                    auto tiles=surround_ui::Layout{o.live.front,preview->front_priority}.destinations();
                    for(unsigned role=0;role<4;++role){auto r=tiles[role];view_destinations[role]={r.x,r.y,r.w,r.h};}
                    packed_revision=preview->layout_revision;
                }
                auto index=size_t(std::distance(view_canvases.begin(),view_available));
                if(canvas_revisions[index]!=packed_revision){
                    auto& canvas=**view_available;canvas.begin_write();surround_ui::initialize(canvas.data(),o.live.front,preview->front_priority);canvas.finish_write();
                    canvas_revisions[index]=packed_revision;
                }
                qv::rga_display_regions(target,**view_available,view_sources,view_destinations,view_batch_allowed);
                double cost=(qv::now_seconds()-pack_begin)*1000;pack_ms+=cost;total_pack_ms+=cost;presented=*view_available;
            }
            auto present_begin=qv::now_seconds();preview->push(std::move(presented),o.fps);
            double cost=(qv::now_seconds()-present_begin)*1000;present_ms+=cost;total_present_ms+=cost;++total_presented;
            if(preview->software){convert_ms+=preview->software->conversion_ms;submit_ms+=preview->software->submission_ms;}
            if(product_bridge.enabled()&&!product_display_ready&&preview->rendered()>0){
                double stamp=qv::now_seconds();bool fresh=false;for(const auto& f:held)fresh|=f&&stamp>=f->received&&stamp-f->received<.75;
                if(fresh){product_bridge.display_ready(stamp);product_display_ready=true;}
            }
            ++pushed;
        }}
        now=std::chrono::steady_clock::now();double dt=std::chrono::duration<double>(now-last).count();
        if(dt>=2){auto current=cpu();auto clock=std::clock();auto rendered=preview?preview->rendered():0;
            auto inferences=worker?worker->inference_count():0;double inference_hz=(inferences-previous_inferences)/dt;
            double machine=current.total>oldcpu.total?100.0*(1.0-double(current.idle-oldcpu.idle)/(current.total-oldcpu.total)):0;
            double process=100.0*(clock-oldclock)/CLOCKS_PER_SEC/dt,display_rate=(rendered-previous_rendered)/dt,elapsed=std::chrono::duration<double>(now-start).count();
            telemetry<<elapsed<<','<<rss_kb()<<','<<sensor("/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq")<<','
                <<sensor("/sys/class/devfreq/fde40000.npu/cur_freq")<<','<<sensor("/sys/class/devfreq/dmc/cur_freq")<<','<<sensor("/sys/class/thermal/thermal_zone0/temp")<<'\n';
            telemetry.flush();if(!telemetry)throw std::runtime_error("Cannot write telemetry.csv");
            std::cout<<std::fixed<<std::setprecision(1)<<"CPU process="<<process<<"% machine="<<machine
                <<"% | PUSH="<<pushed/dt<<" DISPLAY="<<(rendered-previous_rendered)/dt<<" fps RGA="<<(pushed?rga_ms/pushed:0)
                <<"ms PACK="<<(pushed?pack_ms/pushed:0)<<"ms PRESENT="<<(pushed?present_ms/pushed:0)<<"ms skip="<<skipped<<" | INFER="<<inference_hz<<"Hz mode="<<o.ai_mode<<"\n";
            for(unsigned i=0;i<4;++i)if(receivers[i]){auto s=receivers[i]->stats();const auto& old=oldstats[i];
                auto batches=s.rx_batches-old.rx_batches;std::cout<<"  "<<names[i]<<" RX="<<(s.complete-old.complete)/dt
                    <<" batch="<<(batches?double(s.packets-old.packets)/batches:0)<<" kernel_drop="<<s.kernel_drops
                    <<" missing="<<s.missing<<" invalid="<<s.invalid<<" foreign="<<s.foreign<<" pool="<<s.pool_exhausted
                    <<" superseded="<<receivers[i]->replaced()<<" age="<<(held[i]?qv::now_seconds()-held[i]->received:-1)<<"s";
                auto a=worker?worker->stats(i):ai::Stats{};const auto& olda=old_ai[i];auto count=a.completed-olda.completed;
                double n=count?double(count):1;recent_ai_fps[i]=count/dt;
                std::cout<<" | AI="<<recent_ai_fps[i]<<"Hz prep="<<(a.preprocess_ms-olda.preprocess_ms)/n
                         <<" api="<<(a.api_ms-olda.api_ms)/n<<" post="<<(a.postprocess_ms-olda.postprocess_ms)/n
                         <<" track="<<(a.tracking_ms-olda.tracking_ms)/n
                         <<" pipe="<<(a.pipeline_ms-olda.pipeline_ms)/n<<"ms queue_replace="<<a.replaced<<" stale="<<a.stale<<" late="<<a.late
                         <<" max_score="<<std::setprecision(4)<<a.max_class_score<<std::setprecision(1)<<" boxes="<<(worker?worker->result(i).boxes.size():0)<<"\n";
                metrics<<std::fixed<<std::setprecision(3)<<elapsed<<','<<i<<','<<(s.complete-old.complete)/dt<<','<<recent_ai_fps[i]<<','<<display_rate
                       <<','<<process<<','<<machine<<','<<(pushed?rga_ms/pushed:0)<<','<<(a.preprocess_ms-olda.preprocess_ms)/n
                       <<','<<(a.api_ms-olda.api_ms)/n<<','<<(a.postprocess_ms-olda.postprocess_ms)/n<<','<<(a.pipeline_ms-olda.pipeline_ms)/n
                       <<','<<(a.frame_age_ms-olda.frame_age_ms)/n<<','<<(held[i]?(qv::now_seconds()-held[i]->received)*1000:-1)
                       <<','<<s.kernel_drops<<','<<s.missing<<','<<s.invalid<<','<<s.foreign<<','<<s.pool_exhausted<<','<<a.replaced<<','<<a.stale<<','<<a.late<<','<<o.ai_mode<<','<<inference_hz
                       <<','<<std::setprecision(6)<<a.max_class_score<<','<<(worker?worker->result(i).boxes.size():0)
                       <<','<<(pushed?header_ms/pushed:0)<<','<<(pushed?overlay_ms/pushed:0)<<','<<control_ms/dt<<','<<skipped<<','<<(pushed?present_ms/pushed:0)<<','<<(pushed?convert_ms/pushed:0)<<','<<(pushed?submit_ms/pushed:0)<<','<<playback_paused<<','<<(held[o.live.front]?held[o.live.front]->source:0)<<','<<(held[o.live.front]&&held[o.live.front]->still_image_test)<<','<<(pushed?pack_ms/pushed:0)<<','<<(o.surround_view?"surround":"grid")<<'\n';
                oldstats[i]=s;old_ai[i]=a;
                if(worker && !o.lane.model.empty() && o.lane.channels[i]){
                    auto lane=worker->lane_stats(i);auto count=lane.completed-old_lane[i].completed;double n=count?double(count):1;
                    recent_lane_fps[i]=count/dt;auto mask=worker->lane_result(i);
                    std::cout<<"    LANE="<<recent_lane_fps[i]<<"Hz pipe="<<(lane.pipeline_ms-old_lane[i].pipeline_ms)/n
                             <<"ms age="<<(mask?(qv::now_seconds()-mask->received)*1000:-1)<<"ms pixels="<<(mask?mask->pixels:0)
                             <<" cpu_queue_replace="<<lane.cpu_replaced<<" cpu_stale="<<lane.cpu_stale<<" queue_replace="<<lane.replaced<<" stale="<<lane.stale<<" late="<<lane.late<<'\n';
                    old_lane[i]=lane;
                }
            }
            if(worker && o.live.lane_hybrid){auto h=worker->hybrid_stats();auto attempts=h.attempts-old_hybrid.attempts;
                std::cout<<"    CPU_LANE="<<(h.completed-old_hybrid.completed)/dt<<"Hz accepted="<<h.completed
                    <<" rejected="<<h.rejected<<" compute="<<(h.cpu_ms-old_hybrid.cpu_ms)/std::max<uint64_t>(1,attempts)
                    <<"ms max_update_gap="<<h.max_gap_ms<<"ms (separate from MODEL Hz)\n";old_hybrid=h;}
            metrics.flush();if(!metrics)throw std::runtime_error("Metrics write failed");
            if(preview){preview->ai_rates=recent_ai_fps;preview->lane_rate=recent_lane_fps[o.live.front];preview->machine_cpu=machine;preview->display_rate=display_rate;}
            if(preview && !pushed && skipped)throw std::runtime_error("No canvas available for 2 seconds. Downstream retained the DMA pool; stop instead of silently freezing.");
            std::cout.flush();last=now;oldclock=clock;oldcpu=current;previous_rendered=rendered;previous_inferences=inferences;pushed=skipped=0;rga_ms=header_ms=overlay_ms=control_ms=present_ms=convert_ms=submit_ms=pack_ms=0;
        }
        next+=std::chrono::nanoseconds(1000000000/o.fps);
        if(next<std::chrono::steady_clock::now())next=std::chrono::steady_clock::now();
        std::this_thread::sleep_until(next);
    }
    if(signal){signal->key('s',qv::now_seconds(),"系统退出");signal->log("EXIT_STOP",qv::now_seconds());}
    feed_enabled->store(false);
    if(worker)worker->stop();
    for(auto& r:receivers)if(r)r->stop();
    if(worker && !worker->error().empty())throw std::runtime_error("AI: "+worker->error());
    std::ofstream summary(output+"/summary.txt");double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    summary<<std::fixed<<std::setprecision(9)<<"measurement_begin_monotonic_s="<<std::chrono::duration<double>(start.time_since_epoch()).count()<<"\nmeasurement_end_monotonic_s="<<std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()<<"\nnormal_exit=true\nseconds="<<elapsed<<"\nmodel="<<model<<"\n";
    summary<<"ai_mode="<<o.ai_mode<<"\ninference_calls="<<(worker?worker->inference_count():0)<<"\ninference_hz="<<(worker?worker->inference_count()/elapsed:0)
           <<"\nMosaic stage times are shared per composite and must not be summed across channel rows. AI_HZ counts only new frames.\n";
    summary<<"view_layout="<<(o.surround_view?"surround":"grid")<<"\ndisplay_pack_ms="<<(total_presented?total_pack_ms/total_presented:0)<<'\n';
    summary<<"view_submit_effective="<<(!total_presented?"not_run":o.surround_view?(view_batch_allowed?"batch":"serial"):"none")<<'\n';
    for(unsigned k=0;k<4;++k)if(receivers[k]){auto s=receivers[k]->stats();auto a=worker?worker->stats(k):ai::Stats{};
        double n=a.completed?double(a.completed):1;
        summary<<"CH"<<k<<" RX_COMPLETE="<<s.complete<<" AI_COMPLETE="<<a.completed<<" AI_HZ="<<a.completed/elapsed
               <<" PREP_MS="<<a.preprocess_ms/n<<" API_MS="<<a.api_ms/n<<" POST_MS="<<a.postprocess_ms/n<<" PIPE_MS="<<a.pipeline_ms/n
               <<" TRACK_MS="<<a.tracking_ms/n<<" RECEIVE_TO_RESULT_MS="<<a.frame_age_ms/n<<" KERNEL_DROP="<<s.kernel_drops<<" MISSING="<<s.missing
               <<" POOL_EMPTY="<<s.pool_exhausted<<" QUEUE_REPLACED="<<a.replaced<<" STALE_INPUT="<<a.stale<<" LATE_RESULT="<<a.late<<"\n";
        if(worker && !o.lane.model.empty() && o.lane.channels[k]){
            auto lane=worker->lane_stats(k);double n=lane.completed?double(lane.completed):1;
            summary<<"CH"<<k<<" LANE_COMPLETE="<<lane.completed<<" LANE_HZ="<<lane.completed/elapsed
                   <<" LANE_PIPE_MS="<<lane.pipeline_ms/n<<" LANE_API_MS="<<lane.api_ms/n
                   <<" LANE_RECEIVE_TO_RESULT_MS="<<lane.frame_age_ms/n<<" CPU_QUEUE_REPLACED="<<lane.cpu_replaced<<" CPU_STALE="<<lane.cpu_stale<<" LANE_STALE="<<lane.stale<<" LANE_LATE="<<lane.late<<'\n';}
    }
    if(worker){auto h=worker->hybrid_stats();summary<<"HYBRID_ENABLED="<<o.live.lane_hybrid<<" MODEL_DEADLINE_MS="<<o.live.lane_deadline_ms
        <<" CPU_ATTEMPTS="<<h.attempts<<" CPU_ACCEPTED="<<h.completed<<" CPU_ACCEPTED_HZ="<<h.completed/elapsed<<" CPU_REJECTED="<<h.rejected
        <<" CPU_QUEUE_REPLACED="<<h.replaced<<" CPU_MS_MEAN="<<h.cpu_ms/std::max<uint64_t>(1,h.attempts)<<" CPU_MAX_UPDATE_GAP_MS="<<h.max_gap_ms<<'\n';}
    if(playback_paused)paused_seconds+=qv::now_seconds()-pause_started;
    summary<<"playback_pause_count="<<pause_count<<"\nplayback_paused_seconds="<<paused_seconds<<"\nperformance_contains_review_pause="<<(pause_count>0)<<"\ndisplay_present_mean_ms="<<total_present_ms/std::max<uint64_t>(1,total_presented)<<"\nnext_video_requested="<<next_video<<'\n';
    summary<<"No ground-truth accuracy or camera exposure latency is measured. Warmup is excluded from AI totals.\n";
    std::string incomplete;
    for(unsigned k=0;k<4;++k)if(receivers[k]){
        if(!receivers[k]->stats().complete)incomplete+="CH"+std::to_string(k)+" received no complete frames. ";
        else if(worker && o.ai_channels[k] && !worker->stats(k).completed)incomplete+="CH"+std::to_string(k)+" completed no AI inference. ";
        if(worker && !o.lane.model.empty() && o.lane.channels[k] && !worker->lane_stats(k).completed)
            incomplete+="CH"+std::to_string(k)+" completed no lane inference. ";
    }
    summary<<"validation_has_missing_channels="<<!incomplete.empty()<<"\n";summary.flush();
    if(!incomplete.empty())throw std::runtime_error(incomplete+"See the saved metrics; this is not a successful four-channel validation.");
    std::ofstream inputs(output+"/input_sources.csv");
    inputs<<"channel,kind,source_fps,file_bytes,emitted,skipped_source_intervals,loops,eof,copy_ms_total,still_repeats\n";
    for(unsigned k=0;k<4;++k)if(receivers[k])receivers[k]->write_summary(inputs,k);
    inputs.flush();if(!inputs)throw std::runtime_error("Cannot write input_sources.csv");
    std::cout<<"Saved metrics, detections and summary: "<<output<<"\n";
    return next_video?20:0;
}catch(const std::exception&e){std::cerr<<"ERROR: "<<e.what()<<"\n";
    if(!output.empty()){std::ofstream error(output+"/error.txt");error<<e.what()<<'\n';}return 1;}}
