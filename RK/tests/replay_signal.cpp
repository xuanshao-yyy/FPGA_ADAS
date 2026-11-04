#include "signal_control.hpp"
#include <filesystem>
#include <iostream>
#include <cassert>
int main(int argc,char**argv){try{
    if(argc!=5)throw std::runtime_error("replay_signal CONFIG RGB565 OUTPUT red|green");
    auto cfg=signal_demo::load(argv[1]);
    if(!std::filesystem::create_directory(argv[3]))throw std::runtime_error("Output exists; preserve old replay");
    signal_demo::Control control(cfg,argv[3]);
    std::ifstream input(argv[2],std::ios::binary);if(!input)throw std::runtime_error("Missing replay file");
    std::ofstream steps(std::string(argv[3])+"/steps.csv");
    steps<<"index,time_s,direction,light,requested,allowed,alarm,state,age_ms\n";
    std::vector<uint8_t> raw(qv::FrameBytes);
    unsigned frames=0,reds=0,greens=0,allowed=0,alarms=0,unknown_blocks=0;
    bool red=std::string(argv[4])=="red";
    while(input.read(reinterpret_cast<char*>(raw.data()),raw.size())){
        double now=100+frames/30.;
        control.observe(raw.data(),frames+1,frames+1,now,now,true);
        if(frames==0)control.key('w',now);
        if(red){
            if(frames==15||frames==48||frames==81)control.key(' ',now);
            if(frames==30||frames==66||frames==93)control.key('s',now);
            if(frames==39)control.key('a',now);
            if(frames==72)control.key('d',now);
        }else{
            if(frames==18||frames==57||frames==78)control.key(' ',now);
            if(frames==39||frames==69)control.key('s',now);
            if(frames==48)control.key('a',now);
            if(frames==72)control.key('w',now);
        }
        auto d=control.tick(now);
        steps<<frames<<','<<std::fixed<<std::setprecision(6)<<frames/30.<<','<<signal_demo::name(d.direction)<<','<<signal_demo::name(d.light)<<','<<d.requested<<','<<d.allowed<<','<<d.alarm<<','<<d.state<<','<<d.age_ms<<'\n';
        reds+=d.light==signal_demo::Color::Red;greens+=d.light==signal_demo::Color::Green;
        allowed+=d.allowed;alarms+=d.alarm;unknown_blocks+=std::string(d.state)=="BLOCKED_UNKNOWN";
        // Exercise actual NV12 HUD drawing with memory guards and original ROI scaling.
        if(frames==20||frames==50||frames==84){
            constexpr size_t bytes=1280*720*3/2;
            std::vector<uint8_t> guarded(bytes+64,77);std::fill(guarded.begin()+32,guarded.end()-32,128);
            control.draw(guarded.data()+32,0,now);
            assert(std::all_of(guarded.begin(),guarded.begin()+32,[](uint8_t v){return v==77;}));
            assert(std::all_of(guarded.end()-32,guarded.end(),[](uint8_t v){return v==77;}));
        }
        ++frames;
    }
    if(input.gcount())throw std::runtime_error("Partial RGB565 frame");
    assert(frames==cfg.loop_frames && frames>0 && unknown_blocks>0);
    if(red)assert(reds>40 && alarms>0 && allowed==0);
    else assert(greens>30 && allowed>0 && alarms==0);
    std::cout<<"PASS actual RGB565 color/FSM replay frames="<<frames<<" red="<<reds<<" green="<<greens
             <<" allowed="<<allowed<<" red_alarm="<<alarms<<" unknown_blocks="<<unknown_blocks<<". No NPU/FPS/automatic association claim.\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"ERROR "<<e.what()<<'\n';return 1;}}
