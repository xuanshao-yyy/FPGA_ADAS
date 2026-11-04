#pragma once
#include "lane4_core.hpp"
#include "lane.hpp"
#include "tracking.hpp"
namespace lane4 {
inline cv::Mat vehicle_mask(const std::vector<ai::Box>& boxes,const std::vector<ai::TrackedBox>& tracks,float high=.25f){
    cv::Mat mask=cv::Mat::zeros(360,640,CV_8U);
    auto add=[&](const ai::Box& b,bool confirmed){
        if(b.category<2 || b.category>6 || (!confirmed && b.score<high) || !std::isfinite(b.score) || !std::isfinite(b.x1)||!std::isfinite(b.x2)||!std::isfinite(b.y1)||!std::isfinite(b.y2)||b.x2<=b.x1||b.y2<=b.y1)return;
        int x1=std::clamp(int(std::floor(b.x1*.5))-2,0,640),x2=std::clamp(int(std::ceil(b.x2*.5))+2,0,640);
        int y1=std::clamp(int(std::floor(b.y1*.5))-2,0,360),y2=std::clamp(int(std::ceil(b.y2*.5))+2,0,360);
        if(x2>x1 && y2>y1)mask(cv::Rect(x1,y1,x2-x1,y2-y1)).setTo(255);
    };
    for(const auto& b:boxes)add(b,false);
    for(const auto& t:tracks)if(!t.predicted)add(t.box,true);
    return mask;
}
class Post {
    Config cfg;
    std::array<StableCurve,4> left_state,right_state;
public:
    Post(const std::string& camera,float ego):cfg(camera){cfg.ego=ego;}
    void reset(unsigned channel){left_state.at(channel)={};right_state.at(channel)={};}
    ai::LaneMask process(const cv::Mat& classes,const cv::Mat& blocked,double timestamp,unsigned channel,bool geometry){
        if(classes.type()!=CV_8U || classes.size()!=cv::Size(640,360) || blocked.type()!=CV_8U || blocked.size()!=classes.size() || channel>=4)
            throw std::runtime_error("Invalid live lane postprocessing input");
        ai::LaneMask out;out.four_class=true;out.geometry_enabled=geometry;
        out.native_classes.assign(classes.data,classes.data+classes.total());
        out.native_blocked.assign(blocked.data,blocked.data+blocked.total());
        cv::Mat visible=classes.clone();visible.setTo(0,blocked);
        out.blocked_pixels=cv::countNonZero(blocked);out.crosswalk_pixels=cv::countNonZero(visible==3);
        if(geometry && timestamp>=0){
            cv::Mat mask=(visible==1)|(visible==2);
            auto pair=curves(mask,cfg,false);
            auto update=[&](StableCurve& state,Curve curve){
                double dt=state.last_time<0?.1:std::max(0.,timestamp-state.last_time);
                double alpha=1-std::pow(1-cfg.alpha,dt/.1);
                return state.update(curve,timestamp,std::clamp(alpha,.01,1.),cfg.max_gap,cfg.temporal);
            };
            out.left_curve=update(left_state[channel],pair.first);out.right_curve=update(right_state[channel],pair.second);
            auto style=[&](const Curve& curve,StableCurve& state,ai::LaneMask::StyleEvidence& evidence){
                auto votes=semantic_votes(curve,visible);int candidate=votes.style;
                evidence.solid_votes=votes.solid;evidence.dashed_votes=votes.dashed;
                const bool occluded=curve_blocked(curve,blocked,.2);if(occluded)candidate=0;
                evidence.raw_style=candidate;int result=state.stable_style(candidate);evidence.confirmation_streak=state.streak;
                // Low-rate NPU scheduling must not force three redundant observations
                // of an already clear, unoccluded model class. Mixed/thin evidence
                // retains temporal confirmation and never gains a class from colour.
                int count=votes.solid+votes.dashed;
                evidence.direct=candidate!=0&&count>=120&&std::max(votes.solid,votes.dashed)>=count*.9;
                if(evidence.direct)result=candidate;
                evidence.reason=!curve.valid?"NO_CURVE":occluded?"VEHICLE_OCCLUSION":votes.solid+votes.dashed<40?"LOW_SEMANTIC_VOTES":votes.style==0?"MIXED_SEMANTICS":result==0?"TEMPORAL_CONFIRMATION":"CONFIRMED_CANDIDATE";
                return result;
            };
            out.left_style=style(out.left_curve,left_state[channel],out.left_evidence);
            out.right_style=style(out.right_curve,right_state[channel],out.right_evidence);
            double offset=0;
            bool valid=pair_geometry(out.left_curve,out.right_curve,cfg.ego*640,cfg.reference*360,offset);
            out.pair_reason=valid?"PAIR_CANDIDATE":!out.left_curve.valid || !out.right_curve.valid?"MISSING_BOUNDARY":
                cfg.reference*360<out.left_curve.ymin || cfg.reference*360>out.left_curve.ymax || cfg.reference*360<out.right_curve.ymin || cfg.reference*360>out.right_curve.ymax?"REFERENCE_OUTSIDE_FIT":"GEOMETRY_REJECTED";
            if(valid){int y=std::clamp(cvRound(cfg.reference*360),0,359);
                int l=std::clamp(cvRound(out.left_curve.x(y)),0,639),r=std::clamp(cvRound(out.right_curve.x(y)),0,639);
                if(blocked.at<uint8_t>(y,l)||blocked.at<uint8_t>(y,r)){valid=false;out.pair_reason="REFERENCE_OCCLUDED";}
            }
            out.geometry.valid=valid;out.geometry.ego_x=float(cfg.ego);out.geometry.offset=float(offset);
            out.geometry.state=valid?(std::abs(offset)>.65?"DEPARTURE CANDIDATE":"PAIR CANDIDATE"):"UNKNOWN";
        }
        cv::Mat display,display_blocked;
        cv::resize(visible,display,{ai::LaneWidth,ai::LaneHeight},0,0,cv::INTER_NEAREST);
        cv::resize(blocked,display_blocked,display.size(),0,0,cv::INTER_NEAREST);
        out.data.assign(display.data,display.data+display.total());out.pixels=cv::countNonZero(display);
        out.blocked.assign(display_blocked.data,display_blocked.data+display_blocked.total());
        return out;
    }
};
}
