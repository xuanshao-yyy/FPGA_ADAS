#include "signal_control.hpp"
#include "scene_playlist.hpp"
#include <filesystem>
#include <cassert>
#include <unistd.h>
int main(){
    namespace fs=std::filesystem;using namespace signal_demo;
    auto root=fs::temp_directory_path()/("lane_rules_"+std::to_string(getpid()));assert(fs::create_directory(root));
    auto cfg=load("configs/auto_red.json");cfg.automatic=true;cfg.lane_rules=true;cfg.loop_frames=1000;cfg.settings.stop_hold_ms=1500;
    Control control(cfg,root.string());unsigned source=0;
    auto lane=[&](double t,double offset,int left=1,int right=2){ai::LaneMask mask;mask.received=mask.model_received=t;mask.finished=t+.1;mask.frame_id=mask.source=++source;
        mask.four_class=mask.position_valid=mask.geometry.valid=true;mask.relative_offset=offset;mask.left_margin=120*(1+offset);mask.right_margin=120*(1-offset);mask.left_style=left;mask.right_style=right;mask.left_curve.valid=mask.right_curve.valid=true;mask.left_evidence.reason=mask.right_evidence.reason="CONFIRMED_CANDIDATE";
        control.observe_lane(mask,t+.1);return control.lane_decision(t+.1);};
    lane(10,0);lane(10.5,0);control.key('j',10.6);assert(control.has_lane_rules());assert(control.selected()==Direction::Straight);assert(control.lane_feedback(10.6)=="BLOCKED - SOLID LINE");
    assert(control.lane_decision(10.6).alarm&&control.toolbar_status(10.6).find("COMBINED")!=std::string::npos);
    control.key('s',10.6);control.key('m',10.6);assert(control.lane_decision(10.6).marking_permits&&!control.lane_decision(10.6).allowed);assert(control.lane_feedback(10.6)=="WAIT - FRONT EVIDENCE");
    {
        std::array<product::ViewRisk,4> views{};for(auto& v:views){v.fresh=v.calibrated=true;}
        ai::Result front;front.received=10.6;front.finished=10.61;front.frame_id=front.source=++source;control.observe_objects(front,10.62);
        control.set_surround(views);assert(control.lane_decision(10.62).allowed);
        views[1].occupied=true;control.set_surround(views);assert(!control.lane_decision(10.62).allowed);assert(control.lane_feedback(10.62)=="BLOCKED - SIDE OCCUPIED");
        views[1].occupied=false;views[3].approaching=true;control.set_surround(views);assert(!control.lane_decision(10.62).allowed);assert(control.lane_feedback(10.62)=="BLOCKED - REAR APPROACH");
        views[3].approaching=false;views[1].fresh=false;control.set_surround(views);assert(!control.lane_decision(10.62).allowed);
        views={};views[2].image_fresh=true;views[2].proximity=2;control.set_surround(views);
        assert(std::string(control.lane_decision(10.62).rule)!="BLOCKED_SIDE_OCCUPIED"); // right request does not read CH2
        views[1].image_fresh=true;views[1].proximity=1;control.set_surround(views);
        assert(std::string(control.lane_decision(10.62).rule)=="BLOCKED_SIDE_OCCUPIED");
        control.set_surround({});
    }
    control.focus_lost(10.6);assert(control.lane_decision(10.6).requested);control.key('l',10.61);
    lane(11,.3);lane(11.5,.6);lane(12,.7);assert(std::string(lane(12.5,.8).motion)=="NEAR_RIGHT_BOUNDARY");
    control.tick(13.3);assert(control.lane_decision(13.3).position_held);control.tick(13.51);assert(std::string(control.lane_decision(13.51).motion)=="UNKNOWN");
    // A road-left red signal and an independent dashed-left lane request share the restriction.
    lane(14,0,2,2);lane(14.5,0,2,2);control.key('a',14.6);control.key('j',14.6);
    ai::SignalCandidate red;red.direction=0;red.track_id=7;red.box={100,100,180,180,.9f,7};red.color="RED";red.shape="LEFT";
    for(int i=0;i<3;++i){ai::Result r;r.received=14.6+i*.1;r.finished=r.received+.05;r.frame_id=r.source=++source;r.signal_candidates={red};control.observe_automatic(r,r.finished);}
    assert(std::string(control.lane_decision(14.85).rule)=="BLOCKED_SIGNAL_STOP");
    control.key('c',14.9);assert(!control.lane_decision(14.9).requested);
    control.observe(nullptr,1001,1001,15,15,true); // establish loop number
    control.observe(nullptr,2001,2001,15.1,15.1,true);assert(std::string(control.lane_decision(15.1).motion)=="UNKNOWN");
    control.key('l',15.2);control.key('a',15.2);control.key(' ',15.2);
    control.reset_scene(cfg,15.3);
    assert(!control.lane_decision(15.3).requested && std::string(control.lane_decision(15.3).motion)=="UNKNOWN");
    for(unsigned d=0;d<3;++d)assert(std::string(control.permission(Direction(d),15.3))=="WAIT");
    ai::Result old;old.received=15.2;old.finished=15.31;old.frame_id=old.source=1;old.signal_candidates={red};
    control.observe_automatic(old,15.31);for(unsigned d=0;d<3;++d)assert(std::string(control.permission(Direction(d),15.31))=="WAIT");
    assert(fs::file_size(root/"lane_rules.csv")>0);
    { // Visible segmentation alone must be explained without inventing an own-lane pair.
        ai::LaneMask mask;mask.received=mask.model_received=16;mask.frame_id=mask.source=++source;mask.four_class=true;
        mask.data={0,1,2,3};mask.left_evidence.reason=mask.right_evidence.reason="NO_CURVE";
        mask.pair_reason="MISSING_BOUNDARY";control.observe_lane(mask,16.1);
        auto explanation=control.explanation(16.1);assert(!explanation.lane_position_known);
        assert(explanation.lane_detail=="有像素候选，边界未拟合");
        assert(!control.lane_decision(16.1).allowed);
        assert(control.explanation(17).lane_detail=="车道结果过期");
        mask.received=mask.model_received=18;mask.data.assign(100,1);control.observe_lane(mask,18.1);
        assert(control.explanation(18.1).lane_pixels=="已识别实线像素");
        mask.received=mask.model_received=18.5;mask.left_curve.valid=true;mask.left_evidence.reason="CONFIRMED_CANDIDATE";mask.left_style=1;
        control.observe_lane(mask,18.6);auto partial=control.explanation(18.6);
        assert(partial.lane_styles[0]==1&&partial.lane_styles[1]==0&&!partial.lane_position_known);
        assert(partial.lane_detail=="左实线已确认，右侧边界不足");
    }
    { // Playlist validates size, unique IDs, rates and loop counts before opening runtime.
        auto raw=root/"one.rgb565";{std::ofstream f(raw,std::ios::binary);f.seekp(qv::FrameBytes-1);f.put(0);}
        auto conf=root/"config.json";{std::ofstream f(conf);f<<R"({"scene":"test","width":1280,"height":720,"loop_frames":1,"rois":{"LEFT":[],"STRAIGHT":[],"RIGHT":[]}})";}
        auto playlist=root/"playlist.json";
        auto write=[&](double fps,int copies){std::ofstream f(playlist);f<<"{\"scenes\":[";for(int i=0;i<copies;++i){if(i)f<<',';f<<"{\"id\":\"one\",\"file\":\""<<raw.string()<<"\",\"config\":\""<<conf.string()<<"\",\"fps\":"<<fps<<'}';}f<<"]}";};
        write(24,1);assert(scene_player::load(playlist.string(),2).size()==1);
        for(int test=0;test<3;++test){write(test==0?0:24,test==1?2:1);bool rejected=false;try{scene_player::load(playlist.string(),test==2?1:2);}catch(const std::exception&){rejected=true;}assert(rejected);}
    }
    std::cout<<"PASS SCENE and request routing, solid alarm, dashed marking-only advisory, focus/recheck/loop reset, position freshness and traffic-stop precedence\n";
}
