#pragma once
#include "lane.hpp"
#include "tracking.hpp"
#include <string>
#include <rknn_api.h>

namespace ai {
struct RawLane {
    rknn_tensor_attr attr{};
    std::vector<int8_t> quantized;
    std::vector<float> floating;
    double inputs_ms=0,run_ms=0,get_ms=0,copy_ms=0,release_ms=0;
};
class LaneSegmenter {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    LaneSegmenter(const LaneOptions& options,const std::string& output);
    ~LaneSegmenter();
    void reset(unsigned channel); // Only while worker threads are joined.
    RawLane capture(uint8_t* rgb);
    LaneMask process(const RawLane& raw,float confidence,const std::vector<Box>& boxes,
                     const std::vector<TrackedBox>& tracks,double timestamp,unsigned channel);
    LaneMask infer(uint8_t* rgb,float confidence,double& api_ms,double& post_ms,
                   const std::vector<Box>& boxes={},const std::vector<TrackedBox>& tracks={},double timestamp=-1,unsigned channel=0);
};
void verify_lane(const LaneOptions& options,const std::string& output,unsigned warmup,const std::string& probe,bool parity=false);
}
