#include "tracking.hpp"
#include "test_helpers.hpp"
#include <iostream>
ai::Box box(float x,float score=.9f,int category=2){return {x,100,x+100,200,score,category};}
int main(){try{
    ai::RoadTracker tracker;
    CHECK(tracker.update({box(100)},1).empty()); // one-frame false positives are not shown
    auto a=tracker.update({box(102)},1.1);CHECK(a.size()==1 && !a[0].predicted);auto id=a[0].id;
    auto b=tracker.update({box(104,.15f)},1.2);CHECK(b.size()==1 && b[0].id==id && !b[0].predicted);
    CHECK(b[0].box.x1<104 && b[0].box.x1>102); // smoothing without freezing
    auto c=tracker.update({},1.25);CHECK(c.size()==1 && c[0].predicted && c[0].id==id);
    CHECK(tracker.update({},1.4).empty()); // display coasting expires before ID retention
    auto d=tracker.update({box(108)},1.5);CHECK(d.size()==1 && d[0].id==id);
    CHECK(tracker.update({box(109)},1.5).empty()); // duplicates cannot confirm or move tracks
    CHECK(tracker.update({box(110)},2.2).empty()); // long pause resets, requires new confirmation
    auto e=tracker.update({box(111)},2.3);CHECK(e.size()==1 && e[0].id!=id);
    ai::RoadTracker low;CHECK(low.update({box(10,.15f)},1).empty());CHECK(low.update({box(10,.15f)},1.1).empty());
    ai::RoadTracker classes;classes.update({box(10,.9f,0),box(10,.9f,2)},1);
    auto f=classes.update({box(12,.9f,2),box(12,.9f,0)},1.1);CHECK(f.size()==2 && f[0].id!=f[1].id);
    ai::RoadTracker channel;channel.update({box(800)},1);auto other=channel.update({box(800)},1.1);
    CHECK(other.size()==1 && other[0].box.x1>700);CHECK(e[0].box.x1<200);
    ai::RoadTracker malformed;
    auto bad=box(10);bad.x1=NAN;
    CHECK(malformed.update({bad},1).empty());CHECK(malformed.update({bad},1.1).empty());
    ai::RoadTracker bounded;
    std::vector<ai::Box> many(400,box(20));bounded.update(many,1);
    CHECK(bounded.update(many,1.1).size()<=128);
    // A deliberately irregular time sequence; constant velocity is estimated in px/s.
    ai::RoadTracker moving;moving.update({box(0)},1);moving.update({box(10)},1.1);
    auto m=moving.update({box(40)},1.5);CHECK(m.size()==1 && m[0].box.x1>38);
    std::cout<<"PASS: confirmation, low-score recovery, class/channel isolation, time-based expiration, duplicate/gap handling, EMA and bounds\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
