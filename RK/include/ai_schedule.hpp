#pragma once
#include <cmath>
#include <stdexcept>
#include <string>

namespace ai {
// Zero removes the software rate limit; it never means a zero-duration busy loop.
inline double parse_fps(const std::string& raw){
    size_t end=0;double value=std::stod(raw,&end);
    if(end!=raw.size() || !std::isfinite(value) || value<0 || value>30)
        throw std::runtime_error("invalid --ai-fps: use 0 for unlimited, or (0,30]");
    return value;
}
inline double next_due(double now,double fps){return fps==0?now:now+1/fps;}
}
