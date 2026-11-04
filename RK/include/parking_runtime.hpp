#pragma once
#include "protocol.hpp"
#include <memory>
#include <string>
namespace parking {
struct Options {std::string model,socket,rules,active,heap,output;int rgb565=0;};
class Runtime {
    struct Impl;std::unique_ptr<Impl> impl;
public:
    explicit Runtime(const Options& options);
    ~Runtime();
    void submit(std::shared_ptr<qv::Frame> frame,double video_pts);
};
}
