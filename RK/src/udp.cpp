#include "udp.hpp"
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace qv {
namespace {
#ifdef _WIN32
struct Winsock {
    Winsock(){WSADATA data;if(WSAStartup(MAKEWORD(2,2),&data))throw std::runtime_error("WSAStartup failed");}
    ~Winsock(){WSACleanup();}
};
void close_socket(intptr_t s){closesocket(static_cast<SOCKET>(s));}
std::string socket_error(){return "WSA error "+std::to_string(WSAGetLastError());}
#else
void close_socket(intptr_t s){close(static_cast<int>(s));}
std::string socket_error(){return std::strerror(errno);}
#endif
}
UdpReceiver::UdpReceiver(Endpoint ep,FrameFactory factory,unsigned capacity,unsigned coalesce_us,unsigned composite)
    :ep_(std::move(ep)),factory_(std::move(factory)),capacity_(capacity),coalesce_us_(coalesce_us),composite_(composite){}
UdpReceiver::~UdpReceiver(){stop();}
void UdpReceiver::start() {
#ifdef _WIN32
    static Winsock init;
#endif
    if(thread_.joinable())throw std::runtime_error("receiver already started");
    socket_=static_cast<intptr_t>(::socket(AF_INET,SOCK_DGRAM,0));
    if(socket_==-1)throw std::runtime_error("socket: "+socket_error());
    try {
        int bytes=8*1024*1024;
        // Per-socket only. No sysctl or network configuration changes.
#ifdef SO_RCVBUFFORCE
        if(setsockopt(socket_,SOL_SOCKET,SO_RCVBUFFORCE,&bytes,sizeof(bytes))!=0)
#endif
        setsockopt(socket_,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&bytes),sizeof(bytes));
        socklen_t len=sizeof(receive_buffer_);
        getsockopt(socket_,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&receive_buffer_),&len);
        sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(ep_.port);
        if(inet_pton(AF_INET,ep_.bind.c_str(),&addr.sin_addr)!=1)
            throw std::runtime_error("Invalid bind IPv4: "+ep_.bind);
        if(::bind(socket_,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0)
            throw std::runtime_error(ep_.bind+":"+std::to_string(ep_.port)+" bind: "+socket_error());
#ifdef SO_RXQ_OVFL
        int enabled=1;setsockopt(socket_,SOL_SOCKET,SO_RXQ_OVFL,&enabled,sizeof(enabled));
#endif
        stopping_=false;thread_=std::thread(&UdpReceiver::run,this);
    } catch(...) {close_socket(socket_);socket_=-1;throw;}
}
void UdpReceiver::stop() {
    stopping_=true;
    if(thread_.joinable())thread_.join();
    if(socket_!=-1){close_socket(socket_);socket_=-1;}
}
Stats UdpReceiver::stats() const {std::lock_guard<std::mutex> lock(mutex_);return published_;}
std::string UdpReceiver::error() const {std::lock_guard<std::mutex> lock(mutex_);return error_;}
void UdpReceiver::run() {
    try {
        Assembler assembler(capacity_,factory_,composite_);
        in_addr peer{};
        if(inet_pton(AF_INET,ep_.peer.c_str(),&peer)!=1)throw std::runtime_error("Invalid peer IPv4");
        auto consume=[&](const uint8_t* data,size_t size,const sockaddr_in& addr,double now){
            if(addr.sin_addr.s_addr!=peer.s_addr || ntohs(addr.sin_port)!=ep_.port){
                ++assembler.stats.foreign;return;
            }
            auto frame=assembler.feed(data,size,now);
            if(frame)latest.put(std::move(frame));
        };
#ifdef __linux__
        constexpr unsigned Batch=32;
        std::array<std::array<uint8_t,MaxDatagram>,Batch> data{};
        std::array<sockaddr_in,Batch> from{};
        std::array<iovec,Batch> iov{};
        std::array<mmsghdr,Batch> messages{};
        struct Control {alignas(cmsghdr) char data[CMSG_SPACE(sizeof(uint32_t))];};
        std::array<Control,Batch> ancillary{};
        uint32_t previous_drops=0;
        bool draining=false;
        for(unsigned i=0;i<Batch;++i){iov[i]={data[i].data(),data[i].size()};}
        while(!stopping_) {
            pollfd poller{static_cast<int>(socket_),POLLIN,0};
            const int ready=poll(&poller,1,50);
            if(ready<0 && errno!=EINTR)throw std::runtime_error("poll: "+socket_error());
            if(ready>0 && (poller.revents&(POLLERR|POLLNVAL)))throw std::runtime_error("UDP poll error");
            if(ready>0 && (poller.revents&POLLIN)) {
                // Optional short coalescing for the low-CPU viewer. Not a busy
                // wait; trades sub-millisecond latency for fewer recv syscalls.
                if(coalesce_us_ && !draining)std::this_thread::sleep_for(std::chrono::microseconds(coalesce_us_));
                for(unsigned i=0;i<Batch;++i){
                    auto& h=messages[i].msg_hdr;h={};
                    h.msg_name=&from[i];h.msg_namelen=sizeof(from[i]);h.msg_iov=&iov[i];h.msg_iovlen=1;
                    h.msg_control=ancillary[i].data;h.msg_controllen=sizeof(ancillary[i].data);
                }
                const int count=recvmmsg(socket_,messages.data(),Batch,MSG_DONTWAIT,nullptr);
                draining=(count==int(Batch));
                if(count<0 && errno!=EAGAIN && errno!=EINTR)throw std::runtime_error("recvmmsg: "+socket_error());
                const double now=now_seconds();
                if(count>0)++assembler.stats.rx_batches;
                for(int i=0;i<count;++i){
                    auto& h=messages[i].msg_hdr;
#ifdef SO_RXQ_OVFL
                    for(auto* c=CMSG_FIRSTHDR(&h);c;c=CMSG_NXTHDR(&h,c)) {
                        if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SO_RXQ_OVFL && c->cmsg_len>=CMSG_LEN(sizeof(uint32_t))){
                            uint32_t drops;std::memcpy(&drops,CMSG_DATA(c),sizeof(drops));
                            assembler.stats.kernel_drops+=uint32_t(drops-previous_drops);previous_drops=drops;
                        }
                    }
#endif
                    if(h.msg_flags&MSG_TRUNC){++assembler.stats.invalid;continue;}
                    consume(data[i].data(),messages[i].msg_len,from[i],now);
                }
            }
            assembler.expire(now_seconds());
            {std::lock_guard<std::mutex> lock(mutex_);published_=assembler.stats;}
        }
#else
        // Portable loopback validation path; production Linux uses recvmmsg above.
        std::array<uint8_t,MaxDatagram> data{};
        while(!stopping_) {
            fd_set set;FD_ZERO(&set);FD_SET(socket_,&set);timeval timeout{0,50000};
            const int ready=select(static_cast<int>(socket_)+1,&set,nullptr,nullptr,&timeout);
            if(ready<0)throw std::runtime_error("select: "+socket_error());
            if(ready>0){
                sockaddr_in from{};socklen_t len=sizeof(from);
                const int n=recvfrom(socket_,reinterpret_cast<char*>(data.data()),static_cast<int>(data.size()),0,reinterpret_cast<sockaddr*>(&from),&len);
                if(n<0)throw std::runtime_error("recvfrom: "+socket_error());
                consume(data.data(),static_cast<size_t>(n),from,now_seconds());
            }
            assembler.expire(now_seconds());
            {std::lock_guard<std::mutex> lock(mutex_);published_=assembler.stats;}
        }
#endif
    } catch(const std::exception& e){std::lock_guard<std::mutex> lock(mutex_);error_=e.what();}
}
}
