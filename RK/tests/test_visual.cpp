#include "visual.hpp"
#include <iostream>
int main(){try{
    constexpr size_t bytes=1280*720*3/2,guard=64;
    std::vector<uint8_t> storage(bytes+2*guard,0xA5);auto pixels=storage.data()+guard;
    std::fill(pixels,pixels+bytes,16);
    ai::Result result;result.finished=qv::now_seconds();result.received=result.finished;result.boxes={{0,0,1280,720,.9f,7}};
    // Real CH0 capture: result frame320992/source641706; newer frames change source.
    result.frame_id=320992;result.source=641706;
    if(!ui::result_in_frame_order(result,320994))throw std::runtime_error("Newer background must retain detections from an older captured-frame sequence");
    if(ui::result_in_frame_order(result,320990))throw std::runtime_error("Frame rollback must still reject old results");
    result.frame_id=0xfffffffeu;
    if(!ui::result_in_frame_order(result,1))throw std::runtime_error("Normal uint32 frame wrap must retain recent results");
    result.frame_id=2;
    if(ui::result_in_frame_order(result,1))throw std::runtime_error("A result ahead of the background must be rejected");
    result.frame_id=320992;
    for(unsigned k=0;k<4;++k)ui::panel(pixels,k,"HDMI IN1",true,true,true,result,3,500);
    // Check the vertical edge below the caption: caption text may cover the corner.
    for(unsigned k=0;k<4;++k){unsigned x=k%2*640+32,y=k/2*360+100;
        if(pixels[y*1280+x]!=ui::Green.y)throw std::runtime_error("Fresh nonempty results must draw green boxes in each panel");}
    for(size_t k=0;k<guard;++k)if(storage[k]!=0xA5 || storage[guard+bytes+k]!=0xA5)throw std::runtime_error("NV12 overlay wrote outside the canvas");
    for(unsigned y=32;y<720;++y)if(pixels[y*1280+639]!=16 || pixels[y*1280+640]!=16)throw std::runtime_error("Separated-panel center gutter was overwritten");
    // A stale result must not draw its green detection border.
    std::fill(pixels,pixels+bytes,16);result.received=qv::now_seconds()-2;
    ui::panel(pixels,0,"HDMI IN1",true,true,true,result,3,500);
    if(pixels[32*1280+32]==ui::Green.y)throw std::runtime_error("Stale detection boxes must not be displayed");
    std::fill(pixels,pixels+bytes,16);result.received=qv::now_seconds();result.finished=result.received;
    result.tracking=true;result.tracks={{{100,100,300,300,.8f,2},7,false,result.received}};
    ui::panel(pixels,0,"HDMI IN1",true,true,true,result,10,500);
    if(pixels[100*1280+77]!=ui::Green.y)throw std::runtime_error("Confirmed tracks must render");
    std::fill(pixels,pixels+bytes,16);result.tracks[0].predicted=true;result.tracks[0].observed=qv::now_seconds()-1;
    ui::panel(pixels,0,"HDMI IN1",true,true,true,result,10,500);
    if(pixels[100*1280+77]==180)throw std::runtime_error("Expired prediction must not render");
    result.tracks[0].observed=qv::now_seconds();
    ui::panel(pixels,0,"HDMI IN1",true,true,true,result,10,500);
    if(pixels[101*1280+77]!=180)throw std::runtime_error("Fresh prediction must render dashed amber");
    for(size_t k=0;k<guard;++k)if(storage[k]!=0xA5 || storage[guard+bytes+k]!=0xA5)throw std::runtime_error("Tracking overlay exceeded bounds");
    std::cout<<"PASS: changing capture sequence, frame rollback/wrap, visible fresh boxes, panel gutters, NV12 buffer bounds, stale boxes hidden\n";return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
