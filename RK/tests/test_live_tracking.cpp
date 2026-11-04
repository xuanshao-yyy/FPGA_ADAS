#include "live_tracking.hpp"
#include <cassert>
#include <iostream>
int main(){
    ai::LiveTracker tracker;ai::Box car{100,100,160,150,.9f,2};
    auto first=tracker.update({car},1.,0);assert(first.size()==1);auto id=first[0].id;
    for(double t:{1.07,1.22,1.41}){car.x1+=2;car.x2+=2;auto out=tracker.update({car},t,0);assert(out.size()==1 && out[0].id==id);}
    assert(tracker.update({car},1.41,0).empty());assert(tracker.update({car},1.4,0).empty());
    car.score=.15f;auto low=tracker.update({car},1.5,0);assert(low.size()==1 && low[0].id==id);
    tracker.update({},1.6,0);car.score=.9f;auto recovered=tracker.update({car},1.75,0);assert(recovered.size()==1 && recovered[0].id==id);
    auto reset=tracker.update({car},2.5,0);assert(reset.size()==1 && reset[0].id!=id);
    ai::LiveTracker other;auto different=other.update({car},1.,1);assert(different.size()==1 && different[0].id!=id);
    ai::Box bus=car;bus.category=3;auto classes=other.update({car,bus},1.1,1);assert(classes.size()==1);classes=other.update({car,bus},1.2,1);assert(classes.size()==2 && classes[0].id!=classes[1].id);
    size_t peak=0;double now=3;
    for(int i=0;i<3000;++i){car.x1=float((i%15)*75);car.x2=car.x1+25;tracker.update({car},now,0);now+=.11;peak=std::max(peak,tracker.retained());}
    assert(peak<80);
    std::cout<<"PASS irregular dt, duplicate/order, low-score association, recovery, expiry, channel/class isolation; retained peak="<<peak<<"\n";
}
