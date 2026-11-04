#include "light_candidate.hpp"
#include "lane4_post.hpp"
#include <cassert>
#include <cstring>
#include <iostream>

int main(){
    auto image=[](cv::Scalar color){cv::Mat rgb=cv::Mat::zeros(48,20,CV_8UC3);cv::circle(rgb,{10,12},5,color,-1);return rgb;};
    ai::Box box{100,100,120,148,.9f,7};
    assert(std::string(ai::classify_light(image({255,0,0}),box).color)=="RED");
    assert(std::string(ai::classify_light(image({255,220,0}),box).color)=="YELLOW");
    assert(std::string(ai::classify_light(image({0,255,0}),box).color)=="GREEN");
    assert(std::string(ai::classify_light(image({145,40,90}),box).color)=="UNKNOWN");
    assert(std::string(ai::classify_light(image({145,40,90}),box,100,120,160).color)=="RED");
    auto mixed=image({255,0,0});cv::circle(mixed,{10,35},5,{0,255,0},-1);
    assert(std::string(ai::classify_light(mixed,box).reason)=="MULTIPLE_COLORS");
    assert(std::string(ai::classify_light(cv::Mat(48,20,CV_8UC3,cv::Scalar(255,0,0)),box).reason)=="COLOR_FILLS_HOUSING");
    assert(std::string(ai::classify_light(cv::Mat::zeros(5,3,CV_8UC3),box).reason)=="ROI_TOO_SMALL");
    assert(std::string(ai::classify_light(cv::Mat(48,20,CV_8UC3,cv::Scalar(255,255,255)),box).color)=="UNKNOWN");
    std::vector<uint8_t> source(1280*720*2,0);size_t at=(100*1280+100)*2;source[at]=0;source[at+1]=0xf8;
    auto roi=ai::light_roi_rgb565(source.data(),box);assert(roi.at<cv::Vec3b>(0,0)==cv::Vec3b(255,0,0));
    auto invalid=box;invalid.x1=std::numeric_limits<float>::quiet_NaN();assert(ai::light_roi_rgb565(source.data(),invalid).empty());
    cv::Mat blank=cv::Mat::zeros(360,640,CV_8U);lane4::Post post("camera_live.json",.5);
    auto empty=post.process(blank,blank,0,0,true);
    assert(std::string(empty.left_evidence.reason)=="NO_CURVE" && std::string(empty.pair_reason)=="MISSING_BOUNDARY");
    cv::Mat classes=blank.clone();cv::line(classes,{160,140},{160,355},cv::Scalar(1),3);cv::line(classes,{480,140},{480,355},cv::Scalar(2),3);
    for(int i=0;i<3;++i){auto mask=post.process(classes,blank,.1+i*.1,0,true);
        assert(mask.left_evidence.raw_style==1 && mask.right_evidence.raw_style==2);
        assert(mask.left_evidence.confirmation_streak==i+1);
        if(i<2)assert(mask.left_style==1 && mask.right_style==2 && mask.left_evidence.direct && std::string(mask.left_evidence.reason)=="CONFIRMED_CANDIDATE");
        else assert(mask.left_style==1 && mask.right_style==2);
    }
    auto reset=post.process(classes,blank,2,0,true);assert(reset.left_style==1 && reset.left_evidence.direct && reset.left_evidence.confirmation_streak==1);
    { // Screen-filmed road: a valid outer stripe falls outside the old narrow trapezoid.
        auto road=blank.clone();cv::line(road,{0,316},{240,205},cv::Scalar(1),3);cv::line(road,{270,215},{350,331},cv::Scalar(2),3);
        lane4::Post narrow("camera_live.json",.5),wide("camera_wide.json",.5);
        assert(!narrow.process(road,blank,10,0,true).left_curve.valid);
        for(int i=0;i<3;++i){auto recovered=wide.process(road,blank,10+i*.1,0,true);
            assert(recovered.left_curve.valid&&recovered.right_curve.valid&&recovered.geometry.valid);
            if(i==2)assert(recovered.left_style==1&&recovered.right_style==2);
        }
        auto short_road=blank.clone();cv::line(short_road,{190,343},{260,291},cv::Scalar(1),3);cv::line(short_road,{372,343},{300,291},cv::Scalar(2),3);
        wide.reset(0);
        for(int i=0;i<3;++i){auto m=wide.process(short_road,blank,11+i*.1,0,true);assert(m.geometry.valid);if(i==2)assert(m.left_style==1&&m.right_style==2);}
        auto fragments=blank.clone();cv::line(fragments,{190,343},{210,327},cv::Scalar(1),3);cv::line(fragments,{372,343},{352,327},cv::Scalar(2),3);
        auto rejected=wide.process(fragments,blank,12,0,true);assert(!rejected.geometry.valid&&rejected.left_style==0&&rejected.right_style==0);
    }
    std::cout<<"PASS color candidates, ambiguity/size/white/filled-housing rejection, RGB565 ROI, lane UNKNOWN causes and high-support direct classification and temporal diagnostics; no real-road accuracy claim.\n";
}
