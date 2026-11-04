#pragma once
#include "light_candidate.hpp"
#include <numeric>
#include <string>
namespace ai {
struct RGBDecision {
    std::array<float,3> probabilities{};
    std::string raw="UNKNOWN",color="UNKNOWN",reason="RGB_NOT_RUN";
    float probability=0,margin=0;
    double prep_ms=0,api_ms=0;
};
inline cv::Mat rgb_model_input(const uint8_t* pixels,const Box& box){
    if(box.x2-box.x1<8 || box.y2-box.y1<8)return {};
    Box expanded=box;double dx=(double(box.x2)-box.x1)*.08,dy=(double(box.y2)-box.y1)*.08;
    expanded.x1=std::floor(double(box.x1)-dx);expanded.x2=std::ceil(double(box.x2)+dx);
    expanded.y1=std::floor(double(box.y1)-dy);expanded.y2=std::ceil(double(box.y2)+dy);
    auto crop=light_roi_rgb565(pixels,expanded);if(crop.empty())return {};
    double scale=std::min(128./crop.cols,128./crop.rows);
    int w=std::max(1,int(std::nearbyint(crop.cols*scale))),h=std::max(1,int(std::nearbyint(crop.rows*scale)));
    cv::Mat input(128,128,CV_8UC3,cv::Scalar(114,114,114)),resized;
    cv::resize(crop,resized,{w,h},0,0,scale<1?cv::INTER_AREA:cv::INTER_LINEAR);
    resized.copyTo(input(cv::Rect((128-w)/2,(128-h)/2,w,h)));return input;
}
inline RGBDecision rgb_decision(const std::array<float,3>& logits,const std::string& hsv,float threshold=.6f,float margin=.15f){
    RGBDecision result;
    for(float v:logits)if(!std::isfinite(v)){result.reason="RGB_NONFINITE";return result;}
    float maximum=*std::max_element(logits.begin(),logits.end()),sum=0;
    for(unsigned i=0;i<3;++i){result.probabilities[i]=std::exp(logits[i]-maximum);sum+=result.probabilities[i];}
    for(auto& v:result.probabilities)v/=sum;
    int best=int(std::max_element(result.probabilities.begin(),result.probabilities.end())-result.probabilities.begin());
    float second=0;for(int i=0;i<3;++i)if(i!=best)second=std::max(second,result.probabilities[i]);
    const char* names[]={"RED","YELLOW","GREEN"};result.raw=names[best];result.probability=result.probabilities[best];result.margin=result.probability-second;
    if(result.probability<threshold||result.margin<margin){result.reason="RGB_LOW_CONFIDENCE";return result;}
    // The trained model has no OFF/non-signal class: dark compartments must not authorize GREEN.
    if(hsv=="UNKNOWN"){result.reason="RGB_PIXEL_EVIDENCE_UNKNOWN";return result;}
    if(hsv!=result.raw){result.reason="RGB_PIXEL_DISAGREE";return result;}
    result.color=result.raw;result.reason="RGB_PIXEL_CONSENSUS";return result;
}
}
