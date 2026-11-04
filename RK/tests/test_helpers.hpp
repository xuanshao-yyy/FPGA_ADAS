#pragma once
#include "protocol.hpp"
#include <array>
#include <stdexcept>
#include <string>
#define CHECK(x) do { if(!(x))throw std::runtime_error(std::string("CHECK failed: ")+ #x + " line " + std::to_string(__LINE__)); } while(0)
inline void put16(uint8_t* p,unsigned n){p[0]=uint8_t(n>>8);p[1]=uint8_t(n);}
inline void put32(uint8_t* p,uint32_t n){p[0]=uint8_t(n>>24);p[1]=uint8_t(n>>16);p[2]=uint8_t(n>>8);p[3]=uint8_t(n);}
inline uint16_t value_at(unsigned pixel,uint32_t fid){return uint16_t(pixel^fid^0x5a5a);}
inline std::array<uint8_t,qv::Datagram> packet(uint32_t fid,unsigned pid,uint32_t source=7) {
    std::array<uint8_t,qv::Datagram> p{};
    p[0]='Q';p[1]='V';p[2]='0';p[3]='2';put32(p.data()+4,fid);put16(p.data()+8,pid);
    put16(p.data()+10,qv::Packets);put16(p.data()+12,qv::Width);put16(p.data()+14,qv::Height);
    put32(p.data()+16,pid*qv::Chunk);put16(p.data()+20,qv::Chunk);put16(p.data()+22,1);put32(p.data()+24,source);
    for(unsigned i=0;i<qv::Chunk/2;++i){
        auto value=value_at(pid*qv::Chunk/2+i,fid);p[qv::Header+2*i]=uint8_t(value);p[qv::Header+2*i+1]=uint8_t(value>>8);
    }
    return p;
}
inline void check_pixels(const qv::Frame& f){
    for(unsigned i=0;i<qv::Width*qv::Height;++i)
        CHECK((unsigned(f.pixels[2*i])|(unsigned(f.pixels[2*i+1])<<8))==value_at(i,f.id));
}
