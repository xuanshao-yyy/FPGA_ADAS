#pragma once
#include "explanation_state.hpp"
#include <algorithm>
#include <vector>

namespace driver_demo {
struct DashboardBadge {const char* label;int color;};
// Visual summaries only. They never create permission or feed perception rules.
inline DashboardBadge signal_badge(const std::string& value,const std::string& light="UNKNOWN"){
    if(value=="信号允许"&&light=="GREEN")return {"绿灯",4};
    if(value=="信号禁止"&&light=="RED")return {"红灯",6};
    if(value=="信号禁止"&&light=="YELLOW")return {"黄灯",5};
    if(value=="信号禁止")return {"停止保持",6}; // Restrictive state, current color unknown.
    if(value=="禁止尚未解除")return {"待解除",6};
    if(value=="当前未见灯")return {"未见灯",3};
    if(value=="前视证据不足")return {"无证据",3};
    if(value=="未关联此方向")return {"无对应灯",3};
    if(value=="信号暂失")return {"暂失",5};
    if(value=="信号冲突")return {"冲突",5};
    return {"核实中",5};
}
inline DashboardBadge marking_badge(int style){
    if(style==1)return {"实线",6};
    if(style==2)return {"虚线",4}; // Marking condition only, never "safe to change lane".
    return {"待识别",3};
}
inline std::string lane_title(const Explanation& e){
    if(!e.lane_position_known)return "本车道待确认";
    if(e.lane=="当前位于车道中央")return "居中";
    if(e.lane=="当前偏左")return "偏左";
    if(e.lane=="当前偏右")return "偏右";
    if(e.lane=="靠近左侧边界")return "靠近左线";
    if(e.lane=="靠近右侧边界")return "靠近右线";
    return e.lane;
}
// Xft width measurements can be expensive: binary-search UTF-8 boundaries.
template<class Measure> inline std::string fit_label(const std::string& value,int limit,Measure measure){
    if(limit<=0)return "";
    if(measure(value)<=limit)return value;
    const std::string ellipsis="…";
    if(measure(ellipsis)>limit)return "";
    std::vector<size_t> ends{0};
    for(size_t i=1;i<=value.size();++i)
        if(i==value.size()||(static_cast<unsigned char>(value[i])&0xc0)!=0x80)ends.push_back(i);
    size_t lo=0,hi=ends.size()-1;
    while(lo<hi){size_t mid=(lo+hi+1)/2;if(measure(value.substr(0,ends[mid])+ellipsis)<=limit)lo=mid;else hi=mid-1;}
    return value.substr(0,ends[lo])+ellipsis;
}
struct DashboardRect {
    int x,y,w,h;
    bool contains(int xx,int yy)const{return xx>=x&&yy>=y&&xx<x+w&&yy<y+h;}
};
struct DashboardLayout {
    int width,height;
    bool compact()const{return height<190;}
    DashboardRect card(int i)const{
        int available=std::max(3,width-32),first=available*29/100,second=available*29/100;
        int x=i==0?8:i==1?16+first:24+first+second;
        return {x,32,i==0?first:i==1?second:width-8-x,std::max(1,height-58)};
    }
    DashboardRect lane_button(int side)const{
        auto c=card(1);int half=std::max(1,(c.w-28)/2);
        return {c.x+10+side*(half+8),c.y+(compact()?51:68),half,compact()?25:28};
    }
    int lane_hit(int x,int y)const{
        for(int side=0;side<2;++side)if(lane_button(side).contains(x,y))return side;
        return -1;
    }
};
}
