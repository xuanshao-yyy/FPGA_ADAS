#include "lane_hybrid.hpp"
#include <cassert>
#include <iostream>
using namespace ai;
LaneMask anchor(){
    LaneMask m;m.received=m.model_received=10;m.frame_id=m.model_frame_id=1;m.four_class=true;m.geometry_enabled=true;
    m.left_curve.valid=m.right_curve.valid=true;m.left_curve.coef={0,0,160};m.right_curve.coef={0,0,480};
    m.left_curve.ymin=m.right_curve.ymin=160;m.left_curve.ymax=m.right_curve.ymax=350;
    m.geometry.valid=true;m.left_style=1;m.right_style=2;m.left_evidence.confirmation_streak=m.right_evidence.confirmation_streak=3;
    cv::Mat labels=cv::Mat::zeros(360,640,CV_8U);
    cv::line(labels,{160,160},{160,350},1,5);cv::line(labels,{480,160},{480,350},2,5);
    cv::line(labels,{200,250},{420,250},3,3);
    m.native_classes.assign(labels.data,labels.data+labels.total());m.native_blocked.assign(640*360,0);
    return m;
}
int main(){
    cv::setNumThreads(1);cv::setRNGSeed(17);
    auto model=anchor();lane_position(model,320,316.8);assert(model.position_valid && std::string(model.position)=="CENTERED");
    lane_position(model,210,316.8);assert(std::string(model.position)=="NEAR_LEFT_BOUNDARY");
    lane_position(model,490,316.8);assert(std::string(model.position)=="CROSS_RIGHT_CANDIDATE");
    model.right_curve.valid=false;lane_position(model,320,316.8);assert(!model.position_valid);
    model=anchor();model.right_curve.ymax=300;lane_position(model,320,316.8);assert(!model.position_valid);
    model=anchor();cv::Mat gray(180,320,CV_8U);cv::randu(gray,40,90);
    cv::line(gray,{80,80},{80,175},220,3);cv::line(gray,{240,80},{240,175},220,3);
    HybridLane tracker("camera_live.json",.5f,750);tracker.seed(gray,model);
    cv::Mat moved,blocked=cv::Mat::zeros(360,640,CV_8U);
    cv::Mat shift=(cv::Mat_<double>(2,3)<<1,0,2,0,1,0);cv::warpAffine(gray,moved,shift,gray.size(),cv::INTER_LINEAR,cv::BORDER_CONSTANT,50);
    auto update=tracker.update(moved,blocked,10.083333);
    std::cout<<"translation: "<<update.reason<<" features="<<update.features<<" inliers="<<update.inliers<<" offset="<<update.mask.relative_offset<<'\n';
    assert(update.accepted && update.mask.cpu_propagated && update.mask.position_valid);
    assert(std::abs(update.mask.left_curve.x(316)-164)<8 && std::abs(update.mask.right_curve.x(316)-484)<8);
    assert(update.mask.left_evidence.confirmation_streak==3 && update.mask.model_frame_id==1);
    assert(update.mask.crosswalk_pixels==0);
    for(auto c:update.mask.native_classes)assert(c!=3);
    tracker.seed(gray,model);assert(!tracker.update(moved,blocked,11).accepted);
    tracker.seed(gray,model);assert(!tracker.update(moved,blocked,10).accepted);
    tracker.seed(gray,model);assert(!tracker.update(moved,blocked,10.4).accepted);
    tracker.seed(gray,model);auto blackout=tracker.update(cv::Mat::zeros(180,320,CV_8U),blocked,10.083333);assert(!blackout.accepted);
    tracker.seed(gray,model);blocked.setTo(255);assert(!tracker.update(moved,blocked,10.083333).accepted);
    std::cout<<"PASS current-frame motion, position direction, no fresh semantic votes/crosswalk, expiry, gap, black frame and occlusion rejection\n";
}
