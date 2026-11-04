#include "test_helpers.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <numeric>
#include <random>
#include <thread>
#include "udp.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
static std::vector<uint8_t> jumbo(uint32_t fid,unsigned pid){
    std::vector<uint8_t> p(qv::MaxDatagram);
    std::memcpy(p.data(),"QV02",4);
    auto b16=[&](unsigned n,unsigned v){p[n]=uint8_t(v>>8);p[n+1]=uint8_t(v);};
    auto b32=[&](unsigned n,uint32_t v){p[n]=uint8_t(v>>24);p[n+1]=uint8_t(v>>16);p[n+2]=uint8_t(v>>8);p[n+3]=uint8_t(v);};
    b32(4,fid);b16(8,pid);b16(10,qv::JumboPackets);b16(12,1280);b16(14,720);
    b32(16,pid*qv::JumboChunk);b16(20,qv::JumboChunk);b16(22,1);b32(24,7);
    for(unsigned n=0;n<qv::JumboChunk;++n)p[qv::Header+n]=uint8_t((pid*qv::JumboChunk+n)*37+11);
    return p;
}
static void check_jumbo(const qv::Frame& f){
    for(unsigned n=0;n<qv::FrameBytes;++n)CHECK(f.pixels[n]==uint8_t(n*37+11));
}
int main(){try{
    // Parking composite cannot be mistaken for a raw camera stream, or vice versa.
    qv::Assembler raw_only,quad_only(12,{},true);
    auto q=jumbo(1,0);q[3]='3';
    CHECK(!raw_only.feed(q.data(),q.size(),1));CHECK(raw_only.stats.invalid==1);
    CHECK(!quad_only.feed(q.data(),q.size(),1));CHECK(quad_only.stats.packets==1);
    auto raw=jumbo(1,1);CHECK(!quad_only.feed(raw.data(),raw.size(),1));CHECK(quad_only.stats.invalid==1);
    std::shared_ptr<qv::Frame> composite;
    for(unsigned pid=1;pid<qv::JumboPackets;++pid){q=jumbo(1,pid);q[3]='3';composite=quad_only.feed(q.data(),q.size(),1);}
    CHECK(composite);check_jumbo(*composite);
    // Same frame byte count, explicitly different wire geometry and magic.
    qv::Assembler panorama(6,{},2);std::shared_ptr<qv::Frame> pano;
    for(unsigned n=0;n<qv::JumboPackets;++n){
        auto packet=jumbo(1,qv::JumboPackets-1-n);packet[3]='4';
        packet[12]=10;packet[13]=0;packet[14]=1;packet[15]=104;
        if(n==0){CHECK(!raw_only.feed(packet.data(),packet.size(),1));CHECK(!quad_only.feed(packet.data(),packet.size(),1));}
        pano=panorama.feed(packet.data(),packet.size(),1);
    }
    CHECK(pano&&panorama.stats.complete==1);check_jumbo(*pano);
    auto malformed=jumbo(2,0);malformed[3]='4';
    CHECK(!panorama.feed(malformed.data(),malformed.size(),1.1));CHECK(panorama.stats.invalid==1);
    auto missing=jumbo(3,0);missing[3]='4';missing[12]=10;missing[13]=0;missing[14]=1;missing[15]=104;
    CHECK(!panorama.feed(missing.data(),missing.size(),1.2));panorama.expire(1.6);CHECK(panorama.stats.missing==224);
    qv::Assembler a;
    std::vector<unsigned> ids(qv::JumboPackets);std::iota(ids.begin(),ids.end(),0);
    std::mt19937 rng(27);std::shuffle(ids.begin(),ids.end(),rng);
    auto p=jumbo(1,ids.front());CHECK(!a.feed(p.data(),p.size(),1));CHECK(!a.feed(p.data(),p.size(),1));
    std::shared_ptr<qv::Frame> f;
    for(size_t n=1;n<ids.size();++n){p=jumbo(1,ids[n]);f=a.feed(p.data(),p.size(),1);}
    CHECK(f && a.stats.complete==1 && a.stats.duplicates==1);check_jumbo(*f);
    p=jumbo(2,0);a.feed(p.data(),p.size(),1.1);
    auto old=packet(2,1);CHECK(!a.feed(old.data(),old.size(),1.1));CHECK(a.stats.invalid==1);
    a.expire(1.5);CHECK(a.stats.missing==224);
    p=jumbo(3,224);CHECK(!a.feed(p.data(),p.size(),1.6)); // last bytes are in bounds
    for(unsigned byte:{0u,8u,10u,12u,14u,16u,20u,22u}){
        auto bad=jumbo(3,0);bad[byte]^=0x80;CHECK(!a.feed(bad.data(),bad.size(),1.6));
    }
    CHECK(!a.feed(p.data(),p.size()-1,1.6));
    qv::Assembler modes;
    for(unsigned mode=0;mode<4;++mode){
        for(unsigned pid=0;pid<(mode%2?qv::JumboPackets:qv::Packets);++pid){
            if(mode%2){p=jumbo(mode+1,pid);f=modes.feed(p.data(),p.size(),2+mode*.1);}
            else {auto regular=packet(mode+1,pid);f=modes.feed(regular.data(),regular.size(),2+mode*.1);}
        }
        CHECK(f);if(mode%2)check_jumbo(*f);else check_pixels(*f);
    }
    qv::UdpReceiver receiver({"127.0.0.1","127.0.0.2",42572,30,"jumbo loopback"},{},12,300);
    receiver.start();auto s=socket(AF_INET,SOCK_DGRAM,0);
    sockaddr_in from{};from.sin_family=AF_INET;from.sin_port=htons(42572);inet_pton(AF_INET,"127.0.0.2",&from.sin_addr);
    CHECK(bind(s,reinterpret_cast<sockaddr*>(&from),sizeof(from))==0);
    sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons(42572);inet_pton(AF_INET,"127.0.0.1",&to.sin_addr);
    for(unsigned fid=1;fid<=2;++fid){
        for(unsigned pid=0;pid<qv::JumboPackets;++pid){
            p=jumbo(fid,qv::JumboPackets-1-pid);
            CHECK(sendto(s,reinterpret_cast<const char*>(p.data()),int(p.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof(to))==int(p.size()));
            std::this_thread::sleep_for(std::chrono::microseconds(120));
        }
        f.reset();for(unsigned n=0;n<200 && !f;++n){std::this_thread::sleep_for(std::chrono::milliseconds(5));f=receiver.latest.take();}
        CHECK(f && f->id==fid);check_jumbo(*f);
    }
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    receiver.stop();CHECK(receiver.error().empty());CHECK(receiver.stats().complete==2);CHECK(receiver.stats().invalid==0);
    std::cout<<"PASS: QV02 8192-byte payload, all pixel bytes, reorder, duplicate, malformed/mixed modes, loss count, R2 compatibility, actual jumbo UDP loopback\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
