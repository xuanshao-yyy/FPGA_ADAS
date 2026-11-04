#include "signal_state.hpp"
#include <cassert>
#include <iostream>
using namespace signal_demo;
int main(){
    Settings cfg;cfg.stop_hold_ms=1500;State state(cfg);
    for(int i=0;i<3;++i)state.observe(Direction::Straight,Color::Red,10+i*.1,i+1,10+i*.1);
    assert(state.decision(10.2).light==Color::Red&&std::string(state.decision(10.2).instruction)=="STOP");
    state.observe(Direction::Straight,Color::Unknown,10.3,4,10.3);
    auto d=state.decision(10.3);assert(d.light==Color::Unknown&&d.stop_held&&std::string(d.state)=="STOP_HOLD"&&!d.allowed&&std::string(d.instruction)=="STOP");
    // Unknown frames and repeated reads cannot renew the stop evidence clock.
    for(int i=0;i<12;++i){double at=10.4+i*.1;state.observe(Direction::Straight,Color::Unknown,at,i+5,at);}
    assert(state.decision(11.69).stop_held);assert(!state.decision(11.71).stop_held);
    assert(!state.go(11.71).allowed);state.stop();
    // No observation at all uses exactly the same bounded grace.
    State silent(cfg);for(int i=0;i<3;++i)silent.observe(Direction::Straight,Color::Red,20+i*.1,i+1,20+i*.1);
    assert(silent.decision(21.69).stop_held);assert(!silent.decision(21.71).stop_held);
    for(int i=0;i<30;++i)silent.observe(Direction::Straight,Color::Red,20.2,3,20.21);
    assert(!silent.decision(21.71).stop_held);
    State late(cfg);for(int i=0;i<3;++i)late.observe(Direction::Straight,Color::Red,25+i*.1,i+1,25+i*.1);
    late.observe(Direction::Straight,Color::Green,25.25,4,25.6);assert(late.decision(25.6).stop_held&&!late.decision(25.6).allowed);
    assert(!late.decision(26.71).stop_held); // stale GREEN neither releases nor renews the prior stop
    // A fresh green sequence releases the stop grace after existing confirmation.
    State transition(cfg);for(int i=0;i<3;++i)transition.observe(Direction::Straight,Color::Red,30+i*.1,i+1,30+i*.1);
    transition.observe(Direction::Straight,Color::Green,30.3,4,30.3);assert(transition.decision(30.3).stop_held);
    transition.observe(Direction::Straight,Color::Green,30.4,5,30.4);assert(!transition.go(30.4).allowed);
    transition.observe(Direction::Straight,Color::Green,30.5,6,30.5);assert(transition.decision(30.5).allowed&&!transition.decision(30.5).stop_held);
    transition.observe(Direction::Straight,Color::Unknown,30.6,7,30.6);assert(!transition.decision(30.6).allowed&&!transition.decision(30.6).stop_held);
    // Confirmed yellow also means stop; direction/reset never borrows another lamp.
    State yellow(cfg);for(int i=0;i<3;++i)yellow.observe(Direction::Left,Color::Yellow,40+i*.1,i+1,40+i*.1);
    yellow.select(Direction::Left);assert(yellow.decision(40.6).stop_held);yellow.select(Direction::Right);assert(!yellow.decision(40.6).stop_held);
    yellow.select(Direction::Left);yellow.reset(Direction::Left);assert(!yellow.decision(40.6).stop_held);
    State reset(cfg);for(int i=0;i<3;++i)reset.observe(Direction::Straight,Color::Red,50+i*.1,i+1,50+i*.1);
    reset.reset_all();assert(!reset.decision(50.3).stop_held);
    State legacy;for(int i=0;i<3;++i)legacy.observe(Direction::Straight,Color::Red,60+i*.1,i+1,60+i*.1);
    legacy.observe(Direction::Straight,Color::Unknown,60.3,4,60.3);assert(!legacy.decision(60.3).stop_held);
    bool invalid=false;try{Settings bad;bad.stop_hold_ms=2001;State reject(bad);}catch(...){invalid=true;}assert(invalid);
    std::cout<<"PASS restrictive-only bounded grace, missing/duplicate/no-input expiry, green reconfirmation and immediate revocation, per-direction/reset and zero-hold legacy\n";
}
