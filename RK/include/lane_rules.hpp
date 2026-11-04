#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
namespace lane_demo {
// Two-state, variable-dt constant-velocity Kalman filter. Observation units are
// normalized half-lane width. Prediction is display-only and capped at 250 ms.
class OffsetFilter {
    double x=0,v=0,p00=.01,p01=0,p11=.25,t=-1;
public:
    void reset(){t=-1;}
    void update(double z,double stamp){
        if(t<0||stamp-t>1.0){x=z;v=0;p00=.01;p01=0;p11=.25;t=stamp;return;}
        double dt=stamp-t;if(dt<=0)return;
        double a=p00+2*dt*p01+dt*dt*p11+.16*dt*dt*dt*dt/4;
        double b=p01+dt*p11+.16*dt*dt*dt/2,c=p11+.16*dt*dt;
        double innovation=z-(x+v*dt),k0=a/(a+.01),k1=b/(a+.01);
        x+=v*dt+k0*innovation;v=std::clamp(v+k1*innovation,-.6,.6);
        p00=(1-k0)*a;p01=(1-k0)*b;p11=std::max(1e-8,c-k1*b);t=stamp;
    }
    double predict(double now)const{return x+v*std::clamp(now-t,0.,.25);}
};
struct Input {
    uint32_t frame_id=0,source=0;
    double received=-1;
    bool valid=false,propagated=false,left_valid=false,right_valid=false;
    bool pixels_seen=false; // Display evidence only; never authorizes a lane change.
    bool solid_pixels=false,dashed_pixels=false;
    double left=0,right=0,ego=320,offset=0;
    int left_style=0,right_style=0; // 1 solid, 2 dashed; fresh independently confirmed semantics.
    std::string pair_reason="UNSPECIFIED",left_reason="UNSPECIFIED",right_reason="UNSPECIFIED";
};
struct Decision {
    const char* motion="UNKNOWN";
    const char* raw_motion="UNKNOWN";
    const char* trend="UNKNOWN";
    const char* rule="NO_REQUEST";
    const char* reason="NO_LANE_EVIDENCE";
    const char* raw_reason="NO_LANE_EVIDENCE";
    bool requested=false,marking_permits=false,alarm=false;
    bool side_verified=false,allowed=false; // No calibrated adjacent-lane clearance.
    bool raw_valid=false,position_held=false,style_held=false;
    double age_ms=-1,geometry_age_ms=-1,offset=0,raw_offset=0,unknown_run_ms=0;
    uint64_t observations=0,unknown_observations=0;
    int direction=1,left_style=0,right_style=0;
};
class State {
    Input latest,good;
    OffsetFilter filter;
    double hold_ms,last_received=-1,candidate_start=-1,unknown_start=-1;
    unsigned candidate_samples=0,trend_samples=0;
    uint64_t observations=0,unknown_observations=0;
    int candidate_code=0,display_code=0,direction=1,trend_sign=0;
    bool requested=false,alarm=false,filter_restart=false,still_test=false;
    const char* quality="NO_LANE_EVIDENCE";
    static int zone(double x){return x<=-1?-3:x>=1?3:x<-.65?-2:x>.65?2:x<-.2?-1:x>.2?1:0;}
    static const char* motion_name(int c){switch(c){case -3:return "CROSS_LEFT_CANDIDATE";case 3:return "CROSS_RIGHT_CANDIDATE";
        case -2:return "NEAR_LEFT_BOUNDARY";case 2:return "NEAR_RIGHT_BOUNDARY";case -1:return "LEFT_OF_CENTER";case 1:return "RIGHT_OF_CENTER";default:return "CENTERED";}}
public:
    explicit State(double position_hold_ms=1000):hold_ms(position_hold_ms){if(!std::isfinite(hold_ms)||hold_ms<0||hold_ms>1500)throw std::runtime_error("Lane position hold must be 0..1500ms");}
    void set_still_test(bool value){still_test=value;trend_samples=0;trend_sign=0;}
    void stop(){requested=false;alarm=false;}
    void reset(){latest={};good={};filter.reset();last_received=candidate_start=unknown_start=-1;candidate_samples=trend_samples=0;candidate_code=display_code=trend_sign=0;observations=unknown_observations=0;filter_restart=false;quality="NO_LANE_EVIDENCE";stop();}
    void select(int d){if(d!=direction){direction=d;stop();}}
    void request(){requested=true;}
    bool observe(const Input& in,double now){
        if(!std::isfinite(in.received)||!std::isfinite(now)||in.received<0||in.received>now||in.received<=last_received)return false;
        // A CPU-propagated mask is not another model observation and cannot poison its timestamp.
        if(in.propagated)return false;
        const bool contiguous=last_received>=0&&in.received-last_received<=.9;
        Input previous=latest;last_received=in.received;latest=in;++observations;
        bool valid=in.valid&&(now-in.received)<=.75&&std::isfinite(in.ego)&&
            std::isfinite(in.left)&&std::isfinite(in.right)&&std::isfinite(in.offset)&&in.right-in.left>76.8&&in.right-in.left<576&&
            std::abs(in.offset-(in.ego-(in.left+in.right)/2)/((in.right-in.left)/2))<.05;
        quality=valid?"IMAGE_AXIS_POSITION_NOT_VEHICLE_POSE":"INVALID_OR_STALE_PAIR";
        if(valid&&contiguous&&previous.valid){double ratio=(in.right-in.left)/(previous.right-previous.left);
            if(ratio<.65||ratio>1.55||std::abs(in.offset-previous.offset)>.45){valid=false;quality="PAIR_JUMP_RECONFIRM";filter_restart=true;}}
        latest.valid=valid;
        if(!valid){++unknown_observations;if(unknown_start<0)unknown_start=in.received;candidate_samples=trend_samples=0;candidate_start=-1;return true;}
        unknown_start=-1;
        if(!still_test&&contiguous&&previous.valid){double rate=(in.offset-previous.offset)/(in.received-previous.received);int sign=rate<-.1?-1:rate>.1?1:0;
            if(sign!=trend_sign){trend_sign=sign;trend_samples=0;}++trend_samples;
        }else trend_samples=0;
        good=latest;if(filter_restart){filter.reset();filter_restart=false;}filter.update(in.offset,in.received);
        int code=zone(in.offset);
        if(!contiguous||candidate_start<0||code!=candidate_code){candidate_start=in.received;candidate_samples=0;candidate_code=code;}
        ++candidate_samples;
        // No second position confirmation on top of upstream geometry. Critical crossing
        // alerts retain a separate multi-frame confirmation below.
        int next=zone(filter.predict(in.received));
        double threshold=std::abs(display_code)==3?.85:std::abs(display_code)==2?.55:std::abs(display_code)==1?.14:0;
        if(next==display_code||display_code==0||next*display_code<0||std::abs(next)>std::abs(display_code)||std::abs(filter.predict(in.received))<threshold)display_code=next;
        return true;
    }
    Decision decision(double now,bool signal_stop=false){
        Decision d;d.requested=requested;d.direction=direction;d.alarm=alarm;d.raw_offset=latest.offset;d.reason=d.raw_reason=quality;
        d.observations=observations;d.unknown_observations=unknown_observations;
        d.age_ms=latest.received<0?-1:(now-latest.received)*1000;
        d.geometry_age_ms=good.received<0?-1:(now-good.received)*1000;
        bool semantic_fresh=std::isfinite(now)&&d.age_ms>=0&&d.age_ms<=750;
        if(!semantic_fresh&&latest.received>=0)d.raw_reason="MODEL_INPUT_EXPIRED";
        bool fresh=latest.valid&&semantic_fresh;
        d.raw_valid=fresh;if(fresh)d.raw_motion=motion_name(zone(latest.offset));
        // Display/prohibition may bridge a short scheduling gap. A held dashed
        // class never authorizes a manoeuvre; fresh geometry remains required.
        bool class_fresh=std::isfinite(now)&&d.age_ms>=0&&d.age_ms<=std::max(750.,std::min(1000.,hold_ms));
        if(class_fresh){if(latest.left_valid)d.left_style=latest.left_style;if(latest.right_valid)d.right_style=latest.right_style;}
        d.style_held=class_fresh&&!semantic_fresh&&(d.left_style||d.right_style);
        bool held=!fresh&&d.geometry_age_ms>=0&&d.geometry_age_ms<=hold_ms&&hold_ms>0;
        if(fresh||held){d.position_held=held;d.offset=filter.predict(now);d.motion=motion_name(display_code);
            d.trend=still_test?"STATIC_IMAGE_NO_MOTION":held?"HELD_NO_NEW_MOTION":trend_samples>=2?(trend_sign<0?"DRIFTING_LEFT":trend_sign>0?"DRIFTING_RIGHT":"STEADY_IMAGE_OFFSET"):"TREND_CONFIRMING";
        }
        if(!fresh)d.reason=held?"TEMPORARY_GEOMETRY_GAP":"LANE_UNKNOWN_OR_EXPIRED";
        d.unknown_run_ms=unknown_start>=0?std::max(0.,now-unknown_start)*1000:!fresh&&latest.received>=0?std::max(0.,now-latest.received-.75)*1000:0;
        if(requested){
            int style=direction==0?d.left_style:direction==2?d.right_style:0;
            if(direction==1)d.rule="SELECT_LEFT_OR_RIGHT";
            else if(style==1){alarm=true;d.rule="ALARM_SOLID_REQUEST";}
            else if(signal_stop)d.rule="BLOCKED_SIGNAL_STOP";
            else if(style==2&&!d.style_held){d.marking_permits=true;d.rule="MARKING_OK_SIDE_UNVERIFIED";}
            else if(style==2)d.rule="WAIT_LANE_EVIDENCE";
            else d.rule="WAIT_LINE_TYPE_UNKNOWN";
        }
        d.alarm=alarm;
        if(alarm){d.marking_permits=false;d.rule="ALARM_SOLID_REQUEST";}
        if(!still_test&&fresh&&candidate_samples>=2&&latest.received-candidate_start>=.3-1e-7&&
           ((candidate_code==-3&&d.left_style==1)||(candidate_code==3&&d.right_style==1))){
            d.rule="SOLID_CROSS_CANDIDATE";d.marking_permits=false;
        }
        return d;
    }
};
}
