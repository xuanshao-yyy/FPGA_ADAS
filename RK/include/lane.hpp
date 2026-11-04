#pragma once
#include "detection.hpp"
#include "lane_geometry.hpp"
#include "curve.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace ai {
constexpr int LaneWidth=576,LaneHeight=324;
struct LaneMask {
    uint32_t frame_id=0,source=0;
    double received=0,finished=0;
    unsigned pixels=0;
    std::vector<uint8_t> data;
    LaneGeometry geometry;
    double geometry_ms=0;
    bool geometry_enabled=false;
    bool four_class=false;
    Curve left_curve,right_curve;
    int left_style=0,right_style=0;
    struct StyleEvidence {int solid_votes=0,dashed_votes=0,raw_style=0,confirmation_streak=0;bool direct=false;const char* reason="GEOMETRY_DISABLED";};
    StyleEvidence left_evidence,right_evidence;
    const char* pair_reason="GEOMETRY_DISABLED";
    std::vector<uint8_t> blocked;
    std::vector<uint8_t> native_classes;
    std::vector<uint8_t> native_blocked;
    unsigned blocked_pixels=0,crosswalk_pixels=0;
    double inputs_ms=0,run_ms=0,get_ms=0,validate_ms=0,decode_ms=0,occlusion_ms=0;
    double output_copy_ms=0,post_queue_ms=0;
    bool cpu_propagated=false,position_valid=false;
    uint32_t model_frame_id=0;
    double model_received=0,relative_offset=0,left_margin=0,right_margin=0;
    const char* position="UNKNOWN";
};
struct LaneOptions {
    std::string model;
    std::array<bool,4> channels{{true,false,false,false}};
    double fps=0;
    float confidence=.5f;
    unsigned max_age_ms=750;
    bool geometry=true;
    float ego_x=.5f;
    std::string camera="camera_live.json";
    bool occlusion=true;
    bool native_output=false;
};
// The supplied YOLOP head already applies sigmoid separately to its two channels.
// Crop the 140-row letterbox padding, then bilinearly sample at display pixel centers.
inline LaneMask decode_lane(const Tensor& output,float threshold){
    if(!output.data || output.channels!=2 || output.width!=640 || output.height!=640)
        throw std::runtime_error("YOLOP lane output must be two sigmoid planes at 640x640");
    LaneMask mask;mask.data.resize(LaneWidth*LaneHeight);
    auto sample=[&](int channel,float fx,float fy){
        fx=std::clamp(fx,0.f,639.f);fy=std::clamp(fy,140.f,499.f);
        int x=int(fx),y=int(fy),x2=std::min(x+1,639),y2=std::min(y+1,499);
        float dx=fx-x,dy=fy-y;
        return (1-dy)*((1-dx)*output.at(channel,y,x)+dx*output.at(channel,y,x2))+
               dy*((1-dx)*output.at(channel,y2,x)+dx*output.at(channel,y2,x2));
    };
    for(int y=0;y<LaneHeight;++y)for(int x=0;x<LaneWidth;++x){
        float fx=(x+.5f)*640/LaneWidth-.5f,fy=140+(y+.5f)*360/LaneHeight-.5f;
        float background=sample(0,fx,fy),lane=sample(1,fx,fy);
        if(!std::isfinite(background) || !std::isfinite(lane))throw std::runtime_error("Non-finite lane output");
        bool candidate=lane>background && lane>threshold;
        mask.data[y*LaneWidth+x]=candidate;mask.pixels+=candidate;
    }
    return mask;
}
}
