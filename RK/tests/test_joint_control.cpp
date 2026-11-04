#include "signal_control.hpp"
#include <filesystem>
#include <cassert>
#include <unistd.h>
using namespace signal_demo;
struct Fixture {
    std::filesystem::path root;
    Control control;
    unsigned sequence=0;
    std::array<product::ViewRisk,4> views{};
    static Config config(){Config c;c.automatic=c.lane_rules=true;c.loop_frames=1000;c.settings.stop_hold_ms=1500;return c;}
    static std::filesystem::path directory(int n){auto p=std::filesystem::temp_directory_path()/("joint_"+std::to_string(getpid())+"_"+std::to_string(n));assert(std::filesystem::create_directory(p));return p;}
    explicit Fixture(int n):root(directory(n)),control(config(),root.string()){
        for(auto& v:views)v.fresh=v.calibrated=true;
        control.set_surround(views);lane(9.7);lane(10.);front(10.2);
    }
    void front(double t){ai::Result r;r.received=t;r.finished=t+.01;r.source=r.frame_id=++sequence;control.observe_objects(r,t+.01);}
    void lane(double t,int left=2,int right=2){ai::LaneMask m;m.received=m.model_received=t;m.finished=t+.01;m.source=m.frame_id=++sequence;
        m.four_class=m.position_valid=m.geometry.valid=true;m.left_margin=m.right_margin=120;m.relative_offset=0;
        m.left_style=left;m.right_style=right;m.left_curve.valid=m.right_curve.valid=true;
        m.left_evidence.reason=m.right_evidence.reason="CONFIRMED_CANDIDATE";control.observe_lane(m,t+.01);}
    void lights(const char* straight,const char* left){
        for(double t:{10.,10.1,10.2}){ai::Result r;r.received=t;r.finished=t+.01;r.source=r.frame_id=++sequence;
            for(unsigned d:{0u,1u}){ai::SignalCandidate c;c.track_id=d+7;c.direction=int(d);c.color=d?straight:left;c.shape=d?"UP":"LEFT";c.box={float(100+200*d),100,float(170+200*d),200,.9f,7};r.signal_candidates.push_back(c);}
            control.observe_automatic(r,t+.01);}
    }
};
int main(){
    {Fixture f(0);f.control.key('j',10.23);assert(f.control.selected()==Direction::Straight);assert(f.control.lane_decision(10.24).allowed);
     auto e=f.control.explanation(10.24);assert(e.intent==1&&e.lane_intent==0&&e.action=="允许变道 / CHANGE");
     f.control.key('l',10.25);assert(!f.control.lane_decision(10.25).requested);assert(f.control.selected()==Direction::Straight);}
    {Fixture f(1);f.lights("RED","GREEN");f.control.key('j',10.23);assert(std::string(f.control.lane_decision(10.24).rule)=="BLOCKED_SIGNAL_STOP");assert(f.control.explanation(10.24).action=="停止 / STOP");}
    {Fixture f(2);f.lights("GREEN","RED");f.control.key('j',10.23);assert(f.control.lane_decision(10.24).allowed); // Lane-left is not road-left.
     f.control.key('a',10.25);assert(!f.control.lane_decision(10.25).requested);assert(f.control.explanation(10.25).action=="停止 / STOP");}
    {Fixture f(3);f.lights("GREEN","GREEN");f.lane(10.2,1,2);f.control.key('j',10.23);assert(!f.control.lane_decision(10.24).allowed);assert(f.control.explanation(10.24).severity==3);}
    {Fixture f(4);f.lights("GREEN","GREEN");f.views[2].image_fresh=true;f.views[2].proximity=1;f.control.set_surround(f.views);f.control.key('j',10.23);
     assert(std::string(f.control.lane_decision(10.24).rule)=="BLOCKED_SIDE_OCCUPIED");assert(f.control.explanation(10.24).action=="禁止变道 / KEEP");}
    {Fixture f(5);f.views[3].image_fresh=true;f.views[3].image_approaching=true;f.control.set_surround(f.views);f.control.key('m',10.23);assert(std::string(f.control.lane_decision(10.24).rule)=="BLOCKED_REAR_APPROACH");}
    {Fixture f(6);f.control.key('j',10.23);assert(!f.control.lane_decision(10.65).allowed);assert(std::string(f.control.lane_decision(10.65).rule)=="WAIT_FRONT_EVIDENCE");}
    {Fixture f(7);f.views[0].person=1;f.control.set_surround(f.views);f.control.key('j',10.23);assert(std::string(f.control.lane_decision(10.24).rule)=="BLOCKED_FRONT_RISK");assert(f.control.explanation(10.24).severity>=2);}
    {Fixture f(8);f.views[2].fresh=false;f.control.set_surround(f.views);f.control.key('j',10.23);assert(!f.control.lane_decision(10.24).allowed);assert(f.control.explanation(10.24).action=="保持车道 / KEEP");
     f.control.key('c',10.25);assert(!f.control.lane_decision(10.25).requested);assert(std::string(f.control.permission(Direction::Straight,10.25))=="WAIT");}
    {Fixture f(9);f.lights("UNKNOWN","GREEN");f.control.key('j',10.23);assert(std::string(f.control.lane_decision(10.24).rule)=="WAIT_SIGNAL_UNCONFIRMED");}
    {Fixture f(10);ai::LaneMask m;m.received=m.model_received=10.2;m.finished=10.21;m.source=m.frame_id=++f.sequence;
     m.four_class=true;m.left_curve.valid=m.right_curve.valid=true;m.left_style=m.right_style=2;
     m.left_evidence.reason=m.right_evidence.reason="CONFIRMED_CANDIDATE";
     f.control.observe_lane(m,10.21);f.control.key('j',10.23);assert(!f.control.lane_decision(10.24).allowed);}
    {Fixture f(11);auto e=f.control.explanation(10.24);assert(e.vehicle_moving&&e.vehicle_arrow==1);f.control.key('s',10.25);assert(!f.control.explanation(10.25).vehicle_moving&&f.control.explanation(10.25).vehicle_label=="停车提示");f.control.key(' ',10.26);assert(f.control.explanation(10.26).vehicle_moving);}
    {Fixture f(12);f.lights("RED","GREEN");auto e=f.control.explanation(10.24);assert(!e.vehicle_moving&&!e.vehicle_alarm&&!e.audible_alarm);f.control.key(' ',10.25);assert(f.control.explanation(10.25).vehicle_alarm&&f.control.explanation(10.25).audible_alarm);f.control.key('s',10.26);assert(!f.control.explanation(10.26).vehicle_alarm&&!f.control.explanation(10.26).audible_alarm);}
    {Fixture f(13);f.views[2].image_fresh=true;f.views[2].proximity=2;f.control.set_surround(f.views);
     auto e=f.control.explanation(10.23);assert(e.severity<2&&e.vehicle_moving&&!f.control.toolbar_alarm(10.23)&&!e.audible_alarm&&e.near_levels[2]==2);
     f.control.key(' ',10.235);assert(!f.control.explanation(10.24).vehicle_alarm); // GO is road intent, not a side manoeuvre.
     f.control.key('m',10.24);assert(f.control.lane_decision(10.25).allowed);assert(f.control.explanation(10.25).severity<2); // Opposite side remains passive.
     f.control.key('j',10.26);e=f.control.explanation(10.27);assert(e.severity==3&&e.vehicle_alarm&&e.vehicle_arrow==-1&&f.control.toolbar_alarm(10.27));
     assert(std::string(f.control.lane_decision(10.27).rule)=="BLOCKED_SIDE_OCCUPIED"&&e.audible_alarm);
     f.control.key('l',10.28);e=f.control.explanation(10.29);assert(e.severity<2&&e.vehicle_moving&&!f.control.toolbar_alarm(10.29));}
    {Fixture f(14);f.views[1].image_fresh=true;f.views[1].proximity=1;f.control.set_surround(f.views);f.control.key('m',10.23);
     assert(std::string(f.control.lane_decision(10.24).rule)=="BLOCKED_SIDE_OCCUPIED");assert(f.control.explanation(10.24).vehicle_alarm);}
    {Fixture f(15);f.views[3].image_fresh=true;f.views[3].image_approaching=true;f.control.set_surround(f.views);
     assert(f.control.explanation(10.24).severity==3&&f.control.explanation(10.24).vehicle_state=="ALERT");assert(!f.control.lane_decision(10.24).requested);}
    {Fixture f(16);driver_demo::Explanation published;f.control.tick(10.24,&published);auto expected=f.control.explanation(10.24);
     assert(published.vehicle_state==expected.vehicle_state&&published.signal==expected.signal&&published.lane==expected.lane);
     assert(published.ui_alarm==f.control.toolbar_alarm(10.24));
     f.control.key('s',10.25);f.control.tick(10.26,&published);assert(published.manual_stop&&!published.vehicle_moving);}
    // A confirmed side hazard must block even without lane-type evidence.
    int fixture=100;
    for(int direction:{0,2})for(int style:{0,1,2})for(int near:{0,1,2})for(bool fresh:{false,true}){
        Fixture f(fixture++);f.lane(10.21,style,style);
        unsigned ch=direction==0?camera_roles::Left:camera_roles::Right;
        f.views[ch].image_fresh=fresh;f.views[ch].fresh=fresh;f.views[ch].proximity=near;f.control.set_surround(f.views);
        assert(!f.control.explanation(10.23).audible_alarm); // Passive side observations do not beep.
        f.control.key(direction==0?'j':'m',10.24);
        auto d=f.control.lane_decision(10.25);auto e=f.control.explanation(10.25);
        bool blocked=style==1||(fresh&&near>0);
        assert(d.alarm==blocked&&e.vehicle_alarm==blocked&&e.audible_alarm==blocked);
        if(style==1)assert(std::string(d.rule)=="ALARM_SOLID_REQUEST");
        else if(fresh&&near>0)assert(std::string(d.rule)=="BLOCKED_SIDE_OCCUPIED"&&!d.allowed);
        f.control.key('l',10.26);assert(!f.control.explanation(10.27).audible_alarm);
    }
    {Fixture f(200);f.lane(10.21,1,1);f.lights("RED","GREEN");f.control.key('m',10.23);
     assert(std::string(f.control.lane_decision(10.24).rule)=="ALARM_SOLID_REQUEST"&&f.control.explanation(10.24).audible_alarm);}
    {Fixture f(202);f.views[3].image_fresh=f.views[3].image_approaching=true;f.control.set_surround(f.views);
     auto e=f.control.explanation(10.24);assert(e.vehicle_state=="ALERT"&&e.vehicle_alarm&&e.audible_alarm&&e.vehicle_label=="报警");}
    {Fixture f(203);f.lane(10.21,1,1);f.control.key('a',10.23);auto e=f.control.explanation(10.24);
     assert(!e.lane_requested&&!e.vehicle_alarm&&e.vehicle_label=="路口左转意图");}
    {Fixture f(204);std::vector<uint8_t> image(1280*720*2);
     f.control.observe(image.data(),1,1,10.22,10.23,true);f.control.key('j',10.24);
     f.control.observe(image.data(),2,1001,10.3,10.31,true);
     assert(f.control.lane_decision(10.32).requested&&!f.control.lane_decision(10.32).allowed);
     f.control.reset_scene(Fixture::config(),10.4);assert(!f.control.lane_decision(10.41).requested);}
    {Fixture f(205);f.control.key('j',10.23);f.control.focus_lost(10.24);assert(f.control.lane_decision(10.25).requested&&!f.control.explanation(10.25).manual_stop);}
    {Fixture f(206);f.control.key('s',10.23);assert(f.control.explanation(10.24).manual_stop);
     f.control.reset_scene(Fixture::config(),10.25);f.front(10.3);assert(!f.control.explanation(10.32).manual_stop&&f.control.explanation(10.32).vehicle_state=="MOVING");
     f.control.key('s',10.33);f.control.set_still_image(true,10.34);f.front(10.4);assert(!f.control.explanation(10.42).manual_stop&&f.control.explanation(10.42).vehicle_state=="STILL");}
    {Fixture f(207);f.views[2].calibrated=f.views[3].calibrated=false;f.control.set_surround(f.views);f.control.key('j',10.23);
     assert(!f.control.lane_decision(10.24).allowed);
     f.views[2].image_clear=f.views[3].image_clear=true;f.control.set_surround(f.views);
     assert(f.control.lane_decision(10.24).allowed&&f.control.explanation(10.24).action=="允许变道 / CHANGE");
     f.views[2].image_clear=false;f.control.set_surround(f.views);assert(!f.control.lane_decision(10.25).allowed);
     f.views[2].image_clear=true;f.views[3].image_clear=false;f.control.set_surround(f.views);assert(!f.control.lane_decision(10.26).allowed);
     f.views[3].image_clear=true;f.views[2].image_fresh=true;f.views[2].proximity=1;f.control.set_surround(f.views);assert(f.control.lane_decision(10.27).alarm&&!f.control.lane_decision(10.27).allowed);}
    std::cout<<"PASS unified signal/lane/side/rear rules, 36 side-hazard combinations, explicit turn/lane controls, slow/stop priorities and replay intent retention\n";
}
