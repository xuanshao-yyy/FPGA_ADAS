#include "protocol.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace qv {
double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
FramePool::FramePool(unsigned capacity,FrameFactory factory):state_(std::make_shared<State>()) {
    for(unsigned i=0;i<capacity;++i) state_->free.push_back(factory?factory():std::make_unique<Frame>());
}
std::shared_ptr<Frame> FramePool::acquire() {
    auto state=state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    if(state->free.empty()) return {};
    if(state->free.back()->memory)state->free.back()->memory->begin_write();
    Frame* f=state->free.back().release();
    state->free.pop_back();
    // Capturing the pool state lets GstMemory outlive the receiver safely.
    return std::shared_ptr<Frame>(f,[state](Frame* p){
        std::lock_guard<std::mutex> guard(state->mutex);
        state->free.emplace_back(p);
    });
}
static uint16_t be16(const uint8_t* p){return uint16_t((unsigned(p[0])<<8)|p[1]);}
static uint32_t be32(const uint8_t* p){return (uint32_t(p[0])<<24)|(uint32_t(p[1])<<16)|(uint32_t(p[2])<<8)|p[3];}
void Assembler::discard(size_t i) {
    ++stats.incomplete;
    stats.missing+=pending_[i].packets-pending_[i].count;
    pending_.erase(pending_.begin()+i);
}
void Assembler::expire(double now) {
    for(size_t i=0;i<pending_.size();) {
        if(now-pending_[i].started>0.300) discard(i); else ++i;
    }
}
std::shared_ptr<Frame> Assembler::feed(const uint8_t* p,size_t size,double now) {
    if(composite_>2 || size<Header || size>MaxDatagram || std::memcmp(p,composite_==2?"QV04":composite_==1?"QV03":"QV02",4)!=0) {++stats.invalid;return {};}
    const uint32_t fid=be32(p+4), offset=be32(p+16), source=be32(p+24);
    const unsigned pid=be16(p+8);
    const unsigned packets=be16(p+10);
    const unsigned chunk=packets==Packets?Chunk:(packets==JumboPackets?JumboChunk:0);
    if(!chunk || size!=Header+chunk || pid>=packets || be16(p+12)!=(composite_==2?2560:Width) ||
       be16(p+14)!=(composite_==2?360:Height) || offset!=pid*chunk || be16(p+20)!=chunk || be16(p+22)!=1) {
        ++stats.invalid;return {};
    }
    ++stats.packets; stats.bytes+=size;
    if(have_last_ && now-completed_at_>1.0) {
        // Reprogram/reboot recovery. Retire partial frames from the old epoch.
        while(!pending_.empty()) discard(0);
        have_last_=false; have_source_=false;
    }
    if(have_last_) {
        const uint32_t delta=fid-last_;
        if(delta==0){++stats.duplicates;return {};}
        if(delta>=0x80000000u){++stats.old;return {};}
    }
    size_t i=0;
    for(;i<pending_.size();++i) if(pending_[i].frame->id==fid) break;
    if(i==pending_.size()) {
        expire(now);
        if(pending_.size()>=3) discard(0);
        auto f=pool_.acquire();
        if(!f){++stats.pool_exhausted;return {};}
        f->id=fid;f->source=source;
        Partial part;part.frame=std::move(f);part.started=now;part.packets=packets;part.chunk=chunk;
        pending_.push_back(std::move(part));i=pending_.size()-1;
    }
    auto& part=pending_[i];
    if(part.frame->source!=source || part.packets!=packets || part.chunk!=chunk){++stats.invalid;return {};}
    if(part.seen[pid]){++stats.duplicates;return {};}
    std::memcpy(part.frame->pixels.get()+offset,p+Header,chunk);
    part.seen[pid]=1;
    if(++part.count!=packets) return {};
    // Publish only after CPU writes are visible to DMA devices.
    if(part.frame->memory)part.frame->memory->finish_write();
    auto result=part.frame;
    result->received=now;
    pending_.erase(pending_.begin()+i);
    ++stats.complete;
    if(source!=0 && (!have_source_ || source!=last_source_)) ++stats.source_updates;
    last_source_=source;have_source_=true;
    last_=fid;have_last_=true;completed_at_=now;
    // Once a newer frame completes, older partials must not be published later.
    for(size_t j=0;j<pending_.size();) {
        if(uint32_t(pending_[j].frame->id-last_)>=0x80000000u) discard(j); else ++j;
    }
    return result;
}
void Latest::observe(std::function<void(const std::shared_ptr<Frame>&)> callback){
    std::lock_guard<std::mutex> lock(mutex_);observer_=std::move(callback);
}
void Latest::put(std::shared_ptr<Frame> f) {
    std::function<void(const std::shared_ptr<Frame>&)> callback;
    {std::lock_guard<std::mutex> lock(mutex_);callback=observer_;}
    // Only a sealed complete frame reaches this hook. The callback enqueues a
    // shared lease; inference never runs in the network/source thread.
    if(callback)callback(f);
    std::lock_guard<std::mutex> lock(mutex_);
    if(frame_)++replaced_;
    frame_=std::move(f);
}
std::shared_ptr<Frame> Latest::take() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto f=std::move(frame_);return f;
}
uint64_t Latest::replaced(){std::lock_guard<std::mutex> lock(mutex_);return replaced_;}
}
