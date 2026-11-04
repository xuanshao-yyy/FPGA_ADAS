#pragma once
#include "ego_car_asset.hpp"
#include "visual.hpp"
#include "camera_roles.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace surround_ui {
// Display-only layout, 1280x720. Inference and original overlay coordinates stay
// canonical: four 1280x720 sources -> four 576x324 annotated content rectangles.
// This is four separate views around an illustrative car, NOT a BEV stitching.
struct Rect {int x,y,w,h;bool contains(int xx,int yy)const{return xx>=x&&yy>=y&&xx<x+w&&yy<y+h;}};
struct Layout {
    unsigned front=0;bool front_priority=false;
    std::array<unsigned,4> channels()const{
        std::array<unsigned,4> order{front,0,0,0};unsigned next=1;
        for(unsigned ch:camera_roles::Order)if(ch!=front)order[next++]=ch;
        return order;
    }
    static constexpr std::array<Rect,4> tiles{{{352,24,576,324},{16,382,416,234},{848,382,416,234},{480,532,320,180}}};
    static constexpr std::array<Rect,4> front_tiles{{{32,48,672,378},{64,512,320,180},{896,512,320,180},{480,512,320,180}}};
    const std::array<Rect,4>& destinations()const{return front_priority?front_tiles:tiles;}
    Rect source(unsigned role)const{unsigned ch=channels()[role];return {int(ch%2)*640+32,int(ch/2)*360+32,576,324};}
    // Reject matte/car/gutters. Return the same canonical coordinates used by
    // Control::set_roi, never treat a car sprite click as a camera observation.
    bool canonical(int x,int y,unsigned& channel,int& cx,int& cy)const{
        for(unsigned role=0;role<4;++role){auto r=destinations()[role];if(!r.contains(x,y))continue;
            auto s=source(role);channel=channels()[role];
            cx=s.x+(x-r.x)*s.w/r.w;cy=s.y+(y-r.y)*s.h/r.h;return true;
        }
        return false;
    }
};
inline void initialize_original(uint8_t* image,unsigned front=0){
    std::fill(image,image+1280*720,uint8_t(38));
    for(size_t p=1280*720;p<1280*720*3/2;p+=2){image[p]=134;image[p+1]=122;}
    constexpr ui::Color subtle{82,136,122},muted{155,136,122},accent{186,144,96};
    const char* roles[]{"FRONT","LEFT","RIGHT","REAR"};auto order=Layout{front}.channels();
    for(unsigned role=0;role<4;++role){auto r=Layout::tiles[role];
        ui::rectangle(image,r.x-2,r.y-2,r.x+r.w+1,r.y+r.h+1,subtle,1);
        ui::text(image,r.x,r.y-16,std::string(roles[role])+" / CH"+std::to_string(order[role]),role?muted:accent,2,r.x+r.w);
    }
    // Static orientation marks do not reflect any permission or risk judgement.
    ui::text(image,24,24,"XINXING / VISION",muted,1,320);
    ui::text(image,1120,24,"SURROUND",muted,1,1264);
    // Resample the existing car once while initializing each canvas, never per frame.
    constexpr int x=600,y=364,w=80,h=160;
    for(int row=0;row<h;++row)for(int col=0;col<w;++col)image[(y+row)*1280+x+col]=EgoCarNv12[(row*192/h)*96+col*96/w];
    for(int row=0;row<h/2;++row)for(int col=0;col<w;col+=2){size_t source=96*192+(row*96/(h/2))*96+(col*96/w/2)*2;
        image[1280*720+(y/2+row)*1280+x+col]=EgoCarNv12[source];image[1280*720+(y/2+row)*1280+x+col+1]=EgoCarNv12[source+1];}
    // A small neutral nose marker, not a proposed trajectory.
    for(int k=0;k<5;++k){ui::pixel(image,640-k,352+k,subtle);ui::pixel(image,640+k,352+k,subtle);}
}
inline void initialize(uint8_t* image,unsigned front=0,bool front_priority=false){
    if(!front_priority){initialize_original(image,front);return;}
    std::fill(image,image+1280*720,uint8_t(38));
    for(size_t p=1280*720;p<1280*720*3/2;p+=2){image[p]=134;image[p+1]=122;}
    constexpr ui::Color subtle{82,136,122},muted{155,136,122},accent{186,144,96};
    const char* roles[]{"FRONT","LEFT","RIGHT","REAR"};auto layout=Layout{front,true};auto order=layout.channels();
    for(unsigned role=0;role<4;++role){auto r=layout.destinations()[role];
        ui::rectangle(image,r.x-2,r.y-2,r.x+r.w+1,r.y+r.h+1,subtle,1);
        ui::text(image,r.x,r.y-16,std::string(roles[role])+" / CH"+std::to_string(order[role]),role?muted:accent,2,r.x+r.w);}
}

}
