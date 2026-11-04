#pragma once
#include <array>
#include <string>
#include "camera_roles.hpp"
namespace driver_demo {
struct Explanation {
    int severity=1;
    std::string action="观察 / WATCH",reason="等待前视检测证据",scope="模拟辅助提示 · 不控制车辆";
    std::array<std::string,3> signal{{"待确认","待确认","待确认"}};
    std::array<std::string,3> signal_light{{"UNKNOWN","UNKNOWN","UNKNOWN"}}; // Confirmed, direction-associated color for display only.
    std::array<std::string,3> signal_detail{{"等待信号证据","等待信号证据","等待信号证据"}};
    std::array<std::string,3> signal_hint{}; // Position prior only; does not change signal permissions.
    std::string lane="边界未确认",markings="左 未确认 / 右 未确认",lane_request="侧后安全尚未验证";
    std::string lane_detail="等待车道结果";
    std::string lane_pixels="彩色标线=模型像素候选";
    std::string target="前方目标尚未确认",risk="图像近远规则，未标定米数",source="";
    std::array<int,2> lane_styles{{0,0}}; // 0 unknown, 1 solid, 2 dashed; display only.
    double lane_offset=0;
    bool lane_position_known=false,lane_position_held=false,lane_requested=false;
    int intent=1,lane_intent=1; // Road turn and lane-change direction are separate.
    bool still=false;
    std::string vehicle_state="WAIT",vehicle_label="注意观察",vehicle_reason="当前证据尚未更新";
    int vehicle_arrow=-1;
    bool vehicle_moving=false,vehicle_alarm=false,manual_stop=false,ui_alarm=false,audible_alarm=false;
    std::array<int,4> near_levels{}; // Physical channel IDs, not displayed tile indices.
    std::array<bool,4> near_fresh{},near_held{},near_approaching{};
};
// Side observations remain recorded; only the requested side raises a driving alert.
inline bool proximity_alert_active(const Explanation& e,unsigned channel){
    return channel==camera_roles::Front||channel==camera_roles::Rear||
        (e.lane_requested&&channel==(e.lane_intent==0?camera_roles::Left:camera_roles::Right));
}
}
