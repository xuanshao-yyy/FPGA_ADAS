#pragma once
#include <array>
namespace driver_demo {
// Behaviour indicators only. No actuator, speed or travelled-distance estimate.
struct VehicleInput {
    bool manual_stop=false,forced_alarm=false,still=false,front_fresh=false;
    bool signal_stop=false,road_ready=false,front_stop=false,slow=false,rear_alert=false;
    bool lane_requested=false,lane_allowed=false,lane_alarm=false;
    int road_direction=1,lane_direction=1;
};
struct VehicleDecision {
    const char* state="WAIT";const char* label="注意观察";const char* reason="当前证据尚未更新";
    int arrow=-1;bool moving=false,alarm=false;
};
inline VehicleDecision vehicle_decision(const VehicleInput& in){
    if(in.forced_alarm)return {"ALARM","报警","通行请求与当前限制冲突",-1,false,true};
    if(in.manual_stop)return {"STOPPED","停车提示","手动测试状态，重新检查并继续后恢复自动建议"};
    if(in.lane_requested&&in.lane_alarm)return {"ALARM","报警","禁止变道，请保持车道",-1,false,true};
    if(in.rear_alert&&!in.signal_stop&&!in.front_stop)return {"ALERT","报警","后方目标过近或快速接近，请注意后方",-1,in.front_fresh&&in.road_ready,true};
    if(in.signal_stop)return {"STOPPED","红黄灯停车","当前道路方向禁止通行"};
    if(in.front_stop)return {"ALARM","报警","前方目标过近，请减速避让",-1,false,true};
    if(in.slow)return {"SLOW","减速","前方目标接近或需要礼让，请保持间距",in.road_ready&&in.front_fresh&&!in.still?in.road_direction:-1,in.road_ready&&in.front_fresh&&!in.still,false};
    if(!in.front_fresh)return {};
    if(in.still){
        int arrow=in.road_ready&&(!in.lane_requested||in.lane_allowed)?(in.lane_requested?in.lane_direction:in.road_direction):-1;
        return {"STILL",arrow<0?"图片检测":"静帧方向提示","箭头为图片条件提示，不模拟连续运动",arrow,false,false};
    }
    if(in.lane_requested){
        if(!in.lane_allowed||!in.road_ready)return {"WAIT","保持车道","不执行变道请求，请查看限制原因"};
        return {in.lane_direction==0?"CHANGE_LEFT":"CHANGE_RIGHT",in.lane_direction==0?"向左变道":"向右变道","综合变道条件通过",in.lane_direction,true,false};
    }
    if(!in.road_ready)return {};
    return {in.road_direction==0?"TURN_LEFT":in.road_direction==2?"TURN_RIGHT":"MOVING",in.road_direction==0?"路口左转意图":in.road_direction==2?"路口右转意图":"直行提示","方向选择不代表变道许可或已验证路口转弯条件",in.road_direction,true,false};
}
// Up/left/right seven-point arrows, suitable for native and test rendering.
inline std::array<std::array<int,2>,7> arrow_points(int direction){
    std::array<std::array<int,2>,7> p{{{{0,-12}},{{9,-3}},{{3,-3}},{{3,11}},{{-3,11}},{{-3,-3}},{{-9,-3}}}};
    for(auto& v:p){int x=v[0],y=v[1];if(direction==0)v={y,-x};else if(direction==2)v={-y,x};}
    return p;
}
}
