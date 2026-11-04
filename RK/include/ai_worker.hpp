#pragma once
#include "detection.hpp"
#include "protocol.hpp"
#include "lane.hpp"
#include "tracking.hpp"
#include "live_schedule.hpp"
#include "signal_observation.hpp"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace ai {
struct Stats {
    uint64_t completed=0,replaced=0,stale=0,late=0;
    uint64_t cpu_replaced=0,cpu_stale=0;
    double preprocess_ms=0,api_ms=0,postprocess_ms=0,pipeline_ms=0,frame_age_ms=0;
    float max_class_score=0;
    double tracking_ms=0;
};
struct Result {
    uint32_t frame_id=0,source=0;
    double received=0,finished=0;
    std::vector<Box> boxes;
    std::vector<TrackedBox> tracks;
    bool tracking=false;
    float display_threshold=.25f;
    float light_display_threshold=0; // Explicit class-7 override; zero preserves legacy display.
    std::vector<SignalCandidate> signal_candidates; // Same original frame, never re-read from a newer image.
};
struct HybridStats {
    uint64_t attempts=0,completed=0,rejected=0,replaced=0;
    double cpu_ms=0,max_gap_ms=0,last_finished=0;
};
class Worker {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    Worker(const std::string& model,const std::string& heap,int rgb565_format,
           const std::array<bool,4>& channels,double fps,float confidence,float iou,
           unsigned max_age_ms,const std::string& output,unsigned warmup,bool mosaic=false,bool diagnose=false,
           const LaneOptions& lane={},const TrackingOptions& tracking={},const LiveOptions& live={});
    ~Worker();
    Worker(const Worker&)=delete;
    Worker& operator=(const Worker&)=delete;
    void start();
    void stop();
    void reset_scene(unsigned channel); // stop -> reset -> start; keeps loaded RKNN contexts and cumulative metrics.
    void publish(unsigned channel,std::shared_ptr<qv::Frame>);
    Result result(unsigned channel) const;
    Stats stats(unsigned channel) const;
    std::shared_ptr<const LaneMask> lane_result(unsigned channel) const;
    Stats lane_stats(unsigned channel) const;
    HybridStats hybrid_stats() const;
    uint64_t inference_count() const;
    std::string error() const;
};
std::string select_model(const std::string& directory,const std::string& explicit_path);
void verify_model(const std::string& model,const std::string& output,unsigned warmup,const std::string& probe="",bool native=false,bool parity=false);
}
