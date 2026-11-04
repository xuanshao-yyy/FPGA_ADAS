#pragma once
#include "signal_observation.hpp"
#include <algorithm>
#include <unordered_map>

namespace ai {
// Practical direction association for the simulation UI. Layout is evidence,
// not proof of ego-lane relevance. Never remember a lamp's color here.
class SignalAssociation {
    struct Role {Box box;int direction;double seen;std::string basis;uint64_t identity;};
    std::unordered_map<uint64_t,Role> roles;
    std::vector<uint64_t> pending;
    double last=-1,first=-1,group_seen=-1;
    unsigned samples=0;
public:
    static bool eligible(const SignalCandidate& c){
        const auto& b=c.box;float w=b.x2-b.x1,h=b.y2-b.y1;
        return c.track_id&&b.category==7&&std::isfinite(b.x1)&&std::isfinite(b.x2)&&std::isfinite(b.y1)&&std::isfinite(b.y2)&&
            std::isfinite(b.score)&&b.score>=.12f&&b.x1>=0&&b.x2<=1280&&b.y1>=0&&b.y2<=504&&
            w>=4&&h>=6&&w<=384&&h<=240&&h/w>=.25f&&h/w<=6&&c.shape!="DOWN";
    }
public:
    void reset(){roles.clear();pending.clear();last=first=group_seen=-1;samples=0;}
    void update(std::vector<SignalCandidate>& items,double stamp){
        if(!std::isfinite(stamp)||stamp<0||stamp<=last){reset();return;}
        if(last>=0&&stamp-last>.5){pending.clear();samples=0;}
        last=stamp;
        for(auto it=roles.begin();it!=roles.end();)if(stamp-it->second.seen>1.5)it=roles.erase(it);else ++it;
        std::array<bool,3> explicit_direction{};
        std::vector<SignalCandidate*> heads;
        bool incompatible=false;
        for(auto& c:items){
            if(c.direction>=0&&c.direction<3)explicit_direction[size_t(c.direction)]=true;
            auto old=roles.find(c.track_id);
            if(old!=roles.end()&&(overlap(old->second.box,c.box)<.2f||c.shape=="DOWN"||
                (c.direction>=0&&c.direction!=old->second.direction))){roles.clear();pending.clear();samples=0;incompatible=true;}
            if(eligible(c))heads.push_back(&c);
        }
        if(incompatible)return; // Fresh explicit arrows remain untouched.
        // The tracker can briefly split one tiny housing into two IDs. Reattach
        // only a uniquely overlapping, recently seen physical role, not its color.
        for(auto* c:heads)if(c->direction<0&&!roles.count(c->track_id)){
            Role match{};bool found=false,ambiguous=false;
            for(const auto& old:roles)if(stamp-old.second.seen<=.35&&overlap(c->box,old.second.box)>.45f){
                if(found&&match.identity!=old.second.identity){ambiguous=true;break;}
                match=old.second;found=true;
            }
            if(found&&!ambiguous){match.box=c->box;match.seen=stamp;roles.emplace(c->track_id,std::move(match));}
        }
        if(roles.size()>64){reset();return;}
        // Restore each known identity before considering newly appearing boxes.
        // An unrelated new head must not suppress the already associated heads.
        for(auto* c:heads){auto found=roles.find(c->track_id);if(found==roles.end())continue;
            auto& r=found->second;r.box=c->box;r.seen=stamp;
            if(c->direction<0&&!explicit_direction[size_t(r.direction)]){c->direction=r.direction;c->direction_basis=r.basis;c->association_id=r.identity;}}
        std::sort(heads.begin(),heads.end(),[](const auto* a,const auto* b){return a->box.x1<b->box.x1;});
        std::vector<SignalCandidate*> unique;
        for(auto* c:heads){bool duplicate=false;
            for(auto*& old:unique)if(c->track_id==old->track_id||overlap(c->box,old->box)>.5f){
                duplicate=true;
                if(c->color!="UNKNOWN"&&old->color!="UNKNOWN"&&c->color!=old->color){pending.clear();samples=0;return;}
                if(old->color=="UNKNOWN"&&c->color!="UNKNOWN")old=c;
                break;
            }
            if(!duplicate)unique.push_back(c);
        }
        heads=std::move(unique);
        const size_t visible_heads=heads.size();
        // Prefer a coherent housing row over counting every colored box.
        SignalCandidate* anchor=nullptr;
        for(auto* c:heads)if(c->direction>=0&&(!anchor||c->box.y2-c->box.y1>anchor->box.y2-anchor->box.y1))anchor=c;
        if(!anchor)for(auto* c:heads)if(!anchor||c->box.y2-c->box.y1>anchor->box.y2-anchor->box.y1)anchor=c;
        if(anchor){auto a=anchor->box;float h0=a.y2-a.y1;
            heads.erase(std::remove_if(heads.begin(),heads.end(),[&](const auto* c){const auto& b=c->box;float h=b.y2-b.y1;
                return h/h0<.65f||h/h0>1.55f||std::abs((b.y1+b.y2)-(a.y1+a.y2))>std::max(h,h0)*.6f;}),heads.end());}
        // Known roles survive missing neighbours. Never re-rank the remaining head.
        if(!heads.empty()&&std::all_of(heads.begin(),heads.end(),[](const auto* c){return c->direction>=0;}))return;
        // Expand a group only when every old role is present and the proposed
        // order agrees. A partial group must never be re-numbered.
        for(const auto& role:roles)if(std::none_of(heads.begin(),heads.end(),[&](const auto* c){return c->track_id==role.first||c->association_id==role.second.identity;})){
            pending.clear();samples=0;return;}
        if(roles.empty()&&group_seen>=0&&stamp-group_seen<=1.5){pending.clear();samples=0;return;}
        if(heads.empty()||heads.size()>3){pending.clear();samples=0;return;}
        if(heads.size()==1){float center=(heads[0]->box.x1+heads[0]->box.x2)*.5f;
            if(visible_heads!=1||center<192||center>1088){pending.clear();samples=0;return;}}
        std::vector<uint64_t> ids;
        for(size_t i=0;i<heads.size();++i){const auto& b=heads[i]->box;const auto& a=heads[0]->box;
            float h=b.y2-b.y1,h0=a.y2-a.y1;
            int direction=heads.size()==1?1:int(i);
            if(h/h0<.5f||h/h0>2.f||std::abs((b.y1+b.y2)-(a.y1+a.y2))>std::max(h,h0)||
                (i&&b.x1<=heads[i-1]->box.x2)||(heads[i]->direction>=0&&heads[i]->direction!=direction)){
                pending.clear();samples=0;return;}
            ids.push_back(heads[i]->track_id);
        }
        if(ids!=pending){pending=ids;samples=0;first=stamp;}
        if(++samples<2||stamp-first<.08-1e-7)return;
        const std::string basis=heads.size()==1?"LAYOUT_SINGLE_GENERAL":heads.size()==2?"LAYOUT_TWO_ORDER":"LAYOUT_THREE_ORDER";
        if(heads.size()>1)group_seen=stamp;
        for(size_t i=0;i<heads.size();++i){auto& c=*heads[i];int d=heads.size()==1?1:int(i);
            uint64_t identity=c.association_id?c.association_id:c.track_id;
            roles[c.track_id]={c.box,d,stamp,basis,identity};
            if(c.direction<0&&!explicit_direction[size_t(d)]){c.direction=d;c.direction_basis=basis;c.association_id=identity;}
        }
        if(roles.size()>64)reset();
    }
};
}
