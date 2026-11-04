#pragma once
#include "detection.hpp"
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace parking {
// ai::Detector::infer() returns the canonical 1280x720 source coordinates.
// Its decoder has ALREADY removed the model's 12px padding and multiplied by 2.
// Normalize once; the web maps this unit square to a 640x360 mosaic tile.
inline ai::Box normalized_detector_box(ai::Box box){
    box.x1/=1280.f;box.x2/=1280.f;box.y1/=720.f;box.y2/=720.f;return box;
}
// Image-space demonstration thresholds, not calibrated metres or collision TTC.
struct Rules {
    bool enabled=true;double sweep_hz=0,confidence=.25; // 0: no inference rate cap
    double person_yellow=.025,person_red=.08,vehicle_yellow=.08,vehicle_red=.22;
    double yellow_y=.35,red_y=.65,confirm_s=.45,hold_s=1.2,cooldown_s=10;
    std::string revision="defaults";
    bool valid() const {
        for(double v:{sweep_hz,confidence,person_yellow,person_red,vehicle_yellow,vehicle_red,yellow_y,red_y,confirm_s,hold_s,cooldown_s})if(!std::isfinite(v))return false;
        return (sweep_hz==0||(sweep_hz>=1&&sweep_hz<=4))&&confidence>=.05&&confidence<=.9&&
          person_yellow>=.001&&person_red>person_yellow&&person_red<=.8&&
          vehicle_yellow>=.001&&vehicle_red>vehicle_yellow&&vehicle_red<=.8&&
          yellow_y>=0&&red_y>yellow_y&&red_y<=1&&confirm_s>=.1&&confirm_s<=3&&
          hold_s>=.2&&hold_s<=5&&cooldown_s>=2&&cooldown_s<=120;
    }
};
struct Target {
    unsigned id=0;int category=0,level=0,desired=0,observations=0;
    ai::Box raw,box;double area=0,foot=0,first_red=-1,last_seen=0,last_red=-1,last_alert=-1000;
    double pending_s=0;bool held=false;uint64_t event=0;
};
inline double overlap(const ai::Box& a,const ai::Box& b){
    double inter=std::max(0.f,std::min(a.x2,b.x2)-std::max(a.x1,b.x1))*std::max(0.f,std::min(a.y2,b.y2)-std::max(a.y1,b.y1));
    double aa=std::max(0.f,a.x2-a.x1)*std::max(0.f,a.y2-a.y1),bb=std::max(0.f,b.x2-b.x1)*std::max(0.f,b.y2-b.y1);
    return inter/std::max(1e-9,aa+bb-inter);
}
class Engine {
    std::array<std::vector<Target>,4> tracks;
    unsigned next_id=0;uint64_t next_event=0;Rules rules;
public:
    explicit Engine(const Rules& value={}):rules(value){}
    void configure(const Rules& value){rules=value;for(auto& x:tracks)x.clear();}
    const Rules& config()const{return rules;}
    std::vector<Target> update(unsigned ch,const std::vector<ai::Box>& boxes,double now,bool image_valid){
        if(ch>=4||!rules.valid())return {};
        auto previous=tracks[ch];std::vector<Target> current;std::vector<bool> used(previous.size());
        if(!image_valid||!rules.enabled){tracks[ch].clear();return {};}
        for(auto b:boxes){
            if(b.category<0||b.category>6||!std::isfinite(b.score)||b.score<rules.confidence)continue;
            if(!std::isfinite(b.x1)||!std::isfinite(b.y1)||!std::isfinite(b.x2)||!std::isfinite(b.y2))continue;
            b.x1=std::clamp(b.x1,0.f,1.f);b.y1=std::clamp(b.y1,0.f,1.f);b.x2=std::clamp(b.x2,0.f,1.f);b.y2=std::clamp(b.y2,0.f,1.f);
            if(b.x2<=b.x1||b.y2<=b.y1)continue;
            int match=-1;double best=.2;
            for(size_t i=0;i<previous.size();++i)if(!used[i]&&previous[i].category==b.category&&now-previous[i].last_seen<=1.25){double value=parking::overlap(previous[i].raw,b);if(value>best){best=value;match=int(i);}}
            Target t;if(match>=0){t=previous[size_t(match)];used[size_t(match)]=true;}else{t.id=++next_id;t.category=b.category;t.box=b;}
            const bool small=b.category==0||b.category==1||b.category==5||b.category==6;
            double yellow=small?rules.person_yellow:rules.vehicle_yellow,red=small?rules.person_red:rules.vehicle_red;
            t.area=(b.x2-b.x1)*(b.y2-b.y1);t.foot=b.y2;t.raw=b;t.held=false;t.last_seen=now;++t.observations;
            t.box.x1=float(.45*t.box.x1+.55*b.x1);t.box.y1=float(.45*t.box.y1+.55*b.y1);
            t.box.x2=float(.45*t.box.x2+.55*b.x2);t.box.y2=float(.45*t.box.y2+.55*b.y2);t.box.score=b.score;t.box.category=b.category;
            t.desired=t.area>=red&&t.foot>=rules.red_y?2:t.area>=yellow&&t.foot>=rules.yellow_y?1:0;
            if(t.level==2&&now-t.last_red>=rules.hold_s)t.level=0;
            if(t.desired==2){
                if(t.first_red<0)t.first_red=now;
                t.pending_s=now-t.first_red;
                if(t.pending_s+1e-6>=rules.confirm_s&&t.observations>=2){
                    if(t.level!=2&&now-t.last_alert>=rules.cooldown_s){t.event=++next_event;t.last_alert=now;}
                    t.level=2;t.last_red=now;
                }else t.level=std::max(t.level,1);
            }else{
                t.first_red=-1;t.pending_s=0;
                t.level=t.level==2&&now-t.last_red<rules.hold_s?2:t.desired;
            }
            current.push_back(t);if(current.size()==12)break;
        }
        // A short, visibly marked missed-detection hold. It never refreshes seen time.
        for(size_t i=0;i<previous.size()&&current.size()<12;++i)if(!used[i]&&now-previous[i].last_seen<=.75){
            auto t=previous[i];t.held=true;t.first_red=-1;t.pending_s=0;
            if(t.level==2&&now-t.last_red>=rules.hold_s)t.level=0;
            current.push_back(t);
        }
        tracks[ch]=current;return current;
    }
};
}
