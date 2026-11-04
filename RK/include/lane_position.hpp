#pragma once
#include "lane.hpp"
namespace ai {
// Camera-axis position relative to the observed pair. No metric calibration,
// road-wide lane number, turn intention or completed-lane-change assertion.
inline void lane_position(LaneMask& mask,double ego,double reference){
    mask.position="UNKNOWN";mask.position_valid=false;
    auto l=mask.left_curve,r=mask.right_curve;
    if(!l.valid||!r.valid||reference<l.ymin||reference>l.ymax||reference<r.ymin||reference>r.ymax)return;
    double left=l.x(reference),right=r.x(reference),width=right-left;
    if(!std::isfinite(left)||!std::isfinite(right)||width<=76.8||width>=576)return;
    for(double y=std::max(l.ymin,r.ymin);y<=std::min(l.ymax,r.ymax);y+=5)if(l.x(y)+15>=r.x(y))return;
    int yy=std::clamp(int(std::lround(reference)),0,359);
    for(double x:{left,right})if(x<0||x>=640||
        (mask.native_blocked.size()==640*360 && mask.native_blocked[yy*640+int(x)]))return;
    mask.relative_offset=(ego-(left+right)/2)/(width/2);
    if(!std::isfinite(mask.relative_offset))return;
    mask.position_valid=true;
    mask.left_margin=ego-left;mask.right_margin=right-ego;
    double offset=mask.relative_offset;
    mask.position=offset<=-1?"CROSS_LEFT_CANDIDATE":offset>=1?"CROSS_RIGHT_CANDIDATE":
        offset<-.65?"NEAR_LEFT_BOUNDARY":offset>.65?"NEAR_RIGHT_BOUNDARY":
        offset<-.2?"LEFT_OF_CENTER":offset>.2?"RIGHT_OF_CENTER":"CENTERED";
}
}
