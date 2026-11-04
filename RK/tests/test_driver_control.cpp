#include "signal_control.hpp"
#include <filesystem>
#include <cassert>
#include <unistd.h>
int main(){
    using namespace signal_demo;auto root=std::filesystem::temp_directory_path()/("driver_control_"+std::to_string(getpid()));assert(std::filesystem::create_directory(root));
    Config cfg;cfg.automatic=true;cfg.lane_rules=true;Control control(cfg,root.string());
    ai::Result result;result.received=10;result.finished=10.02;result.frame_id=result.source=1;
    control.observe_objects(result,10.03);auto e=control.explanation(10.03);
    assert(e.action=="保持观察 / KEEP"&&e.signal[1]=="当前未见灯");
    ai::SignalCandidate round;round.color="GREEN";round.shape="CIRCLE";round.direction=-1;round.track_id=8;
    round.box={100,100,130,180,.9f,7}; // Plausible peripheral lamp, not a central-single fallback.
    result.signal_candidates={round};control.observe_automatic(result,10.03);e=control.explanation(10.03);
    assert(e.signal_detail[1]=="见绿色·方向未明"&&e.signal[1]!="信号允许");
    round.color="RED";result.signal_candidates.push_back(round);result.received=10.1;result.finished=10.12;
    control.observe_automatic(result,10.12);control.observe_objects(result,10.12);e=control.explanation(10.12);
    assert(e.signal_detail[1]=="多色灯·方向未明"&&e.signal[1]!="信号允许");
    ai::SignalCandidate red;red.direction=1;red.track_id=7;red.color="RED";red.shape="STRAIGHT";red.box={500,100,550,180,.9f,7};
    for(int i=0;i<4;++i){result.received=11+i*.1;result.finished=result.received+.02;result.frame_id=result.source=i+2;result.signal_candidates={red};control.observe_automatic(result,result.finished);control.observe_objects(result,result.finished);control.tick(result.finished);}
    assert(control.explanation(11.33).action=="停止 / STOP");
    assert(control.explanation(11.33).signal_detail[1]=="灯色与方向已关联");
    control.key(' ',11.34,"界面");assert(control.explanation(11.34).vehicle_alarm&&control.explanation(11.34).audible_alarm); // Visible GO uses the existing guarded request.
    result.received=14;result.finished=14.02;result.frame_id=result.source=10;result.signal_candidates={};control.observe_automatic(result,14.02);control.observe_objects(result,14.02);
    assert(control.explanation(14.03).action=="观察 / WATCH"); // absence cannot clear an earlier red restriction
    for(int i=0;i<14;++i){result.received=14.1+i*.1;result.finished=result.received+.02;result.frame_id++;result.source++;
        control.observe_automatic(result,result.finished);control.observe_objects(result,result.finished);control.tick(result.finished);}
    e=control.explanation(15.43);assert(e.signal[1]=="当前未见灯"&&e.action=="保持观察 / KEEP");
    assert(std::string(control.permission(Direction::Straight,15.43))=="WAIT"); // Absence is never GREEN permission.
    control.reset_scene(cfg,15);result.received=16;result.finished=16.02;result.frame_id=result.source=1;control.observe_objects(result,16.02);
    assert(control.explanation(16.03).action=="保持观察 / KEEP");
    ai::TrackedBox car;car.id=22;car.box={400,300,900,710,.9f,2};
    for(int i=0;i<3;++i){result.received=17+i*.1;result.finished=result.received+.02;result.frame_id=result.source=i+2;result.tracks={car};control.observe_objects(result,result.finished);}
    assert(control.explanation(17.23).severity==3&&control.explanation(17.23).target.find("22")!=std::string::npos);
    control.set_still_image(true,18);assert(control.explanation(18).still&&control.explanation(18).severity<3);
    std::vector<uint8_t> image(1280*720*3/2,77);auto before=image;control.draw(image.data(),0,18);assert(image==before); // prose/alarms never paint the picture
    control.reset_scene(cfg,25);result.received=25;result.finished=25.01;result.tracks.clear();control.observe_objects(result,25.02);
    std::array<product::ViewRisk,4> views{};views[2].image_fresh=true;views[2].proximity=2;views[2].proximity_area=.3;
    control.set_surround(views);e=control.explanation(25.03);assert(e.severity<2&&!control.toolbar_alarm(25.03)&&e.near_levels[2]==2&&e.vehicle_moving);
    views={};views[1].image_fresh=true;views[1].proximity=2;control.set_surround(views);e=control.explanation(25.03);assert(e.severity<2&&!control.toolbar_alarm(25.03));
    views={};views[3].image_fresh=true;views[3].image_approaching=true;control.set_surround(views);e=control.explanation(25.03);assert(e.severity==3&&e.vehicle_state=="ALERT"&&e.vehicle_label=="报警"&&e.reason.find("后方")!=std::string::npos);
    views[3].image_fresh=false;control.set_surround(views);assert(control.explanation(25.03).severity<2);
}
