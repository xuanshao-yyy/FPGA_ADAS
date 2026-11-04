#pragma once
#include "detection.hpp"
#include <cstdint>
#include <string>
namespace ai {
struct SignalCandidate {
    Box box{};
    uint64_t track_id=0;
    std::string color="UNKNOWN",shape="UNKNOWN",reason="NO_COLOR_COMPONENT";
    int direction=-1; // LEFT=0, STRAIGHT=1, RIGHT=2; round/down/unknown remain unassigned.
    std::string direction_basis; // Layout source, separate from the raw color/shape failure reason.
    uint64_t association_id=0; // Stable housing identity across a unique, overlapping tracker-ID handover.
    float shape_score=0,shape_margin=0;
    std::string hsv_color="UNKNOWN",rgb_color="UNKNOWN",rgb_reason="NOT_ENABLED";
    std::array<float,3> rgb_probabilities{};
    float rgb_probability=0,rgb_margin=0;
};
}
