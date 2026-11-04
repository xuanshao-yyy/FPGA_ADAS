#pragma once
#include "ai_worker.hpp"
#include <array>
#include <cstdio>
#include <string>

namespace ui {
constexpr int ContentX=32,ContentY=32,ContentW=576,ContentH=324;
struct Color {uint8_t y,u,v;};
inline constexpr Color White{235,128,128},Green{145,54,34};
inline constexpr std::array<Color,4> ChannelColors{{{170,166,16},{160,44,166},{180,16,146},{130,190,180}}};
// Each active content rectangle is completely overwritten by RGA. Only the two
// header strips need a CPU clear; panel gutters remain initialized black.
inline void clear_panel_headers(uint8_t* image){
    for(int top:{0,360}){
        std::fill(image+top*1280,image+(top+32)*1280,uint8_t(16));
        std::fill(image+1280*720+(top/2)*1280,image+1280*720+(top/2+16)*1280,uint8_t(128));
    }
}
// QV02 source is a captured-frame sequence, not a stable camera identity.
// Results may overlay newer frames from the same receiver within their age limit.
template<class Result> inline bool result_in_frame_order(const Result& result,uint32_t current_frame_id){
    return result.finished && uint32_t(current_frame_id-result.frame_id)<0x80000000u;
}
inline std::array<uint8_t,7> glyph(char c){
    switch(c){
    case 'A':return {14,17,17,31,17,17,17};case 'B':return {30,17,17,30,17,17,30};
    case 'C':return {14,17,16,16,16,17,14};case 'D':return {30,17,17,17,17,17,30};
    case 'E':return {31,16,16,30,16,16,31};case 'F':return {31,16,16,30,16,16,16};
    case 'G':return {14,17,16,23,17,17,15};case 'H':return {17,17,17,31,17,17,17};
    case 'I':return {31,4,4,4,4,4,31};case 'J':return {7,2,2,2,18,18,12};
    case 'K':return {17,18,20,24,20,18,17};case 'L':return {16,16,16,16,16,16,31};
    case 'M':return {17,27,21,21,17,17,17};case 'N':return {17,25,25,21,19,19,17};
    case 'O':return {14,17,17,17,17,17,14};case 'P':return {30,17,17,30,16,16,16};
    case 'Q':return {14,17,17,17,21,18,13};case 'R':return {30,17,17,30,20,18,17};
    case 'S':return {15,16,16,14,1,1,30};case 'T':return {31,4,4,4,4,4,4};
    case 'U':return {17,17,17,17,17,17,14};case 'V':return {17,17,17,17,17,10,4};
    case 'W':return {17,17,17,21,21,21,10};case 'X':return {17,17,10,4,10,17,17};
    case 'Y':return {17,17,10,4,4,4,4};case 'Z':return {31,1,2,4,8,16,31};
    case '0':return {14,17,19,21,25,17,14};case '1':return {4,12,4,4,4,4,14};
    case '2':return {14,17,1,2,4,8,31};case '3':return {30,1,1,14,1,1,30};
    case '4':return {2,6,10,18,31,2,2};case '5':return {31,16,16,30,1,1,30};
    case '6':return {14,16,16,30,17,17,14};case '7':return {31,1,2,4,8,8,8};
    case '8':return {14,17,17,14,17,17,14};case '9':return {14,17,17,15,1,1,14};
    case '.':return {0,0,0,0,0,12,12};case ':':return {0,4,4,0,4,4,0};
    case '-':return {0,0,0,31,0,0,0};case '_':return {0,0,0,0,0,0,31};
    case '/':return {1,2,2,4,8,8,16};case '%':return {17,2,4,4,8,16,17};
    default:return {};
    }
}
inline void pixel(uint8_t* image,int x,int y,Color color){
    if(x<0 || x>=1280 || y<0 || y>=720)return;
    image[y*1280+x]=color.y;size_t uv=1280*720+(y/2)*1280+(x&~1);
    image[uv]=color.u;image[uv+1]=color.v;
}
inline void rectangle(uint8_t* image,int x1,int y1,int x2,int y2,Color color,int thickness=2){
    for(int k=0;k<thickness;++k){
        for(int x=x1;x<=x2;++x){pixel(image,x,y1+k,color);pixel(image,x,y2-k,color);}
        for(int y=y1;y<=y2;++y){pixel(image,x1+k,y,color);pixel(image,x2-k,y,color);}
    }
}
inline void text(uint8_t* image,int x,int y,const std::string& value,Color color,int scale=1,int right=1280){
    for(char c:value){if(x+5*scale>=right)break;auto rows=glyph(c);
        for(int yy=0;yy<7;++yy)for(int xx=0;xx<5;++xx)if(rows[yy]&(1<<(4-xx)))
            for(int dy=0;dy<scale;++dy)for(int dx=0;dx<scale;++dx)pixel(image,x+xx*scale+dx,y+yy*scale+dy,color);
        x+=6*scale;
    }
}
// Immutable mask lease prevents address reuse from masquerading as a cache hit.
// Cache only nonzero 2x2 blocks; all integer blend/UV selection rules stay exact.
class LaneOverlayCache {
    struct Block {uint16_t x,y;std::array<uint8_t,4> values;unsigned hits;uint8_t chroma;};
    std::shared_ptr<const ai::LaneMask> owner;
    std::vector<Block> blocks;
    uint64_t builds_=0;
public:
    void bind(std::shared_ptr<const ai::LaneMask> mask){
        if(mask==owner)return;
        owner=std::move(mask);blocks.clear();
        if(!owner)return;
        if(owner->data.size()!=size_t(ContentW*ContentH))throw std::runtime_error("Lane overlay mask geometry mismatch");
        ++builds_;
        for(int y=0;y<ContentH;y+=2)for(int x=0;x<ContentW;x+=2){
            size_t p=size_t(y)*ContentW+x;
            std::array<uint8_t,4> v{owner->data[p],owner->data[p+1],owner->data[p+ContentW],owner->data[p+ContentW+1]};
            unsigned hits=unsigned(v[0]!=0)+unsigned(v[1]!=0)+unsigned(v[2]!=0)+unsigned(v[3]!=0);
            if(hits)blocks.push_back({uint16_t(x),uint16_t(y),v,hits,v[0]?v[0]:std::max({v[1],v[2],v[3]})});
        }
    }
    bool matches(const ai::LaneMask* mask)const{return owner&&owner.get()==mask&&blocks.size()<=size_t(ContentW*ContentH/8);}
    uint64_t builds()const{return builds_;}
    size_t active_blocks()const{return blocks.size();}
    void blend(uint8_t* image,int left,int top)const{
        constexpr Color amber{180,85,170};
        constexpr std::array<Color,4> marking{{White,Green,{110,190,110},{110,200,200}}};
        for(const auto& b:blocks){
            for(unsigned k=0;k<4;++k)if(b.values[k]){
                auto c=owner->four_class?marking[std::min<unsigned>(3,b.values[k])]:amber;
                auto& v=image[(top+b.y+int(k/2))*1280+left+b.x+int(k%2)];v=uint8_t((unsigned(v)+c.y)/2);
            }
            auto c=owner->four_class?marking[std::min<unsigned>(3,b.chroma)]:amber;
            size_t uv=1280*720+((top+b.y)/2)*1280+left+b.x;
            image[uv]=uint8_t((image[uv]*(8-b.hits)+c.u*b.hits)/8);
            image[uv+1]=uint8_t((image[uv+1]*(8-b.hits)+c.v*b.hits)/8);
        }
    }
};
inline void lane_overlay(uint8_t* image,unsigned channel,bool fresh,const ai::LaneMask* mask,double rate,unsigned max_age_ms,bool draw_curves=true,bool draw_status=true,const LaneOverlayCache* cache=nullptr){
    int left=int(channel%2)*640+ContentX,top=int(channel/2)*360+ContentY;
    double age=mask?(qv::now_seconds()-mask->received)*1000:-1;
    bool valid=fresh && mask && mask->finished && age<=max_age_ms;
    constexpr Color Amber{180,85,170};
    constexpr std::array<Color,4> marking{{White,Green,{110,190,110},{110,200,200}}};
    if(valid){
        if(mask->data.size()!=size_t(ContentW*ContentH))throw std::runtime_error("Lane overlay mask geometry mismatch");
        if(cache&&cache->matches(mask))cache->blend(image,left,top);
        else for(int y=0;y<ContentH;y+=2)for(int x=0;x<ContentW;x+=2){
            unsigned hits=0;
            for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx)if(mask->data[(y+dy)*ContentW+x+dx]){
                auto& value=image[(top+y+dy)*1280+left+x+dx];auto c=mask->four_class?marking[std::min<unsigned>(3,mask->data[(y+dy)*ContentW+x+dx])]:Amber;value=uint8_t((unsigned(value)+c.y)/2);++hits;}
            if(hits){auto c=mask->four_class?marking[std::min<unsigned>(3,mask->data[y*ContentW+x]?mask->data[y*ContentW+x]:std::max({mask->data[y*ContentW+x+1],mask->data[(y+1)*ContentW+x],mask->data[(y+1)*ContentW+x+1]}))]:Amber;size_t uv=1280*720+((top+y)/2)*1280+left+x;
                image[uv]=uint8_t((image[uv]*(8-hits)+c.u*hits)/8);
                image[uv+1]=uint8_t((image[uv+1]*(8-hits)+c.v*hits)/8);}
        }
        const auto& g=mask->geometry;
        if(mask->four_class&&draw_curves){
            auto draw=[&](const Curve& curve,int style){
                if(!curve.valid)return;
                auto color=style==1?Green:style==2?Color{110,190,110}:White;
                for(double y=curve.ymin;y<=curve.ymax;y+=.5){
                    int xx=int(std::lround(curve.x(y)*.9)),yy=int(std::lround(y*.9));
                    if(xx<0||xx>=ContentW-1||yy<0||yy>=ContentH)continue;
                    if(mask->blocked.size()==size_t(ContentW*ContentH) && mask->blocked[yy*ContentW+xx])continue;
                    pixel(image,left+xx,top+yy,color);pixel(image,left+xx+1,top+yy,color);
                }
            };
            draw(mask->left_curve,mask->left_style);draw(mask->right_curve,mask->right_style);
        }else if(!mask->four_class&&g.valid){
            constexpr Color Blue{140,190,90};
            auto color=g.confirmed?Blue:White;
            for(int y=int(ContentH*.5f);y<int(ContentH*.94f);++y){
                for(const auto& boundary:{g.left,g.right}){
                    int x=std::clamp(int(boundary.x(float(y))),0,ContentW-2);
                    pixel(image,left+x,top+y,color);pixel(image,left+x+1,top+y,color);
                }
            }
        }
    }
    if(!draw_status)return;
    char status[128];
    if(!mask)std::snprintf(status,sizeof(status),"LANE WAITING - CANDIDATE MASK");
    else std::snprintf(status,sizeof(status),"MODEL %.1fHZ VIEW %s AGE %.0fMS%s",rate,mask->cpu_propagated?"CPU":"MODEL",age,valid?"":" STALE");
    int status_y=top+ContentH-10;
    if(valid && mask->geometry_enabled){
        char geometry[120];
        if(mask->position_valid)std::snprintf(geometry,sizeof(geometry),"%s OFFSET %.2f %s",mask->position,mask->relative_offset,mask->cpu_propagated?"CPU TRACK":"MODEL");
        else std::snprintf(geometry,sizeof(geometry),"BOUNDARIES UNKNOWN - NO OFFSET DECISION");
        for(int y=status_y-12;y<status_y-2;++y)for(int x=left;x<left+ContentW;++x)image[y*1280+x]=16;
        text(image,left+2,status_y-11,geometry,White,1,left+ContentW);
    }
    for(int y=status_y-1;y<top+ContentH;++y)for(int x=left;x<left+ContentW;++x)image[y*1280+x]=16;
    text(image,left+2,status_y,status,White,1,left+ContentW);
}
inline void panel(uint8_t* image,unsigned channel,const char* name,bool active,bool fresh,
                  bool ai_enabled,const ai::Result& result,double ai_fps,unsigned max_age_ms,bool draw_lights=true,bool captions=true,bool header=true){
    int left=int(channel%2)*640,top=int(channel/2)*360;auto color=ChannelColors[channel];
    if(header){
    rectangle(image,left+30,top+30,left+609,top+357,color);
    text(image,left+32,top+4,"CH"+std::to_string(channel)+"  "+name,color,2,left+620);
    }
    double age=result.finished?(qv::now_seconds()-result.received)*1000:-1;
    bool valid=fresh && result.finished && age<=max_age_ms;
    if(header){char status[128];
    if(!active)std::snprintf(status,sizeof(status),"DISABLED");
    else if(!fresh)std::snprintf(status,sizeof(status),"NO COMPLETE INPUT FRAME");
    else if(!ai_enabled)std::snprintf(status,sizeof(status),"AI OFF - RECEIVE/DISPLAY BASELINE");
    else if(!result.finished)std::snprintf(status,sizeof(status),"AI WAITING");
    else std::snprintf(status,sizeof(status),"AI %.1fHZ  AGE %.0fMS  %s %zu%s",ai_fps,age,result.tracking?"TRACKS":"BOXES",result.tracking?result.tracks.size():result.boxes.size(),valid?"":"  STALE");
    text(image,left+32,top+22,status,White,1,left+620);
    }
    if(!valid || !ai_enabled)return;
    auto draw_box=[&](const ai::Box& box,const char* caption,Color color,bool predicted){
        int x1=left+ContentX+std::clamp(int(box.x1*.45f),0,ContentW-1),x2=left+ContentX+std::clamp(int(box.x2*.45f),0,ContentW-1);
        int y1=top+ContentY+std::clamp(int(box.y1*.45f),0,ContentH-1),y2=top+ContentY+std::clamp(int(box.y2*.45f),0,ContentH-1);
        if(predicted){
            for(int x=x1;x<=x2;++x)if((x-x1)%8<4){pixel(image,x,y1,color);pixel(image,x,y2,color);}
            for(int y=y1;y<=y2;++y)if((y-y1)%8<4){pixel(image,x1,y,color);pixel(image,x2,y,color);}
        }else rectangle(image,x1,y1,x2,y2,color);
        if(captions)text(image,x1,std::max(top+ContentY,y1-9),caption,White,1,left+ContentX+ContentW);
    };
    if(result.tracking){
        for(const auto& track:result.tracks){
            if(!draw_lights&&track.box.category==7)continue;
            if(track.predicted && qv::now_seconds()-track.observed>.10)continue;
            char caption[96]{};if(captions)std::snprintf(caption,sizeof(caption),"%s ID%llu %.2f%s",ai::Classes[track.box.category],
                static_cast<unsigned long long>(track.id & 0xffffffffu),track.box.score,track.predicted?" PRED":"");
            draw_box(track.box,caption,track.predicted?Color{180,85,170}:Green,track.predicted);
        }
    }else for(const auto& box:result.boxes){
        if(!draw_lights&&box.category==7)continue;
        if(box.score<(box.category==7&&result.light_display_threshold>0?result.light_display_threshold:result.display_threshold))continue;
        char caption[64]{};if(captions)std::snprintf(caption,sizeof(caption),"%s %.2f",ai::Classes[box.category],box.score);
        draw_box(box,caption,Green,false);
    }
}
}
