#pragma once
#include "detection.hpp"
#include <cmath>
#include <cstdint>

namespace ai {
// Dependency-free two-stage IoU tracker, NOT the official ByteTrack implementation.
// All times are source-frame receive times, not display FPS or frame counts.
struct TrackingOptions {bool enabled=true;float alpha=.65f;float light_confidence=0;}; // 0: inherit legacy policy
struct TrackedBox {
    Box box;
    uint64_t id=0;
    bool predicted=false;
    double observed=0;
};
class RoadTracker {
    struct Track {
        Box measured,smoothed;
        uint64_t id=0;
        unsigned hits=1;
        double seen=0;
        float vx=0,vy=0;
    };
    std::vector<Track> tracks_;
    uint64_t next_id_=1;
    double previous_=0;
    float alpha_=.65f;
    static constexpr double Keep=.6,Predict=.10;
    static Box shifted(Box b,float dx,float dy){
        b.x1+=dx;b.x2+=dx;b.y1+=dy;b.y2+=dy;return b;
    }
    static Box bounded(Box b){
        b.x1=std::clamp(b.x1,0.f,1279.f);b.x2=std::clamp(b.x2,0.f,1280.f);
        b.y1=std::clamp(b.y1,0.f,719.f);b.y2=std::clamp(b.y2,0.f,720.f);return b;
    }
    static bool valid(const Box& b){
        return std::isfinite(b.x1)&&std::isfinite(b.y1)&&std::isfinite(b.x2)&&std::isfinite(b.y2)&&
            std::isfinite(b.score)&&b.category>=0&&b.category<8&&b.x2>b.x1&&b.y2>b.y1;
    }
public:
    explicit RoadTracker(float alpha=.65f):alpha_(alpha){
        if(!std::isfinite(alpha)||alpha<=0||alpha>1)throw std::runtime_error("tracking alpha must be in (0,1]");
    }
    void reset(){tracks_.clear();previous_=0;}
    std::vector<TrackedBox> update(const std::vector<Box>& input,double stamp,float high=.25f){
        if(!std::isfinite(stamp)||stamp<=0)throw std::runtime_error("invalid tracking timestamp");
        if(previous_ && stamp<=previous_)return {}; // never advance on duplicates/out-of-order frames
        if(previous_ && stamp-previous_>Keep)reset();
        previous_=stamp;
        tracks_.erase(std::remove_if(tracks_.begin(),tracks_.end(),[&](const Track&t){return stamp-t.seen>Keep;}),tracks_.end());
        float low=std::min(.1f,high*.4f);
        std::vector<Box> boxes;
        for(const auto& b:input)if(valid(b)&&b.score>=low){auto clipped=bounded(b);if(clipped.x2<=clipped.x1||clipped.y2<=clipped.y1)continue;
            boxes.push_back(clipped);if(boxes.size()==300)break;}
        std::vector<int> matches(tracks_.size(),-1);
        std::vector<bool> used(boxes.size(),false);
        struct Edge {float iou;size_t track,box;};
        for(int stage=0;stage<2;++stage){
            std::vector<Edge> edges;
            for(size_t t=0;t<tracks_.size();++t){
                const auto& track=tracks_[t];
                if(matches[t]>=0 || (stage==1 && track.hits<2))continue;
                double dt=stamp-track.seen;
                auto estimate=bounded(shifted(track.measured,track.vx*float(dt),track.vy*float(dt)));
                for(size_t b=0;b<boxes.size();++b){
                    if(used[b]||boxes[b].category!=track.measured.category||(boxes[b].score>=high)!=(stage==0))continue;
                    float score=overlap(estimate,boxes[b]);
                    if(score>=.25f)edges.push_back({score,t,b});
                }
            }
            std::stable_sort(edges.begin(),edges.end(),[](const Edge&a,const Edge&b){return a.iou>b.iou;});
            for(const auto& e:edges)if(matches[e.track]<0 && !used[e.box]){matches[e.track]=int(e.box);used[e.box]=true;}
        }
        std::vector<TrackedBox> result;
        for(size_t t=0;t<tracks_.size();++t){
            auto& track=tracks_[t];double dt=stamp-track.seen;
            if(matches[t]>=0){
                auto b=boxes[size_t(matches[t])];
                if(dt>0){
                    float vx=((b.x1+b.x2)-(track.measured.x1+track.measured.x2))/float(2*dt);
                    float vy=((b.y1+b.y2)-(track.measured.y1+track.measured.y2))/float(2*dt);
                    track.vx=.5f*track.vx+.5f*std::clamp(vx,-1280.f,1280.f);
                    track.vy=.5f*track.vy+.5f*std::clamp(vy,-720.f,720.f);
                }
                // At low/irregular inference Hz, do not drag a one-second-old display box.
                float a=1-std::pow(1-alpha_,float(dt/.1));
                auto old=track.smoothed;track.smoothed=b;
                track.smoothed.x1=a*b.x1+(1-a)*old.x1;track.smoothed.x2=a*b.x2+(1-a)*old.x2;
                track.smoothed.y1=a*b.y1+(1-a)*old.y1;track.smoothed.y2=a*b.y2+(1-a)*old.y2;
                track.measured=b;track.seen=stamp;++track.hits;
                if(track.hits>=2)result.push_back({track.smoothed,track.id,false,stamp});
            }else if(track.hits>=2 && dt<=Predict){
                auto predicted=bounded(shifted(track.smoothed,track.vx*float(dt),track.vy*float(dt)));
                if(predicted.x2>predicted.x1 && predicted.y2>predicted.y1)result.push_back({predicted,track.id,true,track.seen});
            }
        }
        for(size_t b=0;b<boxes.size();++b)if(!used[b]&&boxes[b].score>=high&&tracks_.size()<128)
            tracks_.push_back({boxes[b],boxes[b],next_id_++,1,stamp,0,0});
        return result;
    }
};
}
