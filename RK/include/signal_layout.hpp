#pragma once
#include "signal_observation.hpp"
#include <algorithm>
#include <vector>
namespace ai {
// A layout PRIOR, not a traffic rule: two heads -> left/general, three -> left/straight/right.
// Detection can miss a head or include another approach. Never feed this into State::observe.
class SignalLayoutHints {
    std::vector<SignalCandidate> previous;
    double last=-1,first=-1;unsigned samples=0;
public:
    std::array<std::string,3> colors{};
    const char* reason="NO_LAYOUT";
    void reset(){previous.clear();last=first=-1;samples=0;colors={};reason="NO_LAYOUT";}
    void update(const std::vector<SignalCandidate>& input,double stamp){
        colors={};reason="LAYOUT_REJECTED";
        if(!std::isfinite(stamp)||stamp<0||stamp<=last){reset();return;}
        std::vector<SignalCandidate> heads;
        for(const auto& c:input){
            const auto& b=c.box;float w=b.x2-b.x1,h=b.y2-b.y1;
            if(!c.track_id||!std::isfinite(w)||!std::isfinite(h)||!std::isfinite(b.x1)||!std::isfinite(b.y1)||
               b.x1<0||b.x2>1280||b.y1<0||b.y2>468||w<4||h<8||h/w<1.3f||h/w>6||c.shape=="DOWN"){reset();return;}
            heads.push_back(c);
        }
        if(heads.size()!=2&&heads.size()!=3){reset();return;}
        std::sort(heads.begin(),heads.end(),[](const auto& a,const auto& b){return a.box.x1<b.box.x1;});
        bool same=previous.size()==heads.size()&&last>=0&&stamp-last<=.3;
        for(size_t i=0;i<heads.size();++i){
            const auto& c=heads[i];const auto& b=c.box;float h=b.y2-b.y1,h0=heads[0].box.y2-heads[0].box.y1;
            if(h/h0<.65f||h/h0>1.55f||std::abs((b.y1+b.y2)-(heads[0].box.y1+heads[0].box.y2))>std::max(h,h0)*.6f||
               (i&&b.x1<=heads[i-1].box.x2)||(c.direction>=0&&c.direction!=int(i))){reset();return;}
            for(size_t j=0;j<i;++j)if(heads[j].track_id==c.track_id){reset();return;}
            if(same&&(c.track_id!=previous[i].track_id||overlap(b,previous[i].box)<.5f))same=false;
        }
        if(!same){samples=0;first=stamp;}
        ++samples;last=stamp;previous=heads;reason="LAYOUT_CONFIRMING";
        if(samples<3||stamp-first<.2-1e-7)return;
        reason=heads.size()==2?"TWO_HEAD_ORDER_PRIOR_NOT_DIRECTION_PROOF":"THREE_HEAD_ORDER_PRIOR_NOT_DIRECTION_PROOF";
        for(size_t i=0;i<heads.size();++i)if(heads[i].color=="RED"||heads[i].color=="GREEN"||heads[i].color=="YELLOW")colors[i]=heads[i].color;
    }
};
}
