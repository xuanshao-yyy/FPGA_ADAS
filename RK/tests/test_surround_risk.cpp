#include "surround_risk.hpp"
#include <cassert>
#include <iostream>
ai::Result sample(double t,int category=2,double scale=1){
    ai::Result r;r.received=t;r.finished=t+.01;r.frame_id=r.source=uint32_t(t*1000);
    ai::TrackedBox b;b.id=9;b.box={float(640-180*scale),float(550-180*scale),float(640+180*scale),float(550+100*scale),.9f,category};
    r.tracks.push_back(b);return r;
}
int main(){
    using namespace product;SurroundRisk state;Profile p;
    for(auto& z:p.zones){z.verified=true;z.near={{.02f,.02f},{.98f,.02f},{.98f,.98f},{.02f,.98f}};}
    state.configure(p);
    for(double t:{1.,1.2,1.4})assert(state.observe(1,sample(t),t+.01));
    auto d=state.decision(1,1.41);assert(d.fresh&&d.calibrated&&d.occupied);
    state.require_image_quality=true;assert(!state.decision(1,1.41).fresh);
    state.image_quality(1,false,1.4);assert(!state.decision(1,1.41).fresh);
    state.image_quality(1,true,1.4);assert(state.decision(1,1.41).fresh);state.require_image_quality=false;
    assert(!state.observe(1,sample(1.4),1.42));assert(!state.decision(1,2.1).fresh);
    state.configure(Profile{});for(double t:{1.,1.2,1.4})state.observe(1,sample(t),t+.01);
    assert(!state.decision(1,1.41).occupied&&!state.decision(1,1.41).calibrated);
    state.configure(p);for(double t:{1.,1.2,1.4}){auto r=sample(t);r.tracks[0].predicted=true;state.observe(1,r,t+.01);}
    assert(!state.decision(1,1.41).occupied);
    state.configure(p);for(int i=0;i<5;++i)state.observe(3,sample(1+i*.2,2,.45+i*.08),1+i*.2+.01);
    assert(state.decision(3,1.81).approaching);
    state.configure(p);for(int i=0;i<5;++i)state.observe(3,sample(1+i*.2,2,.45+i*.08),1+i*.2+.01,true);
    assert(!state.decision(3,1.81).approaching);
    p.zones[0].metric_verified=true;p.zones[0].ground_image=p.zones[0].near;
    p.zones[0].ground=(cv::Mat_<double>(3,3)<<4,0,-2,0,-6,6,0,0,1);
    state.configure(p);for(double t:{1.,1.2,1.4}){auto r=sample(t,0,.3);r.tracks[0].box.y2=576;state.observe(0,r,t+.01);}
    auto person=state.decision(0,1.41);assert(person.person==2&&person.distance>1.1&&person.distance<1.3);
    auto empty=sample(1.6);empty.tracks.clear();state.observe(0,empty,1.61);assert(state.decision(0,1.61).person==2);
    assert(state.decision(0,3).person==0);
    // The same parking image rules work for all side/rear channels without a metric ROI.
    for(unsigned ch:{1u,2u,3u}){
        state.configure(Profile{});
        for(double t:{10.,10.3,10.6}){auto r=sample(t);r.boxes={{320,180,960,710,.9f,2}};state.observe(ch,r,t+.01);}
        auto near=state.decision(ch,10.61);assert(near.image_fresh&&near.proximity==2&&!near.calibrated&&!near.occupied);
        auto gap=sample(10.8);gap.tracks.clear();state.observe(ch,gap,10.81);assert(state.decision(ch,10.81).proximity==2);
        assert(!state.decision(ch,11.7).image_fresh&&state.decision(ch,11.7).proximity==0);
        state.require_image_quality=true;state.image_quality(ch,false,10.8);assert(!state.decision(ch,10.81).image_fresh);state.require_image_quality=false;
        state.configure(Profile{});auto once=sample(12);once.boxes={{320,180,960,710,.9f,2}};state.observe(ch,once,12.01);
        assert(!state.observe(ch,once,12.5));assert(state.decision(ch,12.51).proximity<2); // duplicates cannot confirm red
    }
    // Empty fresh detections need >=600ms and >=3 unique samples. Bad images,
    // a low-score real target, stale evidence and source rewind revoke clearance.
    state.configure(Profile{});state.require_image_quality=true;
    auto vacant=[](double t){auto r=sample(t);r.tracks.clear();return r;};
    for(double t:{20.,20.3}){state.image_quality(2,true,t);state.observe(2,vacant(t),t+.01);}
    assert(!state.decision(2,20.31).image_clear);
    state.image_quality(2,true,20.65);state.observe(2,vacant(20.65),20.66);assert(state.decision(2,20.66).image_clear);
    state.image_quality(2,false,20.66);assert(!state.decision(2,20.67).image_clear);
    state.image_quality(2,true,20.66);assert(!state.decision(2,21.3).image_clear);
    auto occupied=vacant(20.9);occupied.boxes={{50,50,100,100,.16f,2}};state.observe(2,occupied,20.91);assert(!state.decision(2,20.91).image_clear);
    state.observe(2,vacant(21.1),21.11);state.observe(2,vacant(21.4),21.41);assert(!state.decision(2,21.41).image_clear);
    auto rewind=vacant(21.7);rewind.source=1;state.image_quality(2,true,21.7);state.observe(2,rewind,21.71);assert(!state.decision(2,21.71).image_clear);
    std::cout<<"PASS calibrated side occupancy, unverified/duplicate/predicted/stale rejection, rear growth/static rejection and measured-plane pedestrian risk\n";
}
