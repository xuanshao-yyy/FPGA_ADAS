#include "vehicle_simulation.hpp"
#include <cassert>
#include <string>
#include <iostream>
int main(){
    using namespace driver_demo;VehicleInput in;
    assert(std::string(vehicle_decision(in).state)=="WAIT");
    in.front_fresh=in.road_ready=true;auto d=vehicle_decision(in);assert(d.moving&&d.arrow==1&&std::string(d.state)=="MOVING");
    for(int turn:{0,2}){in.road_direction=turn;d=vehicle_decision(in);assert(d.moving&&d.arrow==turn);}
    in.signal_stop=true;d=vehicle_decision(in);assert(!d.moving&&d.arrow==-1&&std::string(d.state)=="STOPPED"&&!d.alarm);
    in.forced_alarm=true;d=vehicle_decision(in);assert(d.alarm&&!d.moving&&d.arrow==-1);
    in.forced_alarm=in.signal_stop=false;in.manual_stop=true;assert(!vehicle_decision(in).moving);in.manual_stop=false;
    in.lane_requested=true;assert(!vehicle_decision(in).moving);in.lane_allowed=true;in.lane_direction=0;assert(vehicle_decision(in).arrow==0);
    in.lane_alarm=true;assert(vehicle_decision(in).alarm&&!vehicle_decision(in).moving);in.lane_alarm=in.lane_requested=false;
    in.slow=true;assert(std::string(vehicle_decision(in).state)=="SLOW");in.front_stop=true;assert(!vehicle_decision(in).moving);
    in.front_stop=false;in.slow=false;in.still=true;assert(std::string(vehicle_decision(in).state)=="STILL"&&!vehicle_decision(in).moving);
    in.still=false;in.front_fresh=true;in.road_ready=false;in.slow=true;
    d=vehicle_decision(in);assert(std::string(d.state)=="SLOW"&&std::string(d.label)=="减速"&&d.arrow==-1&&!d.moving);
    in.front_fresh=false;d=vehicle_decision(in);assert(std::string(d.state)=="SLOW"&&d.arrow==-1);
    in.signal_stop=true;assert(std::string(vehicle_decision(in).state)=="STOPPED");
    auto left=arrow_points(0),up=arrow_points(1),right=arrow_points(2);assert(left[0][0]<0&&up[0][1]<0&&right[0][0]>0);
    std::cout<<"PASS simulated stop/go/turn/lane/slow/alarm/hold/still states and left/up/right vector arrows\n";
}
