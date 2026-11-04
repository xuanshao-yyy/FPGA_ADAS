#pragma once
#include "signal_observation.hpp"
#include "light_candidate.hpp"
#include "tracking.hpp"
#include <vector>
namespace ai {
inline cv::Mat normalized_signal_mask(const cv::Mat& mask){
    std::vector<cv::Point> points;cv::findNonZero(mask,points);if(points.empty())return {};
    cv::Mat out;cv::resize(mask(cv::boundingRect(points)),out,{32,32},0,0,cv::INTER_NEAREST);return out;
}
inline const std::array<std::vector<cv::Mat>,5>& signal_templates(){
    static const auto templates=[](){std::array<std::vector<cv::Mat>,5> all;
        for(int thickness:{3,5,7,9})for(double tip:{.5,.65}){
            cv::Mat left=cv::Mat::zeros(40,40,CV_8U);cv::arrowedLine(left,{34,20},{5,20},255,thickness,cv::LINE_8,0,tip);
            left=normalized_signal_mask(left);all[0].push_back(left);
            cv::Mat up,right,down;cv::rotate(left,up,cv::ROTATE_90_CLOCKWISE);cv::flip(left,right,1);cv::rotate(left,down,cv::ROTATE_90_COUNTERCLOCKWISE);
            all[1].push_back(up);all[2].push_back(right);all[3].push_back(down);
        }
        for(int thickness:{-1,3,6}){cv::Mat circle=cv::Mat::zeros(40,40,CV_8U);cv::circle(circle,{20,20},16,255,thickness);all[4].push_back(normalized_signal_mask(circle));}
        return all;
    }();return templates;
}
struct PackedSignalMask {
    std::array<uint64_t,16> words{};unsigned pixels=0;
};
inline PackedSignalMask pack_signal_mask(const cv::Mat& mask){
    if(mask.type()!=CV_8U||mask.rows!=32||mask.cols!=32)throw std::runtime_error("Signal matcher expects binary32x32");
    PackedSignalMask out;
    for(unsigned y=0;y<32;++y){uint32_t row=0;auto p=mask.ptr<uint8_t>(int(y));
        for(unsigned x=0;x<32;++x)row|=uint32_t(p[x]!=0)<<x;
        out.words[y/2]|=uint64_t(row)<<((y%2)*32);out.pixels+=unsigned(__builtin_popcount(row));}
    return out;
}
inline std::array<float,5> signal_shape_scores(const cv::Mat& component,bool packed=true){
    std::array<float,5> scores{};const auto& templates=signal_templates();
    if(packed){
        static const auto references=[](){std::array<std::vector<PackedSignalMask>,5> values;
            const auto& original=signal_templates();for(size_t i=0;i<original.size();++i)for(const auto& ref:original[i])values[i].push_back(pack_signal_mask(ref));
            return values;}();
        const auto input=pack_signal_mask(component);
        for(size_t i=0;i<references.size();++i)for(const auto& ref:references[i]){
            unsigned intersection=0;for(size_t k=0;k<input.words.size();++k)intersection+=unsigned(__builtin_popcountll(input.words[k]&ref.words[k]));
            float iou=float(intersection)/std::max(1u,input.pixels+ref.pixels-intersection);scores[i]=std::max(scores[i],iou);
        }
    }else for(size_t i=0;i<templates.size();++i)for(const auto& ref:templates[i]){
        cv::Mat both,either;cv::bitwise_and(component,ref,both);cv::bitwise_or(component,ref,either);
        float iou=float(cv::countNonZero(both))/std::max(1,cv::countNonZero(either));scores[i]=std::max(scores[i],iou);
    }
    return scores;
}
inline SignalCandidate classify_signal(const cv::Mat& rgb,const Box& box,bool packed=true){
    SignalCandidate out;out.box=box;
    // Match the validated manual path's area floor: isolated colored background
    // speckles must not turn an otherwise dominant lamp into MULTIPLE_COLORS.
    auto light=classify_light(rgb,box,100,120,160,.005);out.color=light.color;out.reason=light.reason;
    if(out.color=="UNKNOWN")return out;
    cv::Mat work=rgb,hsv;
    if(std::max(rgb.cols,rgb.rows)>128){double scale=128./std::max(rgb.cols,rgb.rows);cv::resize(rgb,work,{},scale,scale,cv::INTER_AREA);}
    cv::cvtColor(work,hsv,cv::COLOR_RGB2HSV);
    cv::Mat mask=cv::Mat::zeros(hsv.size(),CV_8U);
    for(int y=0;y<hsv.rows;++y)for(int x=0;x<hsv.cols;++x){auto p=hsv.at<cv::Vec3b>(y,x);
        if(p[1]<100||p[2]<120)continue;
        bool selected=out.color=="RED"?(p[0]<=10||p[0]>=160):out.color=="YELLOW"?(p[0]>=15&&p[0]<=35):(p[0]>=40&&p[0]<=95);
        if(selected)mask.at<uint8_t>(y,x)=255;
    }
    cv::morphologyEx(mask,mask,cv::MORPH_CLOSE,cv::getStructuringElement(cv::MORPH_RECT,{3,3}));
    cv::Mat labels,stats,centers;int count=cv::connectedComponentsWithStats(mask,labels,stats,centers,8,CV_32S),best=0;
    for(int i=1;i<count;++i)if(!best||stats.at<int>(i,cv::CC_STAT_AREA)>stats.at<int>(best,cv::CC_STAT_AREA))best=i;
    if(!best){out.reason="NO_PICTOGRAM_COMPONENT";return out;}
    int x=stats.at<int>(best,cv::CC_STAT_LEFT),y=stats.at<int>(best,cv::CC_STAT_TOP),w=stats.at<int>(best,cv::CC_STAT_WIDTH),h=stats.at<int>(best,cv::CC_STAT_HEIGHT),area=stats.at<int>(best,cv::CC_STAT_AREA);
    if(w<7||h<7||area<16||float(w)/h<.70f||float(w)/h>1.45f){out.reason="PICTOGRAM_SMALL_OR_INCOMPLETE";return out;}
    for(int i=1;i<count;++i)if(i!=best&&stats.at<int>(i,cv::CC_STAT_AREA)>=area*.4){out.reason="MULTIPLE_PICTOGRAM_COMPONENTS";return out;}
    auto component=normalized_signal_mask(labels(cv::Rect(x,y,w,h))==best);
    auto scores=signal_shape_scores(component,packed);
    int shape=int(std::max_element(scores.begin(),scores.end())-scores.begin());float second=0;
    for(int i=0;i<5;++i)if(i!=shape)second=std::max(second,scores[i]);
    out.shape_score=scores[shape];out.shape_margin=scores[shape]-second;
    if(out.shape_score<.55f||out.shape_margin<.08f){out.reason="PICTOGRAM_AMBIGUOUS";return out;}
    const char* names[]={"LEFT","STRAIGHT","RIGHT","DOWN","CIRCLE"};out.shape=names[shape];
    if(shape<3){out.direction=shape;out.reason="ARROW_GROUP_CANDIDATE_NOT_EGO_LANE_PROOF";}
    else out.reason=shape==4?"CIRCLE_LANE_ASSOCIATION_UNKNOWN":"DOWN_NOT_DRIVING_DIRECTION";
    return out;
}
inline std::vector<SignalCandidate> automatic_signal_candidates(const uint8_t* pixels,const std::vector<Box>& boxes,const std::vector<TrackedBox>& tracks,float threshold,bool packed=true){
    std::vector<SignalCandidate> result;
    for(const auto& box:boxes){if(box.category!=7||!std::isfinite(box.score)||box.score<threshold||result.size()>=32)continue;
        auto rgb=light_roi_rgb565(pixels,box);SignalCandidate item;item.box=box;
        if(rgb.empty())item.reason="INVALID_OR_OVERSIZE_ROI";else item=classify_signal(rgb,box,packed);
        float best=.4f;for(const auto& track:tracks)if(track.box.category==7&&!track.predicted){float iou=overlap(box,track.box);if(iou>best){best=iou;item.track_id=track.id;}}
        result.push_back(std::move(item));
    }return result;
}
}
