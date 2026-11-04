#pragma once
#include "tracking.hpp"
#include "curve.hpp"
#include <string>

namespace driver_demo {
// Image-space demonstration only. No metric distance, speed, TTC or actuator.
struct Corridor {Curve left,right;double received=-1;bool valid=false;};
struct Risk {
    int level=0,category=-1; // 0 observe, 1 simulated slow, 2 simulated stop
    uint64_t target=0;double received=-1,height=0,bottom=0,growth=0;
    bool assumed_region=true,held=false,still=false;
    const char* reason="NO_FRONT_EVIDENCE";
};
class RiskState {
    Risk value,last_stop;
    uint64_t target=0;double last=-1,height=0,first=-1,stop_stamp=-1;
    unsigned samples=0;int candidate=0;
public:
    void reset(){*this=RiskState{};}
    bool observe(const std::vector<ai::TrackedBox>& tracks,double received,double now,const Corridor& road,bool still){
        if(!std::isfinite(received)||!std::isfinite(now)||received>now||now-received>.4||received<=last)return false;
        const ai::TrackedBox* chosen=nullptr;bool assumed=true;double rank=-1;
        for(const auto& item:tracks){const auto& b=item.box;
            if(item.predicted||!item.id||b.category<0||b.category>6||b.score<(b.category==0?.35f:.30f))continue;
            if(!std::isfinite(b.x1)||!std::isfinite(b.x2)||!std::isfinite(b.y1)||!std::isfinite(b.y2)||b.x2<=b.x1||b.y2<=b.y1)continue;
            double x=(b.x1+b.x2)/2./1280.,bottom=b.y2/720.,h=(b.y2-b.y1)/720.;
            if(bottom<.55||bottom>1.01||h<.06)continue;
            double y=b.y2/2.,left=0,right=0;bool geometry=road.valid&&now-road.received>=0&&now-road.received<=.5&&
                y>=road.left.ymin&&y<=road.left.ymax&&y>=road.right.ymin&&y<=road.right.ymax;
            if(geometry){left=road.left.x(y)/640.;right=road.right.x(y)/640.;geometry=left<right&&right-left>.08&&right-left<.85;}
            if(!geometry){double half=.08+.32*std::clamp((bottom-.5)/.5,0.,1.);left=.5-half;right=.5+half;}
            if(x<left||x>right)continue;
            double score=bottom+h*.3;if(score>rank){rank=score;chosen=&item;assumed=!geometry;}
        }
        const double previous=last;last=received;
        if(!chosen){target=0;samples=0;first=-1;value={};value.received=received;value.still=still;value.reason="NO_TARGET_IN_IMAGE_CORRIDOR";return true;}
        const auto& b=chosen->box;double h=(b.y2-b.y1)/720.,bottom=b.y2/720.;bool continuous=chosen->id==target&&previous>=0&&received-previous<=.5;
        double growth=continuous&&!still&&height>.03?(h-height)/(received-previous)/height:0;
        int next=h>=.40&&bottom>=.90?2:(h>=.22&&bottom>=.75)||(b.category==0&&h>=.10&&bottom>=.72)||(continuous&&!still&&growth>.2&&h>.18)?1:0;
        if(!continuous||candidate!=next){first=received;samples=0;}
        candidate=next;target=chosen->id;height=h;++samples;
        bool confirmed=samples>=3&&received-first>=.18;
        value={};value.category=b.category;value.target=target;value.received=received;value.height=h;value.bottom=bottom;
        value.growth=growth;value.assumed_region=assumed;value.still=still;value.level=confirmed?next:0;
        value.reason=!confirmed?"TARGET_CONFIRMING":next==2?"VERY_CLOSE_IMAGE_TARGET":next==1?(b.category==0?"PERSON_IN_IMAGE_PATH":"NEAR_OR_EXPANDING_IMAGE_TARGET"):"TARGET_PRESENT_NO_NEAR_RULE";
        if(value.level==2){stop_stamp=received;last_stop=value;}
        return true;
    }
    Risk decision(double now)const{
        Risk out=value;
        if(out.received<0||now<out.received||now-out.received>.4){out={};out.reason="FRONT_DETECTION_EXPIRED";}
        // A short restrictive grace can bridge a missing box; never grants motion.
        if(out.level<2&&stop_stamp>=0&&now>=stop_stamp&&now-stop_stamp<=.6){out=last_stop;out.held=true;out.reason="BOUNDED_CLOSE_TARGET_HOLD";}
        return out;
    }
};
}
