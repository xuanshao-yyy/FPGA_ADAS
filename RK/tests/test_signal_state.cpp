#include "signal_state.hpp"
#include <cassert>
#include <iostream>
#include <limits>
using namespace signal_demo;
int main(){
    State s;
    assert(s.decision(0).light==Color::Unknown && !s.go(0).allowed);
    s.stop();
    for(int i=0;i<6;++i)s.observe(Direction::Straight,Color::Green,1+i*.04,unsigned(i+1),1+i*.04);
    assert(s.decision(1.20).light==Color::Green && !s.decision(1.20).allowed);
    assert(s.go(1.20).allowed);
    s.observe(Direction::Straight,Color::Red,1.24,7,1.24);
    assert(s.decision(1.24).light==Color::Unknown && !s.decision(1.24).allowed);
    for(int i=1;i<4;++i)s.observe(Direction::Straight,Color::Red,1.24+i*.04,unsigned(7+i),1.24+i*.04);
    assert(s.decision(1.36).alarm && !s.decision(1.36).allowed);
    s.observe(Direction::Straight,Color::Green,1.40,11,1.40);
    for(int i=1;i<6;++i)s.observe(Direction::Straight,Color::Green,1.40+i*.04,unsigned(11+i),1.40+i*.04);
    assert(s.decision(1.60).alarm && !s.decision(1.60).allowed); // red alarm latches
    s.stop();assert(!s.decision(1.60).alarm && s.go(1.60).allowed);
    s.select(Direction::Right);assert(!s.decision(1.60).requested && s.go(1.60).light==Color::Unknown);
    s.select(Direction::Straight);assert(!s.decision(1.60).requested);
    assert(s.decision(2.00).light==Color::Unknown); // expired
    s.observe(Direction::Straight,Color::Green,2.00,30,2.00);
    assert(s.decision(2.00).light==Color::Unknown); // long gap needs reconfirmation
    for(int i=0;i<20;++i)s.observe(Direction::Straight,Color::Green,2.00,30,2.01);
    assert(s.get(Direction::Straight).samples==1); // held frame cannot confirm
    s.observe(Direction::Straight,Color::Unknown,2.04,31,2.04);assert(s.get(Direction::Straight).samples==0);
    s.observe(Direction::Straight,Color::Green,3.0,32,2.5);assert(s.decision(2.5).light==Color::Unknown);
    s.observe(Direction::Straight,Color::Green,1.0,32,3.0);assert(s.get(Direction::Straight).last<0);
    State y;
    for(int i=0;i<4;++i)y.observe(Direction::Straight,Color::Yellow,4+i*.04,unsigned(i+1),4+i*.04);
    assert(std::string(y.go(4.12).state)=="BLOCKED_YELLOW");
    y.reset_all();assert(!y.decision(4.12).requested && !y.decision(4.12).alarm);
    State automatic;for(int i=0;i<4;++i){double t=8+i*.1;
        automatic.observe(Direction::Left,Color::Green,t,i+1,t);
        automatic.observe(Direction::Straight,Color::Red,t,i+1,t);
        automatic.observe(Direction::Right,Color::Yellow,t,i+1,t);}
    assert(automatic.signal_decision(Direction::Left,8.3).allowed);
    assert(std::string(automatic.signal_decision(Direction::Straight,8.3).instruction)=="STOP");
    assert(std::string(automatic.signal_decision(Direction::Right,8.3).instruction)=="STOP");
    assert(automatic.selected()==Direction::Straight&&!automatic.decision(8.3).requested);
    assert(!automatic.signal_decision(Direction::Left,9).allowed);
    bool bad=false;try{Settings settings;settings.max_gap_ms=500;State invalid(settings);}catch(const std::runtime_error&){bad=true;}assert(bad);
    std::cout<<"PASS signal temporal confirmation, immediate transition invalidation, duplicate/stale/future input, per-direction separation, red alarm latch and explicit stop/reset.\n";
}
