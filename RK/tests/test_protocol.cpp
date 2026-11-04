#include "test_helpers.hpp"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <random>
int main(){try{
    qv::Assembler a;
    std::vector<unsigned> order(qv::Packets);std::iota(order.begin(),order.end(),0);
    std::mt19937 rng(17);std::shuffle(order.begin(),order.end(),rng);
    auto first=packet(42,order[0]);CHECK(!a.feed(first.data(),first.size(),1));CHECK(!a.feed(first.data(),first.size(),1));
    for(size_t i=1;i<order.size()-1;++i){auto p=packet(42,order[i]);CHECK(!a.feed(p.data(),p.size(),1));}
    auto final=packet(42,order.back());auto result=a.feed(final.data(),final.size(),1);
    CHECK(result && result->id==42);check_pixels(*result);CHECK(a.stats.duplicates==1);
    CHECK(!a.feed(final.data(),final.size(),1.1));CHECK(a.stats.duplicates==2);
    auto mismatch=packet(43,0);CHECK(!a.feed(mismatch.data(),mismatch.size(),1.1));
    mismatch=packet(43,1,8);CHECK(!a.feed(mismatch.data(),mismatch.size(),1.1));CHECK(a.stats.invalid==1);
    a.expire(1.5);CHECK(a.stats.incomplete==1 && a.stats.missing==1279);
    for(unsigned i=0;i<28;++i){auto p=packet(50,0);CHECK(!a.feed(p.data(),i,1.5));}
    for(unsigned byte:{0u,8u,10u,12u,14u,16u,20u,22u}){
        auto p=packet(50,0);p[byte]^=0x80;CHECK(!a.feed(p.data(),p.size(),1.5));
    }
    qv::Assembler wrap;
    double t=2;
    for(uint32_t fid:{0xffffffffu,0u,100u,1u}){
        t+=fid==1?1.2:0.1;std::shared_ptr<qv::Frame> f;
        for(unsigned i=0;i<qv::Packets;++i){auto p=packet(fid,i);f=wrap.feed(p.data(),p.size(),t);}
        CHECK(f && f->id==fid);check_pixels(*f);
    }
    qv::Assembler bounded;
    for(unsigned i=0;i<4;++i){auto p=packet(i,0);bounded.feed(p.data(),p.size(),1);}
    CHECK(bounded.stats.incomplete==1);bounded.expire(2);CHECK(bounded.stats.incomplete==4);
    // Completed newer frame cannot be replaced by a delayed older frame.
    qv::Assembler stale;
    auto p=packet(5,0);stale.feed(p.data(),p.size(),1);
    for(unsigned i=0;i<qv::Packets;++i){p=packet(6,i);stale.feed(p.data(),p.size(),1.1);}
    p=packet(5,1);CHECK(!stale.feed(p.data(),p.size(),1.2));CHECK(stale.stats.old==1);
    qv::FramePool pool(1);auto lease=pool.acquire();CHECK(lease && !pool.acquire());lease.reset();CHECK(pool.acquire());
    qv::Latest latest;latest.put(result);latest.put(result);CHECK(latest.replaced()==1);CHECK(latest.take()==result);CHECK(!latest.take());
    std::cout<<"PASS: full RGB565 pixels, reorder, duplicate, bad headers, timeout, epoch reset, wrap, bounded pool, latest-frame policy\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
