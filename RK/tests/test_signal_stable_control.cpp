#include "signal_control.hpp"
#include <filesystem>
#include <cassert>
#include <unistd.h>
int main(){
    namespace fs=std::filesystem;using namespace signal_demo;
    auto root=fs::temp_directory_path()/("signal_stable_"+std::to_string(getpid()));assert(fs::create_directory(root));
    auto cfg=load("configs/auto_red.json");cfg.automatic=true;cfg.practical_association=false;cfg.settings.stop_hold_ms=1500;cfg.box_hold_ms=500;
    Control control(cfg,root.string());unsigned source=0;
    ai::SignalCandidate lamp;lamp.box={500,100,580,180,.2f,7};lamp.track_id=10;lamp.direction=1;lamp.color="RED";lamp.shape="STRAIGHT";
    auto input=[&](double t,std::vector<ai::SignalCandidate> candidates){ai::Result result;result.frame_id=result.source=++source;result.received=t;result.finished=t+.05;result.signal_candidates=std::move(candidates);
        control.observe(nullptr,result.frame_id,result.source,t,t,true);control.observe_automatic(result,t+.05);return control.tick(t+.05);};
    for(int i=0;i<4;++i)input(100+i*.1,{lamp});
    auto displayed=control.explanation(100.35);assert(displayed.signal[1]=="信号禁止"&&displayed.signal_light[1]=="RED");
    auto d=input(100.4,{});assert(d.stop_held&&d.light==Color::Unknown&&d.held_light==Color::Red&&!d.allowed);
    displayed=control.explanation(100.45);assert(displayed.signal[1]=="信号禁止"&&displayed.signal_light[1]=="RED");
    assert(control.toolbar_detail(100.45).find("SIGNAL RED")!=std::string::npos);
    assert(displayed.reason=="当前意图方向红灯");
    std::vector<uint8_t> image(1280*720*3/2,16);control.draw(image.data(),0,100.45);
    const int x=32+int(500*.45f),y=32+int(100*.45f);assert(image[y*1280+x]==210); // dashed CACHED border
    std::fill(image.begin(),image.end(),16);control.draw(image.data(),0,100.85);assert(image[y*1280+x]==16);assert(control.tick(100.85).stop_held);
    // A cached RED rectangle is never counted as a new RED observation.
    assert(!control.tick(101.81).stop_held);
    assert(control.explanation(101.81).signal_light[1]=="UNKNOWN");
    lamp.color="GREEN";for(int i=0;i<3;++i)input(102+i*.1,{lamp});control.key(' ',102.25);assert(control.tick(102.25).allowed);
    displayed=control.explanation(102.25);assert(displayed.signal[1]=="信号允许"&&displayed.signal_light[1]=="GREEN");
    d=input(102.3,{});assert(!d.allowed&&!d.stop_held);
    lamp.color="RED";for(int i=0;i<3;++i)input(103+i*.1,{lamp});assert(input(103.3,{}).stop_held);
    lamp.track_id=22;assert(!input(103.4,{lamp}).stop_held); // new signal cannot inherit old stop evidence
    // Duplicate same-track candidates with contradictory colors are rejected.
    auto other=lamp;other.color="GREEN";for(int i=0;i<4;++i){d=input(103.5+i*.1,{lamp,other});assert(d.light==Color::Unknown&&!d.allowed&&!d.stop_held);}
    for(int i=0;i<3;++i)input(104+i*.1,{lamp});assert(input(104.3,{}).stop_held);
    control.key('c',104.35);assert(!control.tick(104.35).stop_held);
    for(int i=0;i<3;++i)input(105+i*.1,{lamp});
    control.observe(nullptr,107,107,105.4,105.4,true);assert(!control.tick(105.4).stop_held);
    std::vector<uint8_t> guarded(1280*720*3/2+64,77);control.draw(guarded.data()+32,0,105.4);
    for(int i=0;i<32;++i)assert(guarded[i]==77&&guarded[guarded.size()-1-i]==77);
    // View-only Kalman boxes change render coordinates, never the recorded ROI/color.
    auto shifted=lamp;shifted.track_id=77;shifted.box={300,100,380,180,.3f,7};input(110,{shifted});
    shifted.box.x1=310;shifted.box.x2=390;input(110.1,{shifted});
    assert(control.toolbar_status(110.15).find("SIM_GO")==std::string::npos);
    std::fill(image.begin(),image.end(),16);control.draw(image.data(),0,110.15);
    int first=-1;for(int xx=150;xx<210;++xx)if(image[(32+45)*1280+xx]==81){first=xx;break;}
    assert(first>=32+int(300*.45f)&&first<=32+int(310*.45f));
    std::ifstream recorded(root/"signal_auto_association.csv");std::string csv((std::istreambuf_iterator<char>(recorded)),{});assert(csv.find("310.000000000")!=std::string::npos);
    control.key('c',110.16);std::fill(image.begin(),image.end(),16);control.draw(image.data(),0,110.2);
    assert(image[(32+45)*1280+32+int(310*.45f)]==16);
    // Yellow keeps its own presentation during the same bounded grace, not red.
    lamp.color="YELLOW";lamp.track_id=90;
    for(int i=0;i<4;++i)input(120+i*.1,{lamp});
    d=input(120.4,{});assert(d.stop_held&&d.held_light==Color::Yellow);
    assert(control.explanation(120.45).signal_light[1]=="YELLOW");
    assert(!control.tick(121.81).stop_held);
    // Suppress the duplicate generic traffic-light overlay, preserving car boxes.
    ai::Result result;result.received=result.finished=qv::now_seconds();result.boxes={{100,100,200,200,.9f,7},{400,100,500,200,.9f,2}};
    std::fill(image.begin(),image.end(),16);ui::panel(image.data(),0,"HDMI",true,true,true,result,10,500,false);
    assert(image[100*1280+77]!=ui::Green.y&&image[100*1280+212]==ui::Green.y);
    std::cout<<"PASS stop grace vs visual-cache expiry, GREEN loss, track replacement, contradictory duplicate, recheck/loop reset, panel dedup and bounds\n";
}
