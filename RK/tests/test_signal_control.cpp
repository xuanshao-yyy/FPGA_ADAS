#include "signal_control.hpp"
#include <filesystem>
#include <cassert>
#include <unistd.h>
int main(){
    namespace fs=std::filesystem;
    auto root=fs::temp_directory_path()/("signal_control_test_"+std::to_string(getpid()));
    assert(fs::create_directory(root));
    auto red=signal_demo::load("configs/red_arrow.json");
    assert(red.rois[0].enabled && red.rois[1].enabled && !red.rois[2].enabled);
    assert(red.red_high==160 && red.min_v==120);
    auto cfg=signal_demo::load("configs/live_manual.json");cfg.loop_frames=10;
    signal_demo::Control control(cfg,root.string());
    std::vector<uint8_t> raw(qv::FrameBytes,0);
    for(int y=110;y<123;++y)for(int x=110;x<123;++x){size_t at=(y*1280+x)*2;raw[at]=0xe0;raw[at+1]=0x07;}
    control.set_roi(100,100,140,140,500);
    for(int i=0;i<8;++i)control.observe(raw.data(),i+1,i+1,500+i/30.,500+i/30.,true);
    assert(control.tick(500+7/30.).light==signal_demo::Color::Green);
    control.key(' ',500+7/30.);assert(control.tick(500+7/30.).allowed);
    control.focus_lost(500+7/30.);assert(control.tick(500+7/30.).requested);
    control.key(' ',500+7/30.);
    control.observe(raw.data(),11,11,500+10/30.,500+10/30.,true);
    auto loop=control.tick(500+10/30.);assert(!loop.requested && loop.light==signal_demo::Color::Unknown);
    control.key('c',500+10/30.);control.key(' ',500+10/30.);
    assert(!control.tick(500+10/30.).allowed);
    assert(control.tick(502).light==signal_demo::Color::Unknown);
    // An out-of-frame drag is rejected rather than binding unrelated pixels.
    control.set_roi(-1,0,300,300,502);
    std::ofstream bad(root/"bad.json");bad<<"{\"scene\":\"bad\",\"width\":1280,\"height\":720,\"loop_frames\":0,\"rois\":{\"LEFT\":[-1,0,100,100],\"STRAIGHT\":[],\"RIGHT\":[]}}";bad.close();
    bool rejected=false;try{signal_demo::load((root/"bad.json").string());}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    assert(std::string(control.permission(signal_demo::Direction::Straight,502))=="WAIT");
    control.set_still_image(true,503);assert(!control.tick(503).requested);
    control.key('d',503);assert(control.selected()==signal_demo::Direction::Right);
    control.set_still_image(false,504);assert(!control.tick(504).requested);
    assert(fs::file_size(root/"signal_permissions.csv")>0);
    std::cout<<"PASS manual ROI config/range validation, actual RGB565 green, UI focus does not change traffic state, file-loop reset, clear binding and expiry.\n";
}
