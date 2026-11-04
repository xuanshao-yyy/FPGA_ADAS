#pragma once
#include "signal_observation.hpp"
#include <unordered_map>
namespace ai {
// Only physical arrow structure is remembered. Color must come from the new frame.
class SignalStructure {
    struct Entry {Box box;int direction=-1,candidate=-1;unsigned streak=0;double seen=-1,confirmed=-1;std::string shape;};
    std::unordered_map<uint64_t,Entry> memory;
public:
    void reset(){memory.clear();}
    void update(std::vector<SignalCandidate>& candidates,double stamp){
        for(auto it=memory.begin();it!=memory.end();)if(stamp-it->second.seen>1.5)it=memory.erase(it);else ++it;
        for(auto& c:candidates){
            if(!c.track_id)continue;
            auto found=memory.find(c.track_id);
            if(found!=memory.end() && (stamp<=found->second.seen || stamp-found->second.seen>.5 || overlap(c.box,found->second.box)<.2f)){
                memory.erase(found);found=memory.end();
            }
            if(c.direction>=0 && c.direction<3 && c.color!="UNKNOWN"){
                auto& e=memory[c.track_id];
                if(e.candidate!=c.direction){e.direction=-1;e.candidate=c.direction;e.streak=0;e.confirmed=-1;}
                if(++e.streak>=3){e.direction=c.direction;e.confirmed=stamp;e.shape=c.shape;}
                e.seen=stamp;e.box=c.box;
            }else if(found!=memory.end()){
                auto& e=found->second;
                bool structure_missing=c.shape=="UNKNOWN" && (c.reason=="PICTOGRAM_AMBIGUOUS" || c.reason=="PICTOGRAM_SMALL_OR_INCOMPLETE" || c.reason=="MULTIPLE_PICTOGRAM_COMPONENTS");
                if(e.direction>=0 && stamp-e.confirmed<=1.5 && structure_missing && c.color!="UNKNOWN"){
                    c.direction=e.direction;c.shape=e.shape;c.reason="TRACKED_ARROW_STRUCTURE_FRESH_COLOR";
                }
                // Known circle/down/conflicting structure never inherits a previous arrow.
                if(c.shape!="UNKNOWN" && c.shape!=e.shape && c.direction<0){memory.erase(found);continue;}
                e.seen=stamp;e.box=c.box;
            }
        }
        // Bound storage even if a broken tracker continuously invents IDs.
        if(memory.size()>64)memory.clear();
    }
};
}
