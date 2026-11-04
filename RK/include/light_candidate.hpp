#pragma once
#include "detection.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <array>

namespace ai {
// Per detected housing only. Color evidence is NOT an ego-lane signal association.
struct LightCandidate {
    Box box;
    const char* color="UNKNOWN";
    const char* reason="NO_COLOR_COMPONENT";
    std::array<int,3> components{}; // red, yellow, green largest component areas
    int analysis_width=0,analysis_height=0;
    double dominance=0;
};
inline LightCandidate classify_light(const cv::Mat& rgb,const Box& box,int min_s=100,int min_v=160,int red_high=170,double min_ratio=.005){
    if(rgb.type()!=CV_8UC3 || rgb.empty())throw std::runtime_error("Light ROI must be RGB8");
    LightCandidate out;out.box=box;
    if(rgb.cols<4 || rgb.rows<6){out.reason="ROI_TOO_SMALL";return out;}
    cv::Mat resized=rgb,hsv;
    if(std::max(rgb.cols,rgb.rows)>128){double s=128./std::max(rgb.cols,rgb.rows);cv::resize(rgb,resized,{},s,s,cv::INTER_AREA);}
    cv::cvtColor(resized,hsv,cv::COLOR_RGB2HSV);
    out.analysis_width=hsv.cols;out.analysis_height=hsv.rows;
    std::array<cv::Mat,3> masks;
    for(auto& mask:masks)mask=cv::Mat::zeros(hsv.size(),CV_8U);
    for(int y=0;y<hsv.rows;++y)for(int x=0;x<hsv.cols;++x){auto v=hsv.at<cv::Vec3b>(y,x);
        if(v[1]<min_s || v[2]<min_v)continue;
        int color=v[0]<=10 || v[0]>=red_high?0:v[0]>=15 && v[0]<=35?1:v[0]>=40 && v[0]<=95?2:-1;
        if(color>=0)masks[color].at<uint8_t>(y,x)=255;
    }
    for(int c=0;c<3;++c){cv::Mat labels,stats,centers;
        int count=cv::connectedComponentsWithStats(masks[c],labels,stats,centers,8,CV_32S);
        for(int i=1;i<count;++i){int area=stats.at<int>(i,cv::CC_STAT_AREA);
            int w=stats.at<int>(i,cv::CC_STAT_WIDTH),h=stats.at<int>(i,cv::CC_STAT_HEIGHT);
            if(w>=2 && h>=2 && w<=3*h && h<=3*w)out.components[c]=std::max(out.components[c],area);}
    }
    int best=int(std::max_element(out.components.begin(),out.components.end())-out.components.begin());
    int area=out.components[best],sum=out.components[0]+out.components[1]+out.components[2];
    int minimum=std::max(4,int(std::ceil(hsv.total()*min_ratio)));
    if(area<minimum){out.reason="INSUFFICIENT_COLOR_PIXELS";return out;}
    out.dominance=double(area)/sum;
    for(int c=0;c<3;++c)if(c!=best && out.components[c]>=minimum){out.reason="MULTIPLE_COLORS";return out;}
    if(out.dominance<.85){out.reason="AMBIGUOUS_COLOR";return out;}
    if(area>int(hsv.total()*.6)){out.reason="COLOR_FILLS_HOUSING";return out;}
    const char* names[]={"RED","YELLOW","GREEN"};out.color=names[best];out.reason="COLOR_COMPONENT_CANDIDATE";
    return out;
}
// Reads only the original RGB565 little-endian bounding ROI: no full-frame color pass.
inline cv::Mat light_roi_rgb565(const uint8_t* source,const Box& box){
    if(!source || !std::isfinite(box.x1)||!std::isfinite(box.x2)||!std::isfinite(box.y1)||!std::isfinite(box.y2))return {};
    int x1=int(std::floor(std::clamp(box.x1,0.f,1280.f))),x2=int(std::ceil(std::clamp(box.x2,0.f,1280.f)));
    int y1=int(std::floor(std::clamp(box.y1,0.f,720.f))),y2=int(std::ceil(std::clamp(box.y2,0.f,720.f)));
    if(x2<=x1 || y2<=y1 || x2-x1>512 || y2-y1>512)return {};
    cv::Mat rgb(y2-y1,x2-x1,CV_8UC3);
    for(int y=y1;y<y2;++y)for(int x=x1;x<x2;++x){size_t i=(size_t(y)*1280+x)*2;unsigned p=source[i]|unsigned(source[i+1])<<8;
        unsigned r=(p>>11)&31,g=(p>>5)&63,b=p&31;
        rgb.at<cv::Vec3b>(y-y1,x-x1)=cv::Vec3b((r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2));}
    return rgb;
}
}
