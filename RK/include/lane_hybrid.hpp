#pragma once
#include "lane4_post.hpp"
#include "lane_position.hpp"
#include "protocol.hpp"
#include <opencv2/video/tracking.hpp>
#include <opencv2/calib3d.hpp>

namespace ai {
struct HybridUpdate {
    LaneMask mask;
    bool accepted=false;
    const char* reason="NO_MODEL_ANCHOR";
    int features=0,inliers=0;
    const char* motion="NONE";
    double flow_ms=0,fit_ms=0,semantic_age_ms=0;
};
class HybridLane {
    lane4::Config config;
    cv::Mat previous_gray,previous_blocked,classes;
    LaneMask anchor;
    Curve left,right;
    double previous_time=-1;
    unsigned semantic_ms;
    bool valid=false;
    static Curve project(const Curve& curve,const cv::Mat& H,const cv::Mat& gray,const cv::Mat& blocked){
        if(!curve.valid)return {};
        std::vector<cv::Point2f> original,warped;
        for(double y=curve.ymin;y<=curve.ymax;y+=4)original.emplace_back(curve.x(y),y);
        cv::perspectiveTransform(original,warped,H);
        cv::Mat grad;cv::Sobel(gray,grad,CV_16S,1,0,3);
        std::vector<Point> points;
        for(auto p:warped){
            if(p.x<8||p.x>=632||p.y<8||p.y>=352)continue;
            int yy=cvRound(p.y/2),base=cvRound(p.x/2),chosen=-1,best=24;
            if(yy>=gray.rows)continue;
            for(int xx=std::max(1,base-3);xx<=std::min(gray.cols-2,base+3);++xx){
                int value=std::abs(grad.at<short>(yy,xx));
                if(value>best && !blocked.at<uint8_t>(cvRound(p.y),xx*2)){best=value;chosen=xx;}
            }
            if(chosen>=0)points.push_back({double(chosen*2),double(p.y)});
        }
        return fit_curve(points,6,70);
    }
public:
    HybridLane(const std::string& camera,float ego,unsigned max_semantic_ms):config(camera),semantic_ms(max_semantic_ms){config.ego=ego;}
    void seed(const cv::Mat& gray,const LaneMask& mask){
        if(gray.size()!=cv::Size(320,180)||gray.type()!=CV_8U)throw std::runtime_error("Hybrid seed gray contract");
        anchor=mask;left=mask.left_curve;right=mask.right_curve;previous_time=mask.received;
        valid=mask.geometry.valid && mask.native_classes.size()==640*360 && mask.native_blocked.size()==640*360;
        previous_gray=gray.clone();
        if(valid){
            classes=cv::Mat(360,640,CV_8U,const_cast<uint8_t*>(mask.native_classes.data())).clone();
            cv::resize(cv::Mat(360,640,CV_8U,const_cast<uint8_t*>(mask.native_blocked.data())),previous_blocked,{320,180},0,0,cv::INTER_NEAREST);
        }
    }
    HybridUpdate update(const cv::Mat& gray,const cv::Mat& blocked,double timestamp){
        HybridUpdate out;out.mask=anchor;out.semantic_age_ms=(timestamp-anchor.received)*1000;
        double begin=0;unsigned phase=0;
        auto fail=[&](const char* reason){out.reason=reason;valid=false;
            if(phase==1)out.flow_ms=(qv::now_seconds()-begin)*1000;
            if(phase==2)out.fit_ms=(qv::now_seconds()-begin)*1000;
            return out;};
        if(gray.size()!=cv::Size(320,180)||gray.type()!=CV_8U||blocked.size()!=cv::Size(640,360)||blocked.type()!=CV_8U)
            throw std::runtime_error("Hybrid frame contract");
        if(!valid)return fail("NO_VALID_MODEL_PAIR");
        if(timestamp<=previous_time)return fail("NON_FORWARD_TIME");
        if(out.semantic_age_ms<0||out.semantic_age_ms>semantic_ms)return fail("SEMANTIC_EXPIRED");
        if(timestamp-previous_time>.35)return fail("FRAME_GAP");
        begin=qv::now_seconds();phase=1;
        // Only road ROI outside vehicles contributes to the road-plane motion.
        cv::Mat roi,now_blocked;cv::resize(config.roi(),roi,{320,180},0,0,cv::INTER_NEAREST);
        cv::resize(blocked,now_blocked,{320,180},0,0,cv::INTER_NEAREST);
        roi.setTo(0,previous_blocked);std::vector<cv::Point2f> p,next,back;
        cv::goodFeaturesToTrack(previous_gray,p,100,.015,7,roi);
        out.features=int(p.size());if(p.size()<16)return fail("LOW_FEATURES");
        std::vector<uint8_t> forward_ok,back_ok;std::vector<float> error,back_error;
        cv::calcOpticalFlowPyrLK(previous_gray,gray,p,next,forward_ok,error,{15,15},2);
        cv::calcOpticalFlowPyrLK(gray,previous_gray,next,back,back_ok,back_error,{15,15},2);
        std::vector<cv::Point2f> a,b;
        for(size_t i=0;i<p.size();++i){
            auto v=next[i];
            if(!forward_ok[i]||!back_ok[i]||error[i]>25||cv::norm(p[i]-back[i])>1.2||cv::norm(v-p[i])>24||
               v.x<2||v.y<2||v.x>=318||v.y>=178||!roi.at<uint8_t>(cvRound(v.y),cvRound(v.x))||now_blocked.at<uint8_t>(cvRound(v.y),cvRound(v.x)))continue;
            a.push_back(p[i]);b.push_back(v);
        }
        if(a.size()<16)return fail("FLOW_REJECTED");
        cv::Mat inlier_mask;auto h=cv::findHomography(a,b,cv::RANSAC,1.5,inlier_mask,120,.99);
        auto supported=[&](const cv::Mat& transform){
            out.inliers=transform.empty()?0:cv::countNonZero(inlier_mask);
            if(transform.empty()||!cv::checkRange(transform)||out.inliers<12||out.inliers<a.size()*.65)return false;
            // A straight stripe alone cannot constrain road-plane motion.
            std::vector<cv::Point2f> support,hull;
            for(size_t i=0;i<a.size();++i)if(inlier_mask.at<uint8_t>(int(i)))support.push_back(a[i]);
            cv::convexHull(support,hull);if(std::abs(cv::contourArea(hull))<1800)return false;
            std::vector<cv::Point2f> probes,moved;
            for(auto c:{left,right})for(double y=c.ymin;y<=c.ymax;y+=20)probes.emplace_back(c.x(y)/2,y/2);
            cv::perspectiveTransform(probes,moved,transform);
            for(size_t i=0;i<probes.size();++i)if(!std::isfinite(moved[i].x)||!std::isfinite(moved[i].y)||cv::norm(probes[i]-moved[i])>24)return false;
            return true;
        };
        out.motion="HOMOGRAPHY";
        if(!supported(h)){
            // Fewer degrees of freedom when a projective estimate is unstable.
            auto affine=cv::estimateAffinePartial2D(a,b,inlier_mask,cv::RANSAC,1.5,120,.99,10);
            h=cv::Mat::eye(3,3,CV_64F);if(!affine.empty())affine.copyTo(h(cv::Rect(0,0,3,2)));else h.release();
            if(!supported(h))return fail("MOTION_REJECTED");
            out.motion="AFFINE_PARTIAL";
        }
        out.flow_ms=(qv::now_seconds()-begin)*1000;begin=qv::now_seconds();phase=2;
        cv::Mat S=(cv::Mat_<double>(3,3)<<2,0,0,0,2,0,0,0,1);cv::Mat H=S*h*S.inv();
        auto l=project(left,H,gray,blocked),r=project(right,H,gray,blocked);
        if(!l.valid||!r.valid)return fail("NO_CURRENT_EDGE_SUPPORT");
        double lo=std::max(l.ymin,r.ymin),hi=std::min(l.ymax,r.ymax);
        if(hi-lo<40)return fail("PAIR_SUPPORT_REJECTED");
        for(double y=lo;y<=hi;y+=5)if(l.x(y)+15>=r.x(y))return fail("PAIR_CROSSING");
        out.mask=anchor;out.mask.cpu_propagated=true;
        out.mask.left_curve=l;out.mask.right_curve=r;
        out.mask.inputs_ms=out.mask.run_ms=out.mask.get_ms=0;
        out.mask.crosswalk_pixels=0; // CPU motion is not a new crosswalk observation.
        cv::Mat warped;cv::warpPerspective(classes,warped,H,{640,360},cv::INTER_NEAREST);
        warped.setTo(0,warped==3);warped.setTo(0,blocked);
        out.mask.native_classes.assign(warped.data,warped.data+warped.total());
        out.mask.native_blocked.assign(blocked.data,blocked.data+blocked.total());
        cv::Mat display,display_blocked;cv::resize(warped,display,{LaneWidth,LaneHeight},0,0,cv::INTER_NEAREST);
        cv::resize(blocked,display_blocked,display.size(),0,0,cv::INTER_NEAREST);
        out.mask.data.assign(display.data,display.data+display.total());out.mask.pixels=cv::countNonZero(display);
        out.mask.blocked.assign(display_blocked.data,display_blocked.data+display_blocked.total());
        out.mask.blocked_pixels=cv::countNonZero(blocked);
        lane_position(out.mask,config.ego*640,config.reference*360);
        // Current edge support can update the drawing even when the pair does
        // not cover the configured reference row. In that case position stays
        // UNKNOWN and the worker requests a new model observation.
        double offset=0;out.mask.geometry.valid=pair_geometry(l,r,config.ego*640,config.reference*360,offset);
        out.mask.geometry.offset=float(offset);out.mask.geometry.state=out.mask.position;
        out.mask.pair_reason="CPU_TRACKED_MODEL_PAIR";
        // Styles and confirmation streaks remain model evidence; never accumulate CPU votes.
        out.mask.left_evidence.reason=out.mask.right_evidence.reason="MODEL_STYLE_CARRIED";
        classes=warped;left=l;right=r;previous_gray=gray.clone();previous_blocked=now_blocked;previous_time=timestamp;
        out.fit_ms=(qv::now_seconds()-begin)*1000;
        out.accepted=true;out.reason=out.mask.position_valid?"TRACKED":"TRACKED_POSITION_UNKNOWN";return out;
    }
};
}
