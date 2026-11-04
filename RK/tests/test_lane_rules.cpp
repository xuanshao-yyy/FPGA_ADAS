#include "lane_rules.hpp"
#include "lane_display.hpp"
#include <cassert>
#include <iostream>
int main(){
    lane_demo::State state;unsigned source=0;
    auto input=[&](double t,double offset,int left=1,int right=2){lane_demo::Input in;in.received=t;in.frame_id=in.source=++source;in.valid=true;in.offset=offset;in.left=320-offset*120-120;in.right=in.left+240;in.left_style=left;in.right_style=right;in.left_valid=left!=0;in.right_valid=right!=0;return in;};
    auto sample=[&](double t,double offset,int left=1,int right=2){auto in=input(t,offset,left,right);state.observe(in,t+.1);return state.decision(t+.1);};
    assert(std::string(sample(10,0).motion)=="CENTERED"); // no redundant second confirmation
    sample(10.5,0);state.select(0);state.request();auto d=state.decision(10.6);assert(d.alarm&&!d.allowed&&std::string(d.rule)=="ALARM_SOLID_REQUEST");
    state.stop();state.select(2);state.request();d=state.decision(10.6);assert(d.marking_permits&&!d.allowed&&!d.side_verified);
    d=state.decision(11.3);assert(!d.marking_permits&&d.position_held&&!d.raw_valid); // old dashed cannot grant advice
    assert(std::string(state.decision(11.51).motion)=="UNKNOWN");
    state.stop();sample(12,.3);sample(12.5,.6);sample(13,.7);d=sample(13.5,.8);assert(std::string(d.motion)=="NEAR_RIGHT_BOUNDARY"&&std::string(d.trend)=="DRIFTING_RIGHT");
    sample(14,1.05);d=sample(14.5,1.1);assert(std::string(d.raw_motion)=="CROSS_RIGHT_CANDIDATE");
    d=sample(15,-.6);assert(d.position_held&&!d.raw_valid&&std::string(d.rule)!="SOLID_CROSS_CANDIDATE");
    sample(15.5,-.7);sample(16,-.8);assert(std::string(sample(16.5,-.9).motion)=="NEAR_LEFT_BOUNDARY");
    sample(17,-1.05);d=sample(17.5,-1.1);assert(std::string(d.rule)=="SOLID_CROSS_CANDIDATE");
    state.reset();sample(20,0,2,2);state.select(0);state.request();assert(std::string(state.decision(20.1,true).rule)=="BLOCKED_SIGNAL_STOP");
    auto invalid=input(20.5,0);invalid.valid=false;state.observe(invalid,20.6);d=state.decision(20.6);assert(d.position_held&&d.unknown_observations==1&&d.observations==2&&d.geometry_age_ms>599);
    invalid.received=20.9;state.observe(invalid,21);assert(std::string(state.decision(21.01).motion)=="UNKNOWN"); // UNKNOWN never renews TTL
    // Fresh trustworthy left semantics can warn even when the right boundary/pair is absent.
    state.stop();invalid=input(21.5,0);invalid.valid=false;invalid.left_style=1;invalid.left_valid=true;invalid.right_valid=false;
    state.observe(invalid,21.6);state.select(0);state.request();assert(state.decision(21.6).alarm);
    state.reset();assert(std::string(state.decision(22).motion)=="UNKNOWN"&&!state.decision(22).requested);
    auto propagated=input(22,0);propagated.propagated=true;assert(!state.observe(propagated,22.1));propagated.propagated=false;assert(state.observe(propagated,22.1));
    assert(!state.observe(propagated,22.2));propagated.received=24;assert(!state.observe(propagated,23));
    // Bounded prediction and deterministic jitter reduction, not another inference.
    lane_demo::OffsetFilter filter;double raw_error=0,smooth_error=0;
    for(int i=0;i<40;++i){double z=(i%2?.08:-.08);filter.update(z,30+i*.5);if(i>3){raw_error+=z*z;smooth_error+=std::pow(filter.predict(30+i*.5),2);}}
    assert(smooth_error<raw_error*.8);assert(filter.predict(60)==filter.predict(100));
    lane_demo::State off(0);auto anchor=input(60,0);off.observe(anchor,60.1);assert(std::string(off.decision(60.8).motion)=="UNKNOWN");
    lane_demo::CurveView view;Curve curve;curve.valid=true;curve.ymin=170;curve.ymax=350;curve.coef={0,0,200};view.observe(curve,70);
    bool held=false;assert(view.at(70.1,true,held).valid&&!held);assert(view.at(70.8,false,held).valid&&held);assert(!view.at(71.01,false,held).valid);
    view.reset();assert(!view.at(71.1,false,held).valid);
    lane_demo::State still;still.set_still_test(true);
    for(int i=0;i<4;++i){auto in=input(90+i*.5,-1.05,1,2);still.observe(in,90+i*.5+.1);auto d=still.decision(90+i*.5+.1);assert(std::string(d.trend)=="STATIC_IMAGE_NO_MOTION"&&std::string(d.rule)!="SOLID_CROSS_CANDIDATE");}
    lane_demo::State short_gap;auto last=input(100,0,1,2);short_gap.observe(last,100.1);
    auto aged=short_gap.decision(100.9);assert(aged.style_held&&!aged.raw_valid&&aged.left_style==1&&aged.right_style==2);
    short_gap.select(0);short_gap.request();assert(short_gap.decision(100.9).alarm);
    short_gap.stop();short_gap.select(2);short_gap.request();aged=short_gap.decision(100.9);assert(!aged.marking_permits&&!aged.allowed&&std::string(aged.rule)=="WAIT_LANE_EVIDENCE");
    short_gap.stop();aged=short_gap.decision(101.01);assert(!aged.style_held&&aged.left_style==0&&aged.right_style==0);
    std::cout<<"PASS bounded Kalman and style holds, no held-dashed permission, independent semantics, crossing confirmation, resets and diagnostics\n";
}
