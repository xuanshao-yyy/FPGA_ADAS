#pragma once
#include "protocol.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace qv {
struct Endpoint {
    std::string bind,peer;
    uint16_t port;
    unsigned fps;
    std::string name;
};
class UdpReceiver {
    Endpoint ep_;
    FrameFactory factory_;
    unsigned capacity_=12,coalesce_us_=0;
    unsigned composite_=0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    Stats published_;
    std::string error_;
    intptr_t socket_=-1;
    int receive_buffer_=0;
    void run();
public:
    Latest latest;
    explicit UdpReceiver(Endpoint,FrameFactory factory={},unsigned capacity=12,unsigned coalesce_us=0,unsigned composite=0);
    ~UdpReceiver();
    void start();
    void stop();
    Stats stats() const;
    std::string error() const;
    int receive_buffer() const {return receive_buffer_;}
};
}
