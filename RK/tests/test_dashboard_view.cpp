#include "dashboard_view.hpp"
#include "signal_mouse.hpp"
#include <cassert>
#include <string>
int main(){
    using namespace driver_demo;
    assert(std::string(signal_badge("信号允许","GREEN").label)=="绿灯"&&signal_badge("信号允许","GREEN").color==4);
    assert(std::string(signal_badge("信号禁止","RED").label)=="红灯"&&signal_badge("信号禁止","RED").color==6);
    assert(std::string(signal_badge("信号禁止","YELLOW").label)=="黄灯"&&signal_badge("信号禁止","YELLOW").color==5);
    assert(std::string(signal_badge("信号禁止","UNKNOWN").label)=="停止保持");
    assert(signal_badge("信号允许","UNKNOWN").color!=4);
    assert(signal_badge("信号冲突","GREEN").color!=4);
    assert(signal_badge("禁止尚未解除").color==6);
    assert(std::string(signal_badge("未关联此方向").label)=="无对应灯"&&signal_badge("未关联此方向").color==3);
    assert(std::string(signal_badge("信号暂失").label)=="暂失"&&signal_badge("信号暂失").color!=4);
    assert(std::string(signal_badge("信号冲突").label)=="冲突"&&signal_badge("信号冲突").color!=4);
    for(const char* value:{"当前未见灯","前视证据不足","信号待核实",""})assert(signal_badge(value).color!=4);
    assert(marking_badge(1).color==6);
    assert(std::string(marking_badge(2).label)=="虚线");
    assert(std::string(marking_badge(2).label).find("允许")==std::string::npos);
    Explanation e;e.lane="当前位于车道中央";assert(lane_title(e)=="本车道待确认");
    e.lane_position_known=true;assert(lane_title(e)=="居中");
    auto width=[](const std::string& s){int n=0;for(unsigned char c:s)if((c&0xc0)!=0x80)++n;return n;};
    auto previous=[&](std::string value,int limit){if(limit<=0)return std::string();if(width(value)<=limit)return value;
        if(width("…")>limit)return std::string();
        while(!value.empty()&&width(value+"…")>limit){size_t end=value.size()-1;while(end&&(static_cast<unsigned char>(value[end])&0xc0)==0x80)--end;value.resize(end);}
        return value+"…";};
    for(const std::string value:{"","abcdef","车道线方向确认中","A红绿灯123·…"})for(int limit=0;limit<30;++limit)
        assert(fit_label(value,limit,width)==previous(value,limit));
    int calls=0;assert(!fit_label(std::string(4096,'a'),40,[&](const std::string& s){++calls;return width(s);}).empty());
    assert(calls<=15); // Bounded text measurements even when a long label is clipped.
    for(const auto& size:{std::array<int,2>{640,444},{800,534},{1280,804},{1600,1030}}){
        signal_demo::MouseLayout mouse;mouse.resize(size[0],size[1]);
        assert(mouse.py==0&&mouse.pw==size[0]&&mouse.ph<=mouse.vy);
        DashboardLayout layout{mouse.pw,mouse.ph};
        for(int i=0;i<3;++i){auto c=layout.card(i);assert(c.x>=0&&c.w>0&&c.y+c.h<mouse.ph);
            if(i)assert(layout.card(i-1).x+layout.card(i-1).w<c.x);}
        for(int side=0;side<2;++side){auto button=layout.lane_button(side);auto card=layout.card(1);
            assert(button.x>=card.x&&button.x+button.w<=card.x+card.w);
            assert(button.y>=card.y&&button.y+button.h<card.y+card.h);
            assert(layout.lane_hit(button.x+button.w/2,button.y+button.h/2)==side);
            assert(layout.lane_hit(button.x+button.w,button.y)==-1);}
        assert(layout.lane_hit(0,0)==-1);
    }
}
