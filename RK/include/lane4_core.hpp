#pragma once
#include <rknn_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include "curve.hpp"
#include <chrono>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
namespace lane4 {
struct Config{
    std::vector<cv::Point2f> corners;
    double ego=.5,reference=.88,tolerance=6,min_span=70,alpha=.6,max_gap=.2,probability=.5;
    bool appearance=true;
    std::string temporal="ema";
    explicit Config(const std::string& path){
        cv::FileStorage file(path,cv::FileStorage::READ);if(!file.isOpened())throw std::runtime_error("Missing camera config");
        std::vector<double> values;file["roi"]>>values;if(values.size()!=8)throw std::runtime_error("ROI needs four normalized xy pairs");
        for(size_t i=0;i<8;i+=2){if(values[i]<0 || values[i]>1 || values[i+1]<0 || values[i+1]>1)throw std::runtime_error("ROI out of bounds");corners.emplace_back(values[i]*639,values[i+1]*359);}
        file["ego_x"]>>ego;file["reference_y"]>>reference;file["ransac_pixels"]>>tolerance;
        file["min_span_pixels"]>>min_span;file["ema_alpha"]>>alpha;file["max_gap_seconds"]>>max_gap;
        file["min_class_probability"]>>probability;int flag=0;file["appearance_styles"]>>flag;appearance=flag!=0;
        if(!file["temporal_filter"].empty())file["temporal_filter"]>>temporal;
        if(temporal!="ema" && temporal!="kalman" && temporal!="none")throw std::runtime_error("temporal_filter must be ema|kalman|none");
        std::vector<cv::Point> polygon;for(auto p:corners)polygon.emplace_back(cvRound(p.x),cvRound(p.y));
        if(!cv::isContourConvex(polygon) || std::abs(cv::contourArea(polygon))<1000 || ego<=0 || ego>=1 ||
           reference<=0 || reference>=1 || alpha<=0 || alpha>1 || max_gap<=0 || tolerance<=0 || min_span<=0 || probability<0 || probability>1)
            throw std::runtime_error("Invalid camera geometry or thresholds");
    }
    cv::Mat roi()const{
        cv::Mat mask=cv::Mat::zeros(360,640,CV_8U);std::vector<cv::Point> p;
        for(auto v:corners)p.emplace_back(cvRound(v.x),cvRound(v.y));
        cv::fillConvexPoly(mask,p,255);return mask;
    }
    cv::Mat homography()const{
        std::vector<cv::Point2f> destination{{128,0},{512,0},{512,359},{128,359}};
        return cv::getPerspectiveTransform(corners,destination);
    }
};
inline std::vector<Point> sliding(const cv::Mat& mask,bool right,double ego=.5){
    std::vector<double> histogram(mask.cols,0);
    for(int y=mask.rows*3/4;y<mask.rows;++y){auto row=mask.ptr<uint8_t>(y);for(int x=0;x<mask.cols;++x)histogram[x]+=row[x]!=0;}
    int lo=right?int(ego*mask.cols+20):int(.04*mask.cols);
    int hi=right?int(.96*mask.cols):int(ego*mask.cols-20);int base=-1;double best=0;
    for(int x=lo+6;x<hi-6;++x){double sum=0;for(int k=-6;k<=6;++k)sum+=histogram[x+k];
        // Prefer nearby evidence over distant road edges, without assuming an identity.
        double score=sum/(1+std::abs(x-ego*mask.cols)/180.);if(score>best){best=score;base=x;}}
    if(base<0 || best<8)return {};
    // Seed at the lowest visible strip, rather than the full-height histogram peak.
    // A sloped stripe's histogram peak can lie far above its observed lower end.
    for(int bottom=mask.rows-1;bottom>mask.rows/2;bottom-=8){
        std::vector<int> votes(mask.cols,0);
        for(int y=std::max(0,bottom-7);y<=bottom;++y){auto row=mask.ptr<uint8_t>(y);for(int x=lo;x<hi;++x)votes[x]+=row[x]!=0;}
        int chosen=-1;
        for(int x=lo;x<hi;){if(votes[x]==0){++x;continue;}int first=x,count=0;double weighted=0;
            while(x<hi && votes[x]){count+=votes[x];weighted+=x*votes[x];++x;}
            if(count>=6 && x-first<=80){int center=cvRound(weighted/count);if(chosen<0 || std::abs(center-ego*mask.cols)<std::abs(chosen-ego*mask.cols))chosen=center;}}
        if(chosen>=0){base=chosen;break;}
    }
    std::vector<Point> points;double center=base;
    for(int y=mask.rows-1;y>=0;y-=4){std::vector<int> xx;
        for(int yy=std::max(0,y-3);yy<=y;++yy){auto row=mask.ptr<uint8_t>(yy);
            for(int x=std::max(0,int(center)-24);x<std::min(mask.cols,int(center)+25);++x)if(row[x])xx.push_back(x);}
        if(xx.size()<2)continue;
        std::sort(xx.begin(),xx.end());center=xx[xx.size()/2];points.push_back({center,double(y)});
    }
    return points;
}
inline std::pair<Curve,Curve> curves(const cv::Mat& mask,const Config& cfg,bool ipm){
    cv::Mat selected=mask&cfg.roi(),work=selected,H;
    if(ipm){H=cfg.homography();cv::warpPerspective(selected,work,H,{640,360},cv::INTER_NEAREST);}
    std::array<Curve,2> fitted;
    for(int side=0;side<2;++side){auto points=sliding(work,side!=0,ipm?.5:cfg.ego);
        if(ipm){auto bird=fit_curve(points,cfg.tolerance,cfg.min_span);points.clear();
            if(bird.valid){std::vector<cv::Point2f> original,sampled;
                for(double y=bird.ymin;y<=bird.ymax;y+=4)sampled.emplace_back(bird.x(y),y);
                cv::perspectiveTransform(sampled,original,H.inv());
                for(auto p:original)if(p.x>=0 && p.x<640 && p.y>=0 && p.y<360)points.push_back({p.x,p.y});}}
        fitted[side]=fit_curve(points,cfg.tolerance,cfg.min_span);
    }
    return {fitted[0],fitted[1]};
}
struct SemanticVotes {int solid=0,dashed=0,style=0;};
inline SemanticVotes semantic_votes(const Curve& curve,const cv::Mat& classes){
    SemanticVotes out;if(!curve.valid)return out;
    for(int y=std::max(0,int(curve.ymin));y<=std::min(359,int(curve.ymax));++y){int x=cvRound(curve.x(y));
        for(int xx=std::max(0,x-4);xx<=std::min(639,x+4);++xx){int c=classes.at<uint8_t>(y,xx);out.solid+=c==1;out.dashed+=c==2;}}
    int n=out.solid+out.dashed;
    if(n>=40){if(out.solid>n*.7)out.style=1;else if(out.dashed>n*.7)out.style=2;}return out;
}
inline int semantic_style(const Curve& curve,const cv::Mat& classes){return semantic_votes(curve,classes).style;}
inline bool curve_blocked(const Curve& curve,const cv::Mat& blocked,double fraction){
    if(!curve.valid)return false;
    int count=0,total=0;
    for(int y=std::max(0,int(curve.ymin));y<=std::min(359,int(curve.ymax));++y){int x=cvRound(curve.x(y));
        if(x>=0 && x<640){++total;count+=blocked.at<uint8_t>(y,x)!=0;}}
    return total>0 && count>total*fraction;
}
// Inspect IEEE float exponent bits without scalar FP comparisons per element.
// All output elements, including letterbox padding, still get checked.
inline bool finite_output(const float* values,size_t count){
    size_t p=0;
#if defined(__aarch64__)
    const auto exponent=vdupq_n_u32(0x7f800000u);
    auto bad=vdupq_n_u32(0);
    for(;p+16<=count;p+=16){
        for(int k=0;k<16;k+=4){
            auto bits=vreinterpretq_u32_f32(vld1q_f32(values+p+k));
            bad=vorrq_u32(bad,vceqq_u32(vandq_u32(bits,exponent),exponent));
        }
    }
    if(vmaxvq_u32(bad))return false;
#endif
    for(;p<count;++p)if(!std::isfinite(values[p]))return false;
    return true;
}
// Float output remains owned by RKNN until Engine::release(). NCHW/identity
// crops are views: no full-output copy and no identity resize. Other layouts
// retain the baseline's split/resize semantics.
#if defined(__aarch64__)
inline cv::Mat decode_four_neon(const std::vector<cv::Mat>& scores,double minimum){
    cv::Mat labels=cv::Mat::zeros(360,640,CV_8U);
    const bool raw=minimum<=.25;
    cv::parallel_for_(cv::Range(0,360),[&](const cv::Range& range){
        for(int y=range.start;y<range.end;++y){
            std::array<const float*,4> rows{};
            for(int c=0;c<4;++c)rows[c]=scores[c].ptr<float>(y);
            auto* destination=labels.ptr<uint8_t>(y);
            for(int x=0;x<640;x+=8){
                float32x4_t maximum[2];uint32x4_t best[2];
                for(int block=0;block<2;++block){
                    int at=x+4*block;maximum[block]=vld1q_f32(rows[0]+at);best[block]=vdupq_n_u32(0);
                    for(int c=1;c<4;++c){
                        auto value=vld1q_f32(rows[c]+at);
                        auto wins=vcgtq_f32(value,maximum[block]);
                        maximum[block]=vbslq_f32(wins,value,maximum[block]);
                        best[block]=vbslq_u32(wins,vdupq_n_u32(c),best[block]);
                    }
                }
                // Strict > keeps the original first-channel tie rule.
                if(raw){
                    auto packed=vcombine_u16(vmovn_u32(best[0]),vmovn_u32(best[1]));
                    vst1_u8(destination+x,vmovn_u16(packed));continue;
                }
                if(!vmaxvq_u32(vorrq_u32(best[0],best[1])))continue;
                uint32_t ids[8];float maxima[8];
                for(int block=0;block<2;++block){vst1q_u32(ids+block*4,best[block]);vst1q_f32(maxima+block*4,maximum[block]);}
                for(int lane=0;lane<8;++lane){
                    if(!ids[lane])continue;
                    // Same scalar float exp and double accumulation as reference:
                    // no approximate exp, reciprocal or changed threshold.
                    double sum=0;
                    for(int c=0;c<4;++c)sum+=std::exp(rows[c][x+lane]-maxima[lane]);
                    if(1/sum>=minimum)destination[x+lane]=uint8_t(ids[lane]);
                }
            }
        }
    });
    return labels;
}
#endif
inline cv::Mat decode_seg_fast(const float* values,const rknn_tensor_attr& a,const std::string& kind,
                        int w,int h,const cv::Rect& content,double minimum){
    int channels=kind=="four"?4:kind=="ppseg"?19:2;
    if(!values || a.n_dims!=4 || a.dims[0]!=1 || a.n_elems!=unsigned(channels*h*w) ||
       content.width<=0 || content.height<=0 || (content&cv::Rect(0,0,w,h))!=content)
        throw std::runtime_error("Invalid decoder tensor or crop");
    bool first=a.dims[1]==unsigned(channels)&&a.dims[2]==unsigned(h)&&a.dims[3]==unsigned(w);
    bool last=a.dims[1]==unsigned(h)&&a.dims[2]==unsigned(w)&&a.dims[3]==unsigned(channels);
    std::vector<cv::Mat> planes,scores;
    if(first && (a.fmt!=RKNN_TENSOR_NHWC || !last)){
        for(int c=0;c<channels;++c)planes.emplace_back(h,w,CV_32F,const_cast<float*>(values+c*h*w));
    }else if(last && a.fmt==RKNN_TENSOR_NHWC){
        cv::Mat packed(h,w,CV_MAKETYPE(CV_32F,channels),const_cast<float*>(values));cv::split(packed,planes);
    }else throw std::runtime_error("Wrong decoder output dimensions/layout");
    for(auto plane:planes){
        cv::Mat restored;
        if(content.size()==cv::Size(640,360))restored=plane(content);
        else cv::resize(plane(content),restored,{640,360});
        scores.push_back(restored);
    }
#if defined(__aarch64__)
    if(kind=="four")return decode_four_neon(scores,minimum);
#endif
    cv::Mat labels=cv::Mat::zeros(360,640,CV_8U);
    cv::parallel_for_(cv::Range(0,360),[&](const cv::Range& range){
        for(int y=range.start;y<range.end;++y){
            std::array<const float*,19> rows{};for(int c=0;c<channels;++c)rows[c]=scores[c].ptr<float>(y);
            auto* destination=labels.ptr<uint8_t>(y);
            for(int x=0;x<640;++x){
                if(kind=="yolop"){destination[x]=rows[1][x]>.5;continue;}
                int best=0;float maximum=rows[0][x];
                for(int c=1;c<channels;++c)if(rows[c][x]>maximum){best=c;maximum=rows[c][x];}
                if(kind=="ppseg"){destination[x]=uint8_t(best);continue;}
                // Background is zero whether confidence passes or fails. In raw
                // mode (minimum=0) softmax is unnecessary for every class.
                if(best==0)continue;
                if(minimum<=1./channels){destination[x]=uint8_t(best);continue;}
                double sum=0;
                // Preserve baseline channel order, float exp and tie handling.
                for(int c=0;c<channels;++c)sum+=std::exp(rows[c][x]-maximum);
                if(1/sum>=minimum)destination[x]=uint8_t(best);
            }
        }
    });
    return labels;
}

}
