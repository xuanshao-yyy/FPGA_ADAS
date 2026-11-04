#include "protocol.hpp"
#include "test_helpers.hpp"
#include <iostream>
struct Memory: qv::FrameMemory {
    std::vector<uint8_t> bytes=std::vector<uint8_t>(qv::FrameBytes);
    unsigned begins=0,ends=0;
    bool writing=false;
    uint8_t* data()const override{return const_cast<uint8_t*>(bytes.data());}
    void begin_write()override{++begins;writing=true;}
    void finish_write()override{CHECK(writing);writing=false;++ends;}
};
int main(){try{
    auto storage=std::make_shared<Memory>();
    auto factory=[storage](){return std::make_unique<qv::Frame>(storage);};
    std::shared_ptr<qv::Frame> held;
    {
        qv::Assembler assembler(1,factory);
        auto p=packet(1,0);CHECK(!assembler.feed(p.data(),p.size(),1));
        CHECK(storage->begins==1 && storage->ends==0 && storage->writing);
        for(unsigned i=1;i<qv::Packets;++i){p=packet(1,i);held=assembler.feed(p.data(),p.size(),1.01);}
        CHECK(held && storage->ends==1 && !storage->writing);check_pixels(*held);
        p=packet(2,0);CHECK(!assembler.feed(p.data(),p.size(),1.02));CHECK(assembler.stats.pool_exhausted==1);
        held.reset();
        p=packet(3,0);CHECK(!assembler.feed(p.data(),p.size(),1.03));CHECK(storage->begins==2);
        assembler.expire(2);CHECK(storage->ends==1); // incomplete frame never published to hardware
        for(unsigned i=0;i<qv::Packets;++i){p=packet(4,i);held=assembler.feed(p.data(),p.size(),2.01);}
        CHECK(held && storage->ends==2);check_pixels(*held);
    }
    CHECK(held && held->memory==storage);check_pixels(*held);held.reset();
    std::cout<<"PASS external storage: complete-frame sealing, incomplete discard, retained frame lifetime, bounded reuse\n";
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
