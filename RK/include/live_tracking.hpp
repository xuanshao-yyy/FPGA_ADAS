#pragma once
#include "tracking.hpp"
#include "ByteTrack/BYTETracker.h"
#include <memory>
namespace ai {
// Time comes from completed receive frames. This is not camera exposure time.
// Kalman velocities are per nominal 1/30 second; motion step uses real dt*30.
class LiveTracker {
    std::array<std::unique_ptr<byte_track::BYTETracker>,8> banks;
    double previous=-1,origin=0;
    size_t tick=0;
    uint64_t generation=0;
    float high,light_high;
    void rebuild(){
        ++generation;
        if(generation>0x7ffffffu)throw std::runtime_error("Tracking generation overflow");
        for(size_t c=0;c<banks.size();++c){
            bool light=c==7&&light_high>0;
            float threshold=light?light_high:high;
            banks[c]=std::make_unique<byte_track::BYTETracker>(30,18,threshold,light?light_high:std::max(.30f,high),.8f);
        }
        previous=-1;tick=0;
    }
public:
    explicit LiveTracker(float threshold=.25f,float light_threshold=0):high(threshold),light_high(light_threshold){
        if(!std::isfinite(light_high)||(light_high!=0&&(light_high<.1f||light_high>1)))throw std::runtime_error("light-conf must be in [.1,1]");
        rebuild();
    }
    void reset(){rebuild();}
    std::vector<TrackedBox> update(const std::vector<Box>& input,double stamp,unsigned channel){
        if(channel>=4 || !std::isfinite(stamp) || stamp<0)throw std::runtime_error("Invalid live tracking stamp");
        if(previous>=0 && stamp<=previous)return {};
        if(previous>=0 && stamp-previous>.6)rebuild();
        double dt=previous<0?1:(stamp-previous)*30;
        if(previous<0)origin=stamp;
        size_t next=std::max(tick+1,size_t(std::llround((stamp-origin)*30))+1),advance=next-tick;
        previous=stamp;tick=next;
        std::array<std::vector<byte_track::Object>,8> objects;
        for(const auto& b:input){
            if(b.category<0 || b.category>=8 || !std::isfinite(b.score) || b.score<.1f ||
               !std::isfinite(b.x1)||!std::isfinite(b.x2)||!std::isfinite(b.y1)||!std::isfinite(b.y2))continue;
            float x1=std::clamp(b.x1,0.f,1280.f),x2=std::clamp(b.x2,0.f,1280.f);
            float y1=std::clamp(b.y1,0.f,720.f),y2=std::clamp(b.y2,0.f,720.f);
            if(x2>x1 && y2>y1)objects[size_t(b.category)].emplace_back(byte_track::Rect<float>(x1,y1,x2-x1,y2-y1),b.category,b.score);
        }
        std::vector<TrackedBox> result;
        for(size_t c=0;c<8;++c)for(auto t:banks[c]->update(objects[c],advance,float(dt))){
            if(!t->isActivated() || t->getSTrackState()!=byte_track::STrackState::Tracked)continue;
            auto r=t->getRect();
            Box b{std::clamp(r.x(),0.f,1280.f),std::clamp(r.y(),0.f,720.f),
                  std::clamp(r.x()+r.width(),0.f,1280.f),std::clamp(r.y()+r.height(),0.f,720.f),t->getScore(),int(c)};
            if(!std::isfinite(b.x1)||!std::isfinite(b.y1)||!std::isfinite(b.x2)||!std::isfinite(b.y2)||b.x2<=b.x1||b.y2<=b.y1)continue;
            if(t->getTrackId()>0xffffffffu)throw std::runtime_error("Track ID overflow");
            uint64_t id=(uint64_t(channel)<<62)|(uint64_t(c)<<59)|(generation<<32)|t->getTrackId();
            result.push_back({b,id,false,stamp});
        }
        return result;
    }
    size_t retained() const {size_t n=0;for(const auto& t:banks)n+=t->retainedCount();return n;}
};
}
