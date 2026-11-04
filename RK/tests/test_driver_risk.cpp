#include "driver_risk.hpp"
#include <cassert>
#include <iostream>
int main(){
    using namespace driver_demo;RiskState state;Corridor road;
    ai::TrackedBox car;car.id=1;car.box={400,300,900,710,.9f,2};
    for(int i=0;i<3;++i)assert(state.observe({car},10+i*.1,10+i*.1+.02,road,false));
    assert(state.decision(10.23).level==2&&state.decision(10.23).assumed_region);
    state.observe({},10.3,10.32,road,false);assert(state.decision(10.4).level==2&&state.decision(10.4).held);
    assert(state.decision(10.81).level==0);
    state.reset();assert(state.decision(10.4).level==0);
    auto side=car;side.box={0,300,180,710,.9f,2};
    for(int i=0;i<3;++i)state.observe({side},20+i*.1,20+i*.1+.02,road,false);
    assert(state.decision(20.23).target==0);
    state.reset();auto person=car;person.id=2;person.box={600,450,680,650,.9f,0};
    for(int i=0;i<3;++i)state.observe({person},30+i*.1,30+i*.1+.02,road,false);
    assert(state.decision(30.23).level==1&&state.decision(30.23).category==0);
    assert(!state.observe({car},29,30.3,road,false));assert(!state.observe({car},31,30.3,road,false));
    state.reset();for(int i=0;i<3;++i){auto box=person;box.box.y1-=i*10;state.observe({box},40+i*.1,40+i*.1+.02,road,true);}
    assert(state.decision(40.23).growth==0&&state.decision(40.23).still);
    state.reset();auto weak=car;weak.box.score=.1f;state.observe({weak},50,50.02,road,false);assert(state.decision(50.03).target==0);
    state.reset();state.observe({car},60,60.02,road,false);car.id=3;state.observe({car},60.1,60.12,road,false);assert(state.decision(60.13).level==0);
    std::cout<<"PASS image-risk confirmation, side exclusion, pedestrian slow, stale/future rejection, bounded hold, scene reset and no static motion claim\n";
}
