#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
namespace ai {
// Jobs: front detection, front joint detection+lane, other CH0..CH3 detection.
// Fairness is measured in completed service time, not a fixed Hz or job count.
class LiveSchedule {
    std::array<double,6> service{};
    std::array<bool,6> was_ready{};
    double floor=0;
public:
    int choose(const std::array<bool,6>& ready,int preferred=-1){
        int best=-1;
        for(size_t i=0;i<6;++i){
            if(ready[i] && !was_ready[i]) service[i]=std::max(service[i],floor);
            if(ready[i] && (best<0 || service[i]<service[size_t(best)]))best=int(i);
            was_ready[i]=ready[i];
        }
        // Wake-up fairness follows the least-served ready job. A forced deadline
        // must not jump this floor to an over-served deep job's virtual time.
        if(best>=0)floor=std::max(floor,service[size_t(best)]);
        if(preferred>=0 && preferred<6 && ready[size_t(preferred)])best=preferred;
        return best;
    }
    void complete(int job,double elapsed_ms,double weight){
        if(job<0 || job>=6 || !std::isfinite(elapsed_ms) || elapsed_ms<0 || !std::isfinite(weight) || weight<=0)
            throw std::runtime_error("Invalid scheduler service");
        service[size_t(job)]+=std::max(.01,elapsed_ms)/weight;
    }
};
inline bool model_deadline_due(double now,double last_start,unsigned deadline_ms){
    return deadline_ms && (last_start<0 || (now-last_start)*1000>=deadline_ms);
}
struct LiveOptions {
    unsigned front=0;
    double shallow_weight=4,deep_weight=3,other_weight=1;
    unsigned save_every=0;
    bool light_candidates=false;
    bool automatic_signal=false;
    bool packed_signal_shape=true;
    std::string front_model;
    std::string light_rgb_model;
    float light_rgb_probability=.6f,light_rgb_margin=.15f;
    unsigned light_rgb_max=4;
    bool native_output=false,full_mosaic=false,lane_post_async=false;
    bool lane_hybrid=false;
    unsigned lane_deadline_ms=0,hybrid_semantic_ms=750;
};
}
