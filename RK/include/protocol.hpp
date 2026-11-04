#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace qv {
constexpr unsigned Width=1280, Height=720, Chunk=1440, Packets=1280;
constexpr unsigned Header=28, Datagram=Header+Chunk, FrameBytes=Width*Height*2;
// R2 and jumbo R3 use the same QV02 fields and unchanged RGB565 pixels.
constexpr unsigned JumboChunk=8192, JumboPackets=FrameBytes/JumboChunk;
constexpr unsigned MaxDatagram=Header+JumboChunk;
static_assert(FrameBytes%JumboChunk==0,"Jumbo packets must divide the frame");
double now_seconds();
// Optional externally allocated storage. Only the RK preview uses DMA memory;
// the original receiver and tests retain their ordinary heap allocation.
struct FrameMemory {
    virtual ~FrameMemory()=default;
    virtual uint8_t* data() const=0;
    virtual void begin_write()=0;
    virtual void finish_write()=0;
};
struct PixelDeleter {
    bool owned=true;
    void operator()(uint8_t* p) const {if(owned)delete[] p;}
};
struct Frame {
    // No zero-fill: only a fully received frame can leave the assembler.
    std::shared_ptr<FrameMemory> memory;
    std::unique_ptr<uint8_t[],PixelDeleter> pixels;
    Frame():pixels(new uint8_t[FrameBytes],PixelDeleter{true}){}
    explicit Frame(std::shared_ptr<FrameMemory> storage)
        :memory(std::move(storage)),pixels(memory->data(),PixelDeleter{false}){}
    uint32_t id=0, source=0;
    double received=0;
    bool still_image_test=false;
    double content_received=0; // Original source publication time, unchanged for image-test repeats.
};
using FrameFactory=std::function<std::unique_ptr<Frame>()>;
class FramePool {
    struct State {
        std::mutex mutex;
        std::vector<std::unique_ptr<Frame>> free;
    };
    std::shared_ptr<State> state_;
public:
    explicit FramePool(unsigned capacity=12,FrameFactory factory={});
    std::shared_ptr<Frame> acquire();
};
struct Stats {
    uint64_t packets=0, bytes=0, complete=0, source_updates=0;
    uint64_t incomplete=0, missing=0, invalid=0, duplicates=0, old=0;
    uint64_t pool_exhausted=0, kernel_drops=0, foreign=0;
    uint64_t rx_batches=0,still_repeats=0;
};
class Assembler {
    struct Partial {
        std::shared_ptr<Frame> frame;
        std::array<uint8_t,Packets> seen{};
        unsigned count=0;
        unsigned packets=Packets, chunk=Chunk;
        double started=0;
    };
    FramePool pool_;
    std::vector<Partial> pending_;
    bool have_last_=false, have_source_=false;
    uint32_t last_=0, last_source_=0;
    double completed_at_=0;
    unsigned composite_=0; // 0 QV02 camera, 1 QV03 parking, 2 QV04 panorama
    void discard(size_t index);
public:
    Stats stats;
    explicit Assembler(unsigned capacity=12,FrameFactory factory={},unsigned composite=0):pool_(capacity,std::move(factory)),composite_(composite){}
    std::shared_ptr<Frame> feed(const uint8_t*,size_t,double);
    void expire(double);
};
// Latest complete frame only. Receiving never waits for the encoder.
class Latest {
    std::mutex mutex_;
    std::shared_ptr<Frame> frame_;
    uint64_t replaced_=0;
    std::function<void(const std::shared_ptr<Frame>&)> observer_;
public:
    void observe(std::function<void(const std::shared_ptr<Frame>&)>);
    void put(std::shared_ptr<Frame>);
    std::shared_ptr<Frame> take();
    uint64_t replaced();
};
}
