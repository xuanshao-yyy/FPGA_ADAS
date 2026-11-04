#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace ai {
inline constexpr const char* Classes[]={"PERSON","RIDER","CAR","BUS","TRUCK","BICYCLE","MOTORCYCLE","TRAFFIC_LIGHT"};
struct Box {float x1=0,y1=0,x2=0,y2=0,score=0;int category=0;};
struct Tensor {
    const float* data=nullptr;int channels=0,height=0,width=0;bool nhwc=false;
    const int8_t* quantized=nullptr;float scale=1;int zero_point=0;
    float at(int channel,int y,int x) const {
        auto index=nhwc?(y*width+x)*channels+channel:(channel*height+y)*width+x;
        return quantized?(int(quantized[index])-zero_point)*scale:data[index];
    }
};
inline float overlap(const Box& a,const Box& b){
    float intersection=std::max(0.f,std::min(a.x2,b.x2)-std::max(a.x1,b.x1))*
                       std::max(0.f,std::min(a.y2,b.y2)-std::max(a.y1,b.y1));
    float area_a=std::max(0.f,a.x2-a.x1)*std::max(0.f,a.y2-a.y1);
    float area_b=std::max(0.f,b.x2-b.x1)*std::max(0.f,b.y2-b.y1);
    return intersection/std::max(area_a+area_b-intersection,1e-9f);
}
inline std::vector<Box> nms(std::vector<Box> boxes,float iou){
    std::stable_sort(boxes.begin(),boxes.end(),[](const Box&a,const Box&b){return a.score>b.score;});
    if(boxes.size()>1000)boxes.resize(1000);
    std::vector<Box> kept;
    for(const auto& box:boxes){
        bool suppressed=false;
        for(const auto& previous:kept)if(box.category==previous.category && overlap(box,previous)>iou){suppressed=true;break;}
        if(!suppressed)kept.push_back(box);
        if(kept.size()==300)break;
    }return kept;
}
inline std::vector<Box> decode_candidates(const std::array<Tensor,9>& outputs,float confidence,int input_width=640,int input_height=640){
    std::vector<Box> boxes;
    for(int branch=0;branch<3;++branch){
        int stride=8<<branch,h=input_height/stride,w=input_width/stride;
        const auto& reg=outputs[branch*3];const auto& cls=outputs[branch*3+1];const auto& sum=outputs[branch*3+2];
        if((!reg.data&&!reg.quantized) || (!cls.data&&!cls.quantized) || (!sum.data&&!sum.quantized) || reg.channels!=64 || (cls.channels!=8&&cls.channels!=10) || sum.channels!=1 ||
           reg.height!=h || reg.width!=w || cls.height!=h || cls.width!=w || sum.height!=h || sum.width!=w)
            throw std::runtime_error("Detector output contract mismatch: 9 outputs / 64,(8 or 10),1 channels / input-derived grid");
        for(int y=0;y<h;++y)for(int x=0;x<w;++x){
            int category=0;float score=-1;
            // Keep the original eight application classes BEFORE ranking/NMS limits.
            // YOLO11 channels 8/9 (sign/train) remain decoded in the tensor contract only.
            for(int c=0;c<8;++c){float v=cls.at(c,y,x);
                if(!std::isfinite(v))throw std::runtime_error("Non-finite classification output");
                if(v>score){score=v;category=c;}
            }
            if(score<confidence)continue;
            // Class probabilities already include sigmoid. score_sum is not objectness.
            float distances[4]{};
            for(int side=0;side<4;++side){
                float logits[16],maximum=-1e30f;
                for(int k=0;k<16;++k){logits[k]=reg.at(side*16+k,y,x);
                    if(!std::isfinite(logits[k]))throw std::runtime_error("Non-finite DFL output");
                    maximum=std::max(maximum,logits[k]);}
                float denominator=0,numerator=0;
                for(int k=0;k<16;++k){float probability=std::exp(logits[k]-maximum);denominator+=probability;numerator+=k*probability;}
                distances[side]=numerator/denominator;
            }
            boxes.push_back({(x+.5f-distances[0])*stride,(y+.5f-distances[1])*stride,
                             (x+.5f+distances[2])*stride,(y+.5f+distances[3])*stride,score,category});
        }
    }
    return boxes;
}
inline std::vector<Box> decode(const std::array<Tensor,9>& outputs,float confidence,float iou,int input_width=640,int input_height=640){
    int content_height=input_width*9/16,top=(input_height-content_height)/2;
    if(input_width<=0 || input_width%32 || input_height%32 || content_height>input_height)
        throw std::runtime_error("Invalid detector input geometry");
    auto kept=nms(decode_candidates(outputs,confidence,input_width,input_height),iou);
    float factor=1280.f/input_width;
    for(auto& box:kept){
        box.x1=std::clamp(box.x1*factor,0.f,1280.f);box.x2=std::clamp(box.x2*factor,0.f,1280.f);
        box.y1=std::clamp((box.y1-top)*factor,0.f,720.f);box.y2=std::clamp((box.y2-top)*factor,0.f,720.f);
    }
    kept.erase(std::remove_if(kept.begin(),kept.end(),[](const Box& b){return b.x2<=b.x1 || b.y2<=b.y1;}),kept.end());
    return kept;
}
// Each 320x320 quadrant contains a 320x180 image, with 70 rows of padding.
// Reject seam-spanning boxes; NMS and coordinate inversion are per channel.
inline std::array<std::vector<Box>,4> split_mosaic(const std::vector<Box>& candidates,
                                                const std::array<bool,4>& active,float iou){
    std::array<std::vector<Box>,4> channels;
    for(auto box:candidates){
        float cx=(box.x1+box.x2)/2,cy=(box.y1+box.y2)/2;
        if(!std::isfinite(cx) || !std::isfinite(cy) || cx<0 || cx>=640 || cy<0 || cy>=640)continue;
        unsigned channel=unsigned(cx/320)+2*unsigned(cy/320);
        float x=float((channel%2)*320),y=float((channel/2)*320),top=y+70;
        if(!active[channel] || cy<top || cy>=top+180)continue;
        box.x1=std::clamp(box.x1,0.f,640.f);box.x2=std::clamp(box.x2,0.f,640.f);
        box.y1=std::clamp(box.y1,0.f,640.f);box.y2=std::clamp(box.y2,0.f,640.f);
        if(box.x1<x || box.x2>x+320 || box.y1<y || box.y2>y+320)continue;
        box.x1=(box.x1-x)*4;box.x2=(box.x2-x)*4;
        box.y1=std::clamp((box.y1-top)*4,0.f,720.f);box.y2=std::clamp((box.y2-top)*4,0.f,720.f);
        if(box.x2>box.x1 && box.y2>box.y1)channels[channel].push_back(box);
    }
    for(auto& boxes:channels)boxes=nms(std::move(boxes),iou);
    return channels;
}
}
