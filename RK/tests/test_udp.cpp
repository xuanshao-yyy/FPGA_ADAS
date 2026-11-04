#include "udp.hpp"
#include "test_helpers.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
int main(int argc,char** argv){try{
    qv::UdpReceiver r({"127.0.0.1","127.0.0.2",42571,30,"loopback"},{},12,argc>1?unsigned(std::stoul(argv[1])):0);r.start();
    auto s=socket(AF_INET,SOCK_DGRAM,0);
    sockaddr_in from{};from.sin_family=AF_INET;from.sin_port=htons(42571);inet_pton(AF_INET,"127.0.0.2",&from.sin_addr);
    CHECK(bind(s,reinterpret_cast<sockaddr*>(&from),sizeof(from))==0);
    sockaddr_in to{};to.sin_family=AF_INET;to.sin_port=htons(42571);inet_pton(AF_INET,"127.0.0.1",&to.sin_addr);
    auto invalid=packet(1,0);invalid[0]='X';
    CHECK(sendto(s,reinterpret_cast<const char*>(invalid.data()),int(invalid.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof(to))==int(invalid.size()));
    auto wrong=socket(AF_INET,SOCK_DGRAM,0);from.sin_port=0;
    CHECK(bind(wrong,reinterpret_cast<sockaddr*>(&from),sizeof(from))==0);
    auto foreign=packet(1,0);
    CHECK(sendto(wrong,reinterpret_cast<const char*>(foreign.data()),int(foreign.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof(to))==int(foreign.size()));
    for(unsigned id=1;id<=2;++id){
        for(unsigned i=0;i<qv::Packets;++i){
            auto p=packet(id,qv::Packets-1-i);
            CHECK(sendto(s,reinterpret_cast<const char*>(p.data()),int(p.size()),0,reinterpret_cast<sockaddr*>(&to),sizeof(to))==int(p.size()));
            // Coalescing is intended for the physical 1GbE link, not an
            // unbounded loopback burst that can fill the unprivileged rcvbuf.
            // Pace both receiver modes near the physical link budget. An
            // unbounded VM loopback burst can overflow its capped socket queue.
            if(i%16==15)std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        std::shared_ptr<qv::Frame> f;
        for(unsigned n=0;n<100 && !f;++n){std::this_thread::sleep_for(std::chrono::milliseconds(5));f=r.latest.take();}
        if(!f){auto st=r.stats();std::cerr<<"packets="<<st.packets<<" invalid="<<st.invalid<<" foreign="<<st.foreign<<" incomplete="<<st.incomplete<<" missing="<<st.missing<<" error="<<r.error()<<"\n";}
        CHECK(f && f->id==id);check_pixels(*f);
    }
#ifdef _WIN32
    closesocket(s);closesocket(wrong);
#else
    close(s);close(wrong);
#endif
    r.stop();CHECK(r.error().empty());CHECK(r.stats().complete==2);CHECK(r.stats().missing==0);
    CHECK(r.stats().invalid==1 && r.stats().foreign==1);
    std::cout<<"PASS: real UDP loopback, rejects wrong peer port / malformed header, two full frames, 1843200 pixels\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
