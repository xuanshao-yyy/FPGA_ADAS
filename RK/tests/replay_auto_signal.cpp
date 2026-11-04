#include "signal_control.hpp"
#include "signal_shape.hpp"
#include "live_tracking.hpp"
#include <filesystem>
#include <cassert>
int main(int argc,char**argv){
    if(argc!=5&&argc!=6)return 2;
    std::ifstream input(argv[1]),video(argv[2],std::ios::binary);assert(input&&video);
    namespace fs=std::filesystem;assert(fs::create_directory(argv[3]));
    auto cfg=signal_demo::load(argv[4]);cfg.automatic=true;
    if(argc==6)cfg.settings.stop_hold_ms=std::stod(argv[5]);
    signal_demo::Control control(cfg,argv[3]);ai::LiveTracker tracker(.25f,.15f);
    std::ofstream candidates(std::string(argv[3])+"/candidates.csv"),steps(std::string(argv[3])+"/steps.csv");
    candidates<<"frame_id,source,received_s,track_id,x1,y1,x2,y2,score,color,shape,direction,shape_iou,shape_margin,reason\n";
    steps<<"frame_id,source,received_s,finished_s,direction,light,requested,allowed,alarm,state,candidates,stop_held,stop_age_ms,instruction\n";
    std::vector<uint8_t> raw(qv::FrameBytes);double received,finished;unsigned id,source,n,frames=0,found=0,arrows=0,alarms=0,allowed=0;int previous_direction=-1;uint64_t previous_loop=~uint64_t(0);
    while(input>>received>>finished>>id>>source>>n){std::vector<ai::Box> boxes(n);for(auto& b:boxes)assert(bool(input>>b.x1>>b.y1>>b.x2>>b.y2>>b.score>>b.category));
        uint64_t loop=(uint64_t(source)-1)/cfg.loop_frames;auto index=(source-1)%cfg.loop_frames;
        video.seekg(uint64_t(index)*qv::FrameBytes);assert(bool(video.read(reinterpret_cast<char*>(raw.data()),raw.size())));
        auto tracks=tracker.update(boxes,received,0);ai::Result result;result.frame_id=id;result.source=source;result.received=received;result.finished=finished;
        result.signal_candidates=ai::automatic_signal_candidates(raw.data(),boxes,tracks,.15f);control.observe(nullptr,id,source,received,received,true);control.observe_automatic(result,finished);
        int dir=index<38?1:index<74?0:2;
        if(loop!=previous_loop||dir!=previous_direction){control.key(dir==1?'w':dir==0?'a':'d',finished);previous_loop=loop;previous_direction=dir;}
        if(index%38>=12&&index%38<18)control.key(' ',finished); // scripted requests, never injected labels/colors
        auto decision=control.tick(finished);
        steps<<id<<','<<source<<','<<std::fixed<<std::setprecision(9)<<received<<','<<finished<<','<<signal_demo::name(decision.direction)<<','<<signal_demo::name(decision.light)<<','<<decision.requested<<','<<decision.allowed<<','<<decision.alarm<<','<<decision.state<<','<<result.signal_candidates.size()<<','<<decision.stop_held<<','<<decision.stop_age_ms<<','<<decision.instruction<<'\n';
        for(const auto& c:result.signal_candidates){candidates<<id<<','<<source<<','<<std::fixed<<std::setprecision(9)<<received<<','<<c.track_id<<','<<c.box.x1<<','<<c.box.y1<<','<<c.box.x2<<','<<c.box.y2<<','<<c.box.score<<','<<c.color<<','<<c.shape<<','<<c.direction<<','<<c.shape_score<<','<<c.shape_margin<<','<<c.reason<<'\n';found+=c.color!="UNKNOWN";arrows+=c.direction>=0;}
        alarms+=decision.alarm;allowed+=decision.allowed;++frames;
    }
    assert(frames>0&&found>0);
    if(cfg.scene=="auto_red_detector_roi")assert(arrows>0&&alarms>0&&allowed==0);
    std::cout<<"PASS same-source automatic ROI replay frames="<<frames<<" colored_candidates="<<found<<" arrow_candidates="<<arrows<<" alarm_frames="<<alarms<<" allowed_frames="<<allowed<<". Detector boxes came from RK logs, colors/shapes from original RGB565; no new NPU or accuracy claim.\n";
}
