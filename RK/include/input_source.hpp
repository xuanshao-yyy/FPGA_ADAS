#pragma once
#include "udp.hpp"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace qv {
// Only the source changes: published frames use the same DMA ownership and clock
// as UDP frames. Small clips are preloaded; large clips use a sliding mmap
// working set. The latter includes local disk/page-cache overhead in the test.
class MemoryVideo {
    std::vector<uint8_t> bytes_;
    uint8_t* mapped_=nullptr;
    size_t size_=0,advised_begin_=0,advised_end_=0;
    void prepare_page_window(size_t offset){
        if(!mapped_)return;
        const size_t page=size_t(sysconf(_SC_PAGESIZE));
        const size_t block=8*FrameBytes;
        const size_t begin=(offset/block*block)/page*page;
        const size_t end=std::min(size_,begin+16*FrameBytes);
        if(begin==advised_begin_&&end==advised_end_)return;
        // Frames are copied into the DMA pool before this mapping is reused.
        if(begin<advised_begin_||begin>=advised_end_){
            if(advised_end_>advised_begin_)madvise(mapped_+advised_begin_,advised_end_-advised_begin_,MADV_DONTNEED);
        }else if(begin>advised_begin_)madvise(mapped_+advised_begin_,begin-advised_begin_,MADV_DONTNEED);
        madvise(mapped_+begin,end-begin,MADV_WILLNEED);
        advised_begin_=begin;advised_end_=end;
    }
    FramePool pool_;
    double fps_;
    bool loop_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_=false,eof_=false,paused_=false,image_test_=false,resume_seek_=false;
    uint64_t frozen_tick_=0,last_tick_=0,resume_tick_=0,delivery_id_=0;
    double frozen_received_=0,last_content_received_=0;
    std::chrono::steady_clock::time_point pause_begin_{};
    std::chrono::steady_clock::duration pause_total_{};
    Stats stats_;
    uint64_t skipped_=0,loops_=0;
    double copy_ms_=0;
    std::string error_;
    void publish_tick(uint64_t tick,bool still){
        auto frame=pool_.acquire();if(!frame){++stats_.pool_exhausted;return;}
        const auto copy_start=std::chrono::steady_clock::now();
        const auto count=size_/FrameBytes;
        const size_t offset=(tick%count)*FrameBytes;prepare_page_window(offset);
        std::memcpy(frame->pixels.get(),(mapped_?mapped_:bytes_.data())+offset,FrameBytes);
        if(frame->memory)frame->memory->finish_write();
        delivery_id_=std::max(delivery_id_+1,tick+1);
        frame->id=uint32_t(delivery_id_);frame->source=uint32_t(tick+1);
        frame->received=now_seconds();frame->still_image_test=still;
        frame->content_received=still?frozen_received_:frame->received;
        if(still)++stats_.still_repeats;
        else {++stats_.source_updates;last_tick_=tick;last_content_received_=frame->received;}
        copy_ms_+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-copy_start).count();
        latest.put(std::move(frame));++stats_.complete;
    }
    void run() {
        try {
            const uint64_t count=size_/FrameBytes;
            auto start=std::chrono::steady_clock::now();uint64_t tick=0;
            while(true){
                std::unique_lock<std::mutex> lock(mutex_);if(stopping_)break;
                if(paused_){
                    if(!image_test_){wake_.wait(lock,[&]{return stopping_||!paused_||image_test_;});continue;}
                    publish_tick(frozen_tick_,true);
                    wake_.wait_for(lock,std::chrono::duration<double>(1/fps_),[&]{return stopping_||!paused_||!image_test_;});
                    continue;
                }
                if(resume_seek_){tick=resume_tick_;pause_total_={};start=std::chrono::steady_clock::now()-std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(tick/fps_));resume_seek_=false;}
                const auto due=start+pause_total_+std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(tick/fps_));
                if(wake_.wait_until(lock,due,[&]{return stopping_||paused_;})){if(stopping_)break;continue;}
                const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start-pause_total_).count();
                const uint64_t current=std::max(tick,uint64_t(std::floor(std::max(0.,elapsed)*fps_)));
                skipped_+=current-tick;tick=current;
                if(!loop_ && tick>=count){eof_=true;break;}
                publish_tick(tick,false);loops_=tick/count;++tick;
                if(!loop_ && tick>=count){eof_=true;break;}
            }
        }catch(const std::exception& e){std::lock_guard<std::mutex> lock(mutex_);error_=e.what();}
    }
public:
    Latest latest;
    static uint64_t validated_size(const std::string& path){
        if(!std::filesystem::is_regular_file(path))throw std::runtime_error("Local frame file not found: "+path);
        const auto bytes=std::filesystem::file_size(path);
        if(!bytes || bytes%FrameBytes)throw std::runtime_error("Expected packed 1280x720 RGB565 LE, not MP4: "+path);
        return bytes;
    }
    MemoryVideo(const std::string& path,double fps,bool loop,FrameFactory factory={},unsigned capacity=8,size_t preload_limit=512ull*1024*1024)
        :pool_(capacity,std::move(factory)),fps_(fps),loop_(loop){
        if(!std::isfinite(fps_)||fps_<1||fps_>120)throw std::runtime_error("Local source fps must be in [1,120]");
        size_=validated_size(path);
        if(size_>preload_limit){
            const int fd=open(path.c_str(),O_RDONLY);if(fd<0)throw std::runtime_error("Could not open mapped frame file: "+path);
            void* data=mmap(nullptr,size_,PROT_READ,MAP_PRIVATE,fd,0);close(fd);
            if(data==MAP_FAILED)throw std::runtime_error("Could not map local frame file: "+path);
            mapped_=static_cast<uint8_t*>(data);madvise(mapped_,size_,MADV_SEQUENTIAL);
        }else{
            bytes_.resize(size_);std::ifstream file(path,std::ios::binary);file.read(reinterpret_cast<char*>(bytes_.data()),bytes_.size());
            if(!file || file.gcount()!=std::streamsize(size_))throw std::runtime_error("Could not preload local frame file: "+path);
        }
    }
    ~MemoryVideo(){stop();if(mapped_)munmap(mapped_,size_);}
    void start(){if(thread_.joinable())throw std::runtime_error("Local source already started");thread_=std::thread([this]{run();});}
    void stop(){ {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;wake_.notify_all();}if(thread_.joinable())thread_.join(); }
    void pause(bool value,bool image_test=false,uint32_t source=0,double content_received=0){
        std::lock_guard<std::mutex> lock(mutex_);if(value==paused_)return;
        auto now=std::chrono::steady_clock::now();
        if(value){pause_begin_=now;image_test_=image_test;frozen_tick_=source?uint64_t(source)-1:last_tick_;frozen_received_=content_received>0?content_received:last_content_received_;}
        else {pause_total_+=now-pause_begin_;if(image_test_){resume_seek_=true;resume_tick_=frozen_tick_+1;}image_test_=false;}
        paused_=value;wake_.notify_all();
    }
    Stats stats()const{std::lock_guard<std::mutex> lock(mutex_);return stats_;}
    std::string error()const{std::lock_guard<std::mutex> lock(mutex_);return error_;}
    void write_summary(std::ostream& out,unsigned channel)const{
        std::lock_guard<std::mutex> lock(mutex_);
        out<<channel<<','<<(mapped_?"mapped_rgb565":"preloaded_rgb565")<<','<<fps_<<','<<size_<<','<<stats_.complete<<','<<skipped_<<','<<loops_<<','<<eof_<<','<<copy_ms_<<','<<stats_.still_repeats<<'\n';
    }
};
class InputSource {
    std::unique_ptr<UdpReceiver> udp_;
    std::unique_ptr<MemoryVideo> file_;
public:
    InputSource(Endpoint ep,const std::string& path,double fps,bool loop,FrameFactory factory,unsigned capacity,unsigned batch){
        if(path.empty())udp_=std::make_unique<UdpReceiver>(std::move(ep),std::move(factory),capacity,batch);
        else file_=std::make_unique<MemoryVideo>(path,fps,loop,std::move(factory),capacity);
    }
    void observe(std::function<void(const std::shared_ptr<Frame>&)> callback){if(file_)file_->latest.observe(std::move(callback));else udp_->latest.observe(std::move(callback));}
    bool local()const{return bool(file_);}
    void pause(bool value,bool image_test=false,uint32_t source=0,double received=0){if(file_)file_->pause(value,image_test,source,received);}
    void start(){if(file_)file_->start();else udp_->start();}
    void stop(){if(file_)file_->stop();else udp_->stop();}
    Stats stats()const{return file_?file_->stats():udp_->stats();}
    std::string error()const{return file_?file_->error():udp_->error();}
    std::shared_ptr<Frame> take(){return file_?file_->latest.take():udp_->latest.take();}
    uint64_t replaced(){return file_?file_->latest.replaced():udp_->latest.replaced();}
    void write_summary(std::ostream& out,unsigned channel)const{
        if(file_)file_->write_summary(out,channel);
        else out<<channel<<",fpga_udp,0,0,"<<udp_->stats().complete<<",0,0,0,0,0\n";
    }
};
}
