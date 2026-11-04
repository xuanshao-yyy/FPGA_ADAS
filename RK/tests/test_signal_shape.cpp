#include "signal_shape.hpp"
#include <cassert>
#include <iostream>
#include <chrono>
int main(int argc,char**argv){
    ai::Box box{10,10,90,90,.2f,7};
    const std::array<cv::Point,3> end{{{16,40},{40,16},{64,40}}};
    const std::array<cv::Point,3> start{{{64,40},{40,64},{16,40}}};
    for(int d=0;d<3;++d)for(auto rgb:{cv::Scalar(240,20,20),cv::Scalar(20,240,20),cv::Scalar(240,240,20)}){
        cv::Mat image(80,80,CV_8UC3,cv::Scalar(10,10,10));cv::arrowedLine(image,start[d],end[d],rgb,7,cv::LINE_8,0,.5);
        auto result=ai::classify_signal(image,box),reference=ai::classify_signal(image,box,false);
        assert(result.color==reference.color&&result.direction==reference.direction&&result.shape_score==reference.shape_score&&result.shape_margin==reference.shape_margin&&result.reason==reference.reason);
        std::cout<<d<<" "<<result.shape<<" "<<result.shape_score<<" "<<result.shape_margin<<" "<<result.reason<<"\n";assert(result.direction==d);
    }
    cv::Mat round(80,80,CV_8UC3,cv::Scalar(10,10,10));cv::circle(round,{40,40},15,{20,240,20},-1);
    auto circle=ai::classify_signal(round,box);assert(circle.color=="GREEN"&&circle.direction==-1&&circle.shape=="CIRCLE");
    cv::Mat mixed=round.clone();cv::circle(mixed,{15,15},7,{240,20,20},-1);auto conflict=ai::classify_signal(mixed,box);assert(conflict.color=="UNKNOWN"&&conflict.direction==-1);
    cv::Mat speckle(80,80,CV_8UC3,cv::Scalar(10,10,10));cv::arrowedLine(speckle,{64,40},{16,40},{240,20,20},7,cv::LINE_8,0,.5);
    cv::circle(speckle,{12,12},2,{20,240,20},-1);auto noisy=ai::classify_signal(speckle,box);assert(noisy.color=="RED"&&noisy.direction==0);
    cv::Mat dark(80,80,CV_8UC3,cv::Scalar(10,10,10));assert(ai::classify_signal(dark,box).direction==-1);
    cv::Mat down=dark.clone();cv::arrowedLine(down,{40,16},{40,64},{240,20,20},7,cv::LINE_8,0,.5);assert(ai::classify_signal(down,box).direction==-1);
    assert(ai::automatic_signal_candidates(nullptr,{{0,0,80,80,.9f,2}},{},.15f).empty());
    assert(ai::automatic_signal_candidates(nullptr,{{0,0,80,80,std::nanf(""),7}},{},.15f).empty());
    { // The actual original-frame RGB565 -> ROI -> HSV/arrow -> fresh tracker association chain.
        std::vector<uint8_t> frame(1280*720*2,0);ai::Box location{500,100,580,180,.2f,7};
        cv::Mat roi(80,80,CV_8UC3,cv::Scalar(10,10,10));cv::arrowedLine(roi,{64,40},{16,40},{150,20,20},7,cv::LINE_8,0,.5);
        for(int y=0;y<80;++y)for(int x=0;x<80;++x){auto rgb=roi.at<cv::Vec3b>(y,x);
            unsigned pixel=(unsigned(rgb[0]>>3)<<11)|(unsigned(rgb[1]>>2)<<5)|(rgb[2]>>3);size_t at=((y+100)*1280+x+500)*2;
            frame[at]=uint8_t(pixel);frame[at+1]=uint8_t(pixel>>8);}
        ai::TrackedBox observed;observed.id=42;observed.box=location;
        auto values=ai::automatic_signal_candidates(frame.data(),{location},{observed},.15f);
        assert(values.size()==1&&values[0].track_id==42&&values[0].color=="RED"&&values[0].direction==0);
        auto rgb=ai::light_roi_rgb565(frame.data(),location);
        assert(ai::classify_light(rgb,location,100,120,160,.005).color==values[0].color);
        observed.predicted=true;values=ai::automatic_signal_candidates(frame.data(),{location},{observed},.15f);
        assert(values[0].track_id==0); // A cached/predicted box cannot renew physical signal presence.
    }
    uint32_t seed=17;
    for(int n=0;n<256;++n){cv::Mat mask(32,32,CV_8U);
        for(int y=0;y<32;++y)for(int x=0;x<32;++x){seed=seed*1664525u+1013904223u;mask.at<uint8_t>(y,x)=(seed>>24)<unsigned(n)?255:0;}
        assert(ai::signal_shape_scores(mask,true)==ai::signal_shape_scores(mask,false));}
    if(argc==2&&std::string(argv[1])=="--bench"){
        const auto component=ai::signal_templates()[0][0];volatile float sink=0;
        auto time=[&](bool packed){auto begin=std::chrono::steady_clock::now();for(int i=0;i<3000;++i)sink+=ai::signal_shape_scores(component,packed)[0];
            return std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count()/3000;};
        auto dense=time(false),packed=time(true);
        std::cout<<"{\"dense_us\":"<<dense<<",\"packed_us\":"<<packed<<",\"iterations\":3000,\"binary_score_parity_cases\":256,\"guard\":"<<sink<<"}\n";
    }
    std::cout<<"PASS oriented red/green arrow silhouettes, circle remains unassigned, mixed colors/down/blank rejected, non-light class ignored\n";
}
