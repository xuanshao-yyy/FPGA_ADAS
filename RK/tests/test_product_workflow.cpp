#include "product_io.hpp"
#include <cassert>
#include <filesystem>
#include <thread>
#include <iostream>
int main(){
    namespace fs=std::filesystem;auto root=fs::temp_directory_path()/("workflow_"+std::to_string(getpid()));assert(fs::create_directory(root));
    {
        product::Bridge bridge(root.string(),"");
        auto command=[&](const char* key,double sent,int pid){cv::FileStorage f((root/"driver_command.json").string(),cv::FileStorage::WRITE|cv::FileStorage::FORMAT_JSON);f<<"id"<<"test-command"<<"key"<<key<<"pid"<<pid<<"sent_s"<<sent;};
        command("w",1,getpid());assert(bridge.take_command(1.1)=='w');assert(!fs::exists(root/"driver_ack.json"));bridge.ack_command(1.11);
        {cv::FileStorage f((root/"driver_ack.json").string(),cv::FileStorage::READ);int applied=0;f["applied"]>>applied;assert(applied==1);}
        command("q",1,getpid());assert(!bridge.take_command(1.1));
        command("w",1,getpid()+1);assert(!bridge.take_command(1.1));
        command("w",1,getpid());assert(!bridge.take_command(4));
        std::array<std::shared_ptr<qv::Frame>,4> frames{};frames[0]=std::make_shared<qv::Frame>();frames[0]->received=5;frames[0]->id=42;frames[0]->source=19;
        std::fill(frames[0]->pixels.get(),frames[0]->pixels.get()+qv::FrameBytes,0x7f);
        {std::ofstream f(root/"evidence.request");f<<"test";}
        driver_demo::Explanation e;e.vehicle_state="STOPPED";e.vehicle_label="手动停车";
        assert(bridge.tick(5.1,e,{0,0,0,0},frames));
        {cv::FileStorage f((root/"perception.json").string(),cv::FileStorage::READ);std::string value;f["vehicle_state"]>>value;assert(value=="STOPPED");double age=0;f["channels"][0]["rx_age_ms"]>>age;assert(age>99&&age<101);}
    }
    unsigned files=0;for(const auto& p:fs::directory_iterator(root/"evidence"))if(p.path().extension()==".rgb565"){++files;assert(fs::file_size(p)==qv::FrameBytes);std::ifstream f(p.path(),std::ios::binary);assert(f.get()==0x7f);}
    assert(files==1);assert(fs::exists(root/"events.jsonl"));
    std::cout<<"PASS command PID/age/allowlist/after-dispatch ACK and bounded asynchronous received-frame evidence\n";
}
