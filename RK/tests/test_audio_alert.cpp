#include "audio_alert.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>
int main(){
    using namespace audio_alert;Gate gate;
    assert(!gate.update(false,1));assert(gate.update(true,1.1));
    for(int i=0;i<1000;++i)assert(!gate.update(true,1.2+i*.01));
    assert(!gate.update(false,12));assert(gate.update(true,12.1));assert(!gate.update(false,12.2));assert(!gate.update(true,12.3));
    assert(!gate.update(false,15));assert(gate.update(true,15.1));
    auto root=std::filesystem::temp_directory_path()/("cue_"+std::to_string(getpid()));assert(std::filesystem::create_directory(root));
    write_wav((root/"test.wav").string());assert(std::filesystem::file_size(root/"test.wav")==7724);
    std::ifstream f(root/"test.wav",std::ios::binary);char header[44];f.read(header,44);assert(std::string(header,4)=="RIFF"&&std::string(header+8,4)=="WAVE");
    {Cue disabled(root.string(),false);disabled.observe(true,1);assert(disabled.ui_state()==0);}
    {Cue failed(root.string(),true,"/bin/false");failed.observe(true,1);
        for(int i=0;i<100&&failed.ui_state()!=2;++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(failed.ui_state()==2);}
    {
        auto switched=root/"switch";assert(std::filesystem::create_directory(switched));
        Cue cue(switched.string(),false,"/bin/true");assert(cue.ui_state()==0);
        auto lines=[&](){std::ifstream log(switched/"audio_alert.csv");std::string row;int n=0;while(std::getline(log,row))++n;return n;};
        auto wait_for=[&](int expected){for(int i=0;i<100&&lines()<expected;++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(lines()==expected);};
        cue.toggle();assert(cue.ui_state()==1);cue.observe(true,1);wait_for(1);
        cue.toggle();assert(cue.ui_state()==0);cue.observe(false,4);cue.observe(true,4.1);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));assert(lines()==1);
        cue.toggle();assert(cue.ui_state()==1);cue.observe(true,5);wait_for(2);
        for(int i=0;i<100;++i)cue.observe(true,5.1+i*.01);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));assert(lines()==2);
    }
    std::cout<<"PASS two-beep WAV, violation edges/cooldown/hold, disabled output and nonblocking failed-player handling; no audible-device validation\n";
}
