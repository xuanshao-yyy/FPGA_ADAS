#include "input_source.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
#include <sstream>
#include "test_helpers.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
int main(){
    auto dir=std::filesystem::temp_directory_path()/("local_source_"+std::to_string(getpid()));
    std::filesystem::create_directory(dir);auto path=(dir/"clip.rgb565").string();
    {std::ofstream f(path,std::ios::binary);std::vector<char> frame(qv::FrameBytes);for(int i=0;i<3;++i){std::fill(frame.begin(),frame.end(),char(10+i));f.write(frame.data(),frame.size());}}
    assert(qv::MemoryVideo::validated_size(path)==3*qv::FrameBytes);
    for(bool mapped:{false,true})for(bool loop:{false,true}){
        qv::MemoryVideo source(path,30,loop,{},2,mapped?0:512ull*1024*1024);source.start();uint32_t previous=0;unsigned seen=0;
        const auto end=qv::now_seconds()+.23;
    while(qv::now_seconds()<end){auto f=source.latest.take();if(f){assert(f->id>previous);assert(f->source==f->id);assert(f->received<=qv::now_seconds());assert(f->pixels[0]==10+(f->id-1)%3);assert(f->pixels[qv::FrameBytes-1]==f->pixels[0]);previous=f->id;++seen;}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        source.stop();assert(source.error().empty());assert(seen>=(loop?4u:3u));assert(loop || source.stats().complete==3);
        std::ostringstream record;source.write_summary(record,0);assert(record.str().find(mapped?"0,mapped_rgb565,":"0,preloaded_rgb565,")==0);
    }
    {qv::MemoryVideo source(path,30,true);source.start();
     std::this_thread::sleep_for(std::chrono::milliseconds(60));source.pause(true);
     auto before=source.stats().complete;auto last=source.latest.take();assert(last);auto id=last->id;last.reset();
     std::this_thread::sleep_for(std::chrono::milliseconds(160));assert(source.stats().complete==before);assert(!source.latest.take());
     source.pause(false);source.pause(false);
     std::shared_ptr<qv::Frame> resumed;const auto deadline=qv::now_seconds()+.12;
     while(!resumed&&qv::now_seconds()<deadline){resumed=source.latest.take();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
     assert(resumed&&resumed->id==id+1);assert(resumed->received>qv::now_seconds()-.12);
     source.pause(true);source.stop();assert(source.error().empty());}
    {std::atomic<unsigned> notified{0};qv::MemoryVideo source(path,30,true);
     source.latest.observe([&](const std::shared_ptr<qv::Frame>& frame){assert(frame&&frame->source==frame->id);assert(frame->pixels[0]==10+(frame->id-1)%3);++notified;});
     source.start();std::this_thread::sleep_for(std::chrono::milliseconds(200));
     source.pause(true);auto before=notified.load();assert(before>=4);assert(source.latest.replaced()>=3);
     std::this_thread::sleep_for(std::chrono::milliseconds(120));assert(notified==before);source.stop();
     assert(notified==source.stats().complete);assert(source.latest.take());}
    {qv::MemoryVideo source(path,30,true,{},8,0);source.start();std::shared_ptr<qv::Frame> selected;
     while(!selected) {selected=source.latest.take();std::this_thread::sleep_for(std::chrono::milliseconds(1));}
     const auto frozen=selected->source;const auto captured=selected->received;const auto pixel=selected->pixels[0];
     source.pause(true,true,frozen,captured);source.latest.take();auto unique=source.stats().source_updates;
     uint32_t id=selected->id;unsigned repeats=0;auto end=qv::now_seconds()+.18;
     while(qv::now_seconds()<end){auto f=source.latest.take();if(f){assert(f->still_image_test&&f->source==frozen&&f->content_received==captured&&f->id>id&&f->pixels[0]==pixel);id=f->id;++repeats;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}
     assert(repeats>=3&&source.stats().source_updates==unique&&source.stats().still_repeats>=repeats);
     source.pause(false);std::shared_ptr<qv::Frame> resumed;end=qv::now_seconds()+.12;
     while(!resumed&&qv::now_seconds()<end){auto f=source.latest.take();if(f&&!f->still_image_test)resumed=f;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
     assert(resumed&&resumed->source==frozen+1&&resumed->id>id&&resumed->content_received>captured);source.stop();}
    std::shared_ptr<qv::Frame> held;
    {qv::MemoryVideo source(path,30,true,{},1);source.start();while(!(held=source.latest.take()))std::this_thread::sleep_for(std::chrono::milliseconds(1));
     std::this_thread::sleep_for(std::chrono::milliseconds(100));source.stop();assert(source.stats().pool_exhausted>0);}
    assert(held->pixels[0]==10);held.reset();
    bool bad=false;try{qv::MemoryVideo source(path,0,true);}catch(const std::exception&){bad=true;}assert(bad);
    {std::ofstream f(dir/"invalid.rgb565");f<<"bad";}
    bad=false;try{qv::MemoryVideo::validated_size((dir/"invalid.rgb565").string());}catch(const std::exception&){bad=true;}assert(bad);
    // The file endpoint is not bound, so real FPGA traffic on that port cannot
    // overwrite local CH0. Other InputSource instances still use UdpReceiver.
    qv::InputSource mixed({"not_an_ip","not_an_ip",5000,30,"CH0"},path,30,true,{},2,300);
    mixed.start();std::this_thread::sleep_for(std::chrono::milliseconds(75));mixed.stop();assert(mixed.error().empty());assert(mixed.stats().complete>=2);
    std::ostringstream record;mixed.write_summary(record,0);assert(record.str().find("0,preloaded_rgb565,30,")==0);
    // Local CH0 alongside a real UDP input exercises both branches together.
    qv::InputSource local({"invalid","invalid",42573,30,"CH0"},path,30,true,{},2,300);
    qv::InputSource udp({"127.0.0.1","127.0.0.2",42573,30,"CH1"},"",30,true,{},8,300);
    local.start();udp.start();auto socket_fd=socket(AF_INET,SOCK_DGRAM,0);assert(socket_fd>=0);
    sockaddr_in from{};from.sin_family=AF_INET;from.sin_port=htons(42573);inet_pton(AF_INET,"127.0.0.2",&from.sin_addr);
    assert(bind(socket_fd,reinterpret_cast<sockaddr*>(&from),sizeof(from))==0);
    sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons(42573);inet_pton(AF_INET,"127.0.0.1",&to.sin_addr);
    for(unsigned pid=0;pid<qv::Packets;++pid){auto data=packet(91,pid);
        assert(sendto(socket_fd,data.data(),data.size(),0,reinterpret_cast<sockaddr*>(&to),sizeof(to))==ssize_t(data.size()));
        if(pid%16==15)std::this_thread::sleep_for(std::chrono::microseconds(200));}
    std::shared_ptr<qv::Frame> received;
    for(unsigned n=0;n<200&&!received;++n){received=udp.take();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    assert(received&&received->id==91);check_pixels(*received);assert(local.take());
    udp.stop();local.stop();close(socket_fd);assert(udp.error().empty()&&local.error().empty());
    assert(udp.stats().complete==1&&udp.stats().missing==0);assert(local.stats().complete>0);
    std::array<std::unique_ptr<qv::MemoryVideo>,4> four;
    for(auto& source:four){source=std::make_unique<qv::MemoryVideo>(path,30,true);source->start();}
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    for(auto& source:four){source->stop();assert(source->latest.take());assert(source->stats().complete>=2);assert(source->error().empty());}
    std::filesystem::remove_all(dir);std::cout<<"PASS local frame colors, clock, loop, EOF, ownership, pool pressure and mixed source selection\n";
}
