#pragma once
#include "rk_dma.hpp"
#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace product {
// One queued immutable frame; no JPEG encoding or per-frame disk writes.
class EvidenceWriter {
    struct Job {std::string id;std::shared_ptr<qv::Frame> frame;};
    std::string directory,error_;std::mutex mutex;std::condition_variable wake;Job pending;
    bool stopping=false;std::thread worker;
    void run(){
        for(;;){Job job;{std::unique_lock<std::mutex> lock(mutex);wake.wait(lock,[&]{return stopping||bool(pending.frame);});
            if(!pending.frame&&stopping)return;
            job=std::move(pending);pending={};}
            try{
                if(std::filesystem::space(directory).available<512ull*1024*1024)throw std::runtime_error("Evidence skipped: free space below 512 MiB");
                auto memory=std::dynamic_pointer_cast<qv::DmaBuffer>(job.frame->memory);if(memory)memory->begin_read();
                auto path=directory+"/"+job.id+".rgb565";
                std::ofstream file(path+".tmp",std::ios::binary);file.write(reinterpret_cast<const char*>(job.frame->pixels.get()),qv::FrameBytes);file.close();
                if(memory)memory->finish_read();
                if(!file)throw std::runtime_error("Evidence write failed");
                std::filesystem::rename(path+".tmp",path);
                std::vector<std::filesystem::directory_entry> images;
                for(const auto& p:std::filesystem::directory_iterator(directory))if(p.is_regular_file()&&p.path().extension()==".rgb565"&&p.path().stem().string().find_first_not_of("0123456789-")==std::string::npos)images.push_back(p);
                std::sort(images.begin(),images.end(),[](const auto& a,const auto& b){return a.last_write_time()<b.last_write_time();});
                for(size_t i=0;i+32<images.size();++i)std::filesystem::remove(images[i].path());
                std::lock_guard<std::mutex> lock(mutex);error_.clear();
            }catch(const std::exception& e){std::lock_guard<std::mutex> lock(mutex);error_=e.what();}
        }
    }
public:
    explicit EvidenceWriter(std::string root):directory(std::move(root)){
        if(!directory.empty()){std::filesystem::create_directories(directory);worker=std::thread([this]{run();});}
    }
    ~EvidenceWriter(){{std::lock_guard<std::mutex> lock(mutex);stopping=true;}wake.notify_one();if(worker.joinable())worker.join();}
    bool submit(std::string id,std::shared_ptr<qv::Frame> frame){std::lock_guard<std::mutex> lock(mutex);
        if(!worker.joinable()||stopping||pending.frame||!frame||id.empty()||id.size()>70||id.find_first_not_of("0123456789-")!=std::string::npos)return false;
        pending={std::move(id),std::move(frame)};wake.notify_one();return true;}
    std::string error(){std::lock_guard<std::mutex> lock(mutex);return error_;}
};
}
