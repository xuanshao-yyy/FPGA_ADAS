#pragma once
#include "ai_worker.hpp"
#include "parking_guard.hpp"
#include <deque>
#include <map>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace product {
struct Zone {
    bool verified=false,metric_verified=false;
    std::vector<cv::Point2f> near,ground_image;
    cv::Mat ground;
    double area_vehicle=.08,area_small=.025,near_m=3,stop_m=1.5;
};
struct Profile {std::string id="uncalibrated",front_source;std::array<Zone,4> zones;};
inline std::vector<cv::Point2f> polygon(const cv::FileNode& node){
    std::vector<cv::Point2f> out;
    if(node.empty())return out;
    if(!node.isSeq()||node.size()<3||node.size()>12)throw std::runtime_error("ROI requires 3..12 points");
    for(auto point:node){std::vector<double> p;point>>p;
        if(p.size()!=2||!std::isfinite(p[0])||!std::isfinite(p[1])||p[0]<0||p[0]>1||p[1]<0||p[1]>1)throw std::runtime_error("ROI coordinates must be normalized");
        out.emplace_back(float(p[0]),float(p[1]));}
    if(std::abs(cv::contourArea(out))<.005||!cv::isContourConvex(out))throw std::runtime_error("ROI must be convex and have usable area");
    return out;
}
inline Profile load_profile(const std::string& path){
    cv::FileStorage fs(path,cv::FileStorage::READ);if(!fs.isOpened())throw std::runtime_error("Cannot read risk profile");
    Profile p;fs["id"]>>p.id;if(p.id.empty()||p.id.size()>80)throw std::runtime_error("Invalid calibration id");
    if(!fs["front_source"].empty())fs["front_source"]>>p.front_source;
    auto channels=fs["channels"];if(!channels.isSeq()||channels.size()!=4)throw std::runtime_error("Calibration needs four channels");
    for(unsigned i=0;i<4;++i){auto n=channels[int(i)];auto& z=p.zones[i];int valid=0,metric=0;n["verified"]>>valid;n["metric_verified"]>>metric;
        z.verified=valid==1;z.metric_verified=metric==1;z.near=polygon(n["near_polygon"]);
        if(z.verified&&z.near.empty())throw std::runtime_error("Verified ROI is empty");
        if(!n["area_vehicle"].empty())n["area_vehicle"]>>z.area_vehicle;
        if(!n["area_small"].empty())n["area_small"]>>z.area_small;
        if(!n["near_m"].empty())n["near_m"]>>z.near_m;
        if(!n["stop_m"].empty())n["stop_m"]>>z.stop_m;
        if(!std::isfinite(z.area_vehicle)||!std::isfinite(z.area_small)||z.area_vehicle<.005||z.area_vehicle>.6||z.area_small<.002||z.area_small>.4||!std::isfinite(z.stop_m)||!std::isfinite(z.near_m)||z.stop_m<=0||z.near_m<=z.stop_m)throw std::runtime_error("Invalid risk thresholds");
        if(z.metric_verified){
            z.ground_image=polygon(n["ground_image"]);std::vector<cv::Point2f> world;
            for(auto point:n["ground_world"]){std::vector<double> xy;point>>xy;
                if(xy.size()!=2||!std::isfinite(xy[0])||!std::isfinite(xy[1])||std::abs(xy[0])>100||xy[1]<0||xy[1]>100)throw std::runtime_error("Invalid measured ground points");
                world.emplace_back(float(xy[0]),float(xy[1]));}
            if(z.ground_image.size()!=4||world.size()!=4||!cv::isContourConvex(world)||std::abs(cv::contourArea(world))<.05)throw std::runtime_error("Metric calibration requires four convex measured points");
            z.ground=cv::getPerspectiveTransform(z.ground_image,world);
            if(!cv::checkRange(z.ground)||std::abs(cv::determinant(z.ground))<1e-10)throw std::runtime_error("Singular ground calibration");
        }
    }
    return p;
}
inline parking::Rules load_near_rules(const std::string& path){
    cv::FileStorage file(path,cv::FileStorage::READ);if(!file.isOpened())throw std::runtime_error("Missing drive near rules");parking::Rules r;
    int enabled=1;file["enabled"]>>enabled;r.enabled=enabled!=0;if(!file["revision"].empty())file["revision"]>>r.revision;
    auto number=[&](const char* key,double& value){if(!file[key].empty())file[key]>>value;};
    number("confidence",r.confidence);number("person_yellow",r.person_yellow);number("person_red",r.person_red);
    number("vehicle_yellow",r.vehicle_yellow);number("vehicle_red",r.vehicle_red);number("yellow_y",r.yellow_y);number("red_y",r.red_y);
    number("confirm_s",r.confirm_s);number("hold_s",r.hold_s);number("cooldown_s",r.cooldown_s);
    if(!r.valid())throw std::runtime_error("Invalid drive near rules");
    return r;
}
struct ViewRisk {bool fresh=false,calibrated=false,occupied=false,approaching=false;uint64_t target=0;double received=-1,area=0,tau=-1,distance=-1;int person=0;const char* reason="NO_OBSERVATION";
    bool image_fresh=false,proximity_held=false,image_approaching=false,image_clear=false;double clear_age_ms=0;int proximity=0;double proximity_area=0;};
class SurroundRisk {
    struct Track {double last=-1,first=-1;unsigned hits=0;std::deque<std::pair<double,double>> sizes;};
    struct Camera {double clear_since=-1;unsigned clear_samples=0;double received=-1,first=-1,last_busy=-1,last_fast=-1,person_stamp=-1,last_image_fast=-1;uint32_t source=0;unsigned samples=0;ViewRisk value,last_person;std::map<uint64_t,Track> tracks;std::vector<parking::Target> image_targets;};
    std::array<Camera,4> camera;
    parking::Rules image_rules;parking::Engine image_guard;
    std::array<double,4> quality_time{{-1,-1,-1,-1}};
    std::array<bool,4> quality_ok{};
public:
    Profile profile;
    bool require_image_quality=false;
    void image_quality(unsigned ch,bool good,double received){quality_time.at(ch)=received;quality_ok.at(ch)=good;if(!good){camera.at(ch).clear_since=-1;camera.at(ch).clear_samples=0;}}
    void reset(){camera={};quality_time={{-1,-1,-1,-1}};quality_ok={};image_guard.configure(image_rules);}
    void configure(Profile value){profile=std::move(value);reset();}
    void configure_near_rules(const parking::Rules& value){if(!value.valid())throw std::runtime_error("Invalid near rules");image_rules=value;image_guard.configure(value);for(auto& c:camera)c.image_targets.clear();}
    static bool inside(const std::vector<cv::Point2f>& polygon,double x,double y){return !polygon.empty()&&cv::pointPolygonTest(polygon,cv::Point2f(float(x),float(y)),false)>=0;}
    bool observe(unsigned ch,const ai::Result& r,double now,bool still=false){
        if(ch>=4||!r.finished||!std::isfinite(r.received)||!std::isfinite(now)||r.received>now||now-r.received>.5)return false;
        auto& c=camera[ch];if(r.received<=c.received)return false;
        bool discontinuity=c.received<0||r.received-c.received>.75||uint32_t(r.source-c.source)>=0x80000000u;
        if(discontinuity){c=Camera{};image_guard.update(ch,{},r.received,false);}
        if(c.first<0)c.first=r.received;
        c.received=r.received;c.source=r.source;++c.samples;
        auto& v=c.value;v={};v.received=r.received;v.calibrated=profile.zones[ch].verified;
        const auto& zone=profile.zones[ch];
        // Absence is a bounded image advisory, not calibrated physical clearance.
        // Count unique completed detections only; even a far visible target vetoes it.
        bool target=false;
        auto present=[](const ai::Box& b){return b.category>=0&&b.category<=6&&std::isfinite(b.score)&&b.score>=.15f&&std::isfinite(b.x1)&&std::isfinite(b.x2)&&std::isfinite(b.y1)&&std::isfinite(b.y2)&&b.x2>b.x1&&b.y2>b.y1;};
        for(const auto& b:r.boxes)target=target||present(b);
        for(const auto& t:r.tracks)if(!t.predicted)target=target||present(t.box);
        bool usable=!require_image_quality||(quality_ok[ch]&&quality_time[ch]>=0&&now>=quality_time[ch]&&now-quality_time[ch]<=1.);
        if(target||!usable){c.clear_since=-1;c.clear_samples=0;}
        else {if(c.clear_since<0)c.clear_since=r.received;++c.clear_samples;}
        if(ch){std::vector<ai::Box> boxes;for(const auto& b:r.boxes)boxes.push_back(parking::normalized_detector_box(b));
            c.image_targets=image_guard.update(ch,boxes,r.received,true);}
        for(auto it=c.tracks.begin();it!=c.tracks.end();)if(r.received-it->second.last>1.5)it=c.tracks.erase(it);else ++it;
        for(const auto& t:r.tracks){const auto& b=t.box;
            if(t.predicted||!t.id||b.score<.3f||b.category<0||b.category>6||!std::isfinite(b.x1)||!std::isfinite(b.x2)||!std::isfinite(b.y1)||!std::isfinite(b.y2)||b.x2<=b.x1||b.y2<=b.y1)continue;
            double x=(b.x1+b.x2)/2560.,bottom=b.y2/720.,h=(b.y2-b.y1)/720.,a=(b.x2-b.x1)*(b.y2-b.y1)/(1280.*720.);
            bool in=zone.verified&&inside(zone.near,x,bottom);
            auto& track=c.tracks[t.id];bool continuous=track.last>=0&&r.received-track.last<=.75;
            if(!continuous){track=Track{};track.first=r.received;}
            track.last=r.received;++track.hits;
            bool clipped=b.x1<=2||b.x2>=1278||b.y1<=2||b.y2>=718;
            if(!still&&!clipped&&h>.03){track.sizes.emplace_back(r.received,std::log(h));
                while(track.sizes.size()>8||(!track.sizes.empty()&&r.received-track.sizes.front().first>1.4))track.sizes.pop_front();
            }else track.sizes.clear();
            double tau=-1;
            if(track.sizes.size()>=3&&track.sizes.back().first-track.sizes.front().first>=.35){
                double slope=(track.sizes.back().second-track.sizes.front().second)/(track.sizes.back().first-track.sizes.front().first);
                unsigned increasing=0;for(size_t i=1;i<track.sizes.size();++i)increasing+=track.sizes[i].second>track.sizes[i-1].second;
                if(slope>.18&&increasing+1>=track.sizes.size())tau=1/slope;
            }
            bool confirmed=track.hits>=2&&r.received-track.first>=.12;
            if(ch&&confirmed&&!still&&tau>0&&tau<4.0&&a>.012)c.last_image_fast=r.received;
            bool small=b.category==1||b.category==5||b.category==6;
            if(ch&&b.category!=0&&in&&confirmed&&a>=(small?zone.area_small:zone.area_vehicle)){
                c.last_busy=r.received;v.target=t.id;v.area=std::max(v.area,a);}
            if(ch&&b.category!=0&&in&&!still&&tau>0&&tau<4.0&&a>.012){c.last_fast=r.received;v.target=t.id;v.tau=tau;}
            if(ch==0&&b.category==0&&in&&confirmed){
                int level=h>=.40&&bottom>=.90?2:h>=.10&&bottom>=.72?1:0;double distance=-1;
                if(zone.metric_verified&&!clipped&&inside(zone.ground_image,x,bottom)){
                    const auto& H=zone.ground;double w=H.at<double>(2,0)*x+H.at<double>(2,1)*bottom+H.at<double>(2,2);
                    if(std::abs(w)>1e-8){double xx=(H.at<double>(0,0)*x+H.at<double>(0,1)*bottom+H.at<double>(0,2))/w;
                        double yy=(H.at<double>(1,0)*x+H.at<double>(1,1)*bottom+H.at<double>(1,2))/w;
                        if(std::isfinite(xx)&&std::isfinite(yy)&&yy>=0){distance=std::hypot(xx,yy);level=distance<=zone.stop_m?2:distance<=zone.near_m?1:level;}}
                }
                if(level>v.person||(level==v.person&&distance>=0&&(v.distance<0||distance<v.distance))){v.person=level;v.distance=distance;v.target=t.id;}
            }
        }
        v.occupied=c.last_busy>=0&&r.received-c.last_busy<=.8;
        v.approaching=c.last_fast>=0&&r.received-c.last_fast<=.8;
        if(v.person>0){c.person_stamp=r.received;c.last_person=v;}
        return true;
    }
    ViewRisk decision(unsigned ch,double now)const{
        auto v=camera.at(ch).value;const auto& c=camera.at(ch);
        v.image_fresh=ch&&image_rules.enabled&&c.received>=0&&now>=c.received&&now-c.received<=.8&&c.samples>=2&&c.received-c.first>=.12;
        if(require_image_quality)v.image_fresh=v.image_fresh&&quality_ok[ch]&&quality_time[ch]>=0&&now>=quality_time[ch]&&now-quality_time[ch]<=1.;
        if(v.image_fresh){
            for(const auto& t:c.image_targets)if(now>=t.last_seen&&now-t.last_seen<=.75){
                int level=t.level==2&&now-t.last_red>=image_rules.hold_s?0:t.level;
                if(level>v.proximity||(level==v.proximity&&t.area>v.proximity_area)){v.proximity=level;v.proximity_area=t.area;v.proximity_held=t.held;}}
            v.image_approaching=c.last_image_fast>=0&&now-c.last_image_fast<=.8;
        }
        v.fresh=c.received>=0&&now>=c.received&&now-c.received<=.5&&c.samples>=3&&c.received-c.first>=.3;
        if(require_image_quality)v.fresh=v.fresh&&quality_ok[ch]&&quality_time[ch]>=0&&now>=quality_time[ch]&&now-quality_time[ch]<=1.0;
        if(v.fresh&&v.person<c.last_person.person&&c.person_stamp>=0&&now-c.person_stamp<=.6){v.person=c.last_person.person;v.distance=c.last_person.distance;v.target=c.last_person.target;}
        if(!v.fresh){v.occupied=false;v.approaching=false;v.person=0;v.distance=-1;}
        v.clear_age_ms=c.clear_since>=0?std::max(0.,(c.received-c.clear_since)*1000):0;
        v.image_clear=v.fresh&&v.image_fresh&&c.clear_samples>=3&&v.clear_age_ms>=600&&!v.proximity&&!v.image_approaching&&!v.occupied&&!v.approaching;
        v.reason=!v.calibrated?"UNCALIBRATED":!v.fresh?(require_image_quality&&!quality_ok[ch]?"IMAGE_UNAVAILABLE":"STALE_OR_CONFIRMING"):v.occupied?"SIDE_OCCUPIED":v.approaching?"IMAGE_APPROACH":v.person>0?"PERSON_NEAR":"NO_NEAR_RULE_TRIGGERED";
        return v;
    }
};
}
