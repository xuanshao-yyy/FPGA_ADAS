#include "visual.hpp"
#include <cassert>
#include <iostream>
namespace {
constexpr int bytes=1280*720*3/2;
void fill_panel(std::vector<uint8_t>& p,int channel,bool content,int seed){
    int x=channel%2*640+(content?32:0),y=channel/2*360+(content?32:0),w=content?576:640,h=content?324:360;
    for(int row=0;row<h;++row)for(int col=0;col<w;++col)p[(y+row)*1280+x+col]=content?uint8_t(16+(seed+row+col)%200):16;
    for(int row=0;row<h/2;++row)for(int col=0;col<w;++col)p[1280*720+(y/2+row)*1280+x+col]=content?uint8_t(60+(seed+row+col)%120):128;
}
void draw(std::vector<uint8_t>& image,int frame){
    for(int c=0;c<4;++c){int x=c%2*640,y=c/2*360;
        ui::rectangle(image.data(),x+30,y+30,x+609,y+357,ui::ChannelColors[c]);
        ui::text(image.data(),x+32,y+4,frame%2?"CH0 LONG LABEL 123456789":"CH0",ui::White,2,x+620);
        ui::text(image.data(),x+32,y+22,frame%2?"AI WAITING LONG STATUS":"AI OFF",ui::White,1,x+620);
        ui::rectangle(image.data(),x+70+frame,y+80,x+200+frame,y+170,frame%2?ui::Green:ui::White);
    }
}
}
int main(){
    std::vector<uint8_t> baseline(bytes,16),content(bytes,16);std::fill(baseline.begin()+1280*720,baseline.end(),128);content=baseline;
    for(int frame=0;frame<12;++frame){
        for(int c=0;c<4;++c){bool fresh=(frame+c)%4!=0;
            fill_panel(baseline,c,false,0);if(fresh)fill_panel(baseline,c,true,frame+c);
            if(fresh)fill_panel(content,c,true,frame+c);else fill_panel(content,c,false,0);
        }
        ui::clear_panel_headers(content.data());draw(baseline,frame);draw(content,frame);assert(baseline==content);
    }
    std::vector<uint8_t> guard(bytes+64,77);ui::clear_panel_headers(guard.data()+32);
    for(int i=0;i<32;++i)assert(guard[i]==77&&guard[guard.size()-1-i]==77);
    // Surround crops out the canonical headers. Omitting those writes must
    // preserve every visible luma/chroma byte, including detection captions.
    for(unsigned channel=0;channel<4;++channel){
        std::vector<uint8_t> with_header(bytes,77),without_header=with_header;
        ai::Result result;result.finished=result.received=qv::now_seconds();result.boxes={{200,200,700,650,.9f,2}};
        ui::panel(with_header.data(),channel,"TEST",true,true,true,result,12,1000,true,true,true);
        ui::panel(without_header.data(),channel,"TEST",true,true,true,result,12,1000,true,true,false);
        int x=int(channel%2)*640+32,y=int(channel/2)*360+32;
        for(int row=0;row<324;++row)assert(std::equal(with_header.begin()+(y+row)*1280+x,with_header.begin()+(y+row)*1280+x+576,without_header.begin()+(y+row)*1280+x));
        for(int row=0;row<162;++row)assert(std::equal(with_header.begin()+1280*720+(y/2+row)*1280+x,with_header.begin()+1280*720+(y/2+row)*1280+x+576,without_header.begin()+1280*720+(y/2+row)*1280+x));
    }
    std::cout<<"PASS byte-identical full-clear/content-refresh with changing frames, source loss/recovery, caption shortening and NV12 bounds. Real RGA/DMA parity pending.\n";
}
