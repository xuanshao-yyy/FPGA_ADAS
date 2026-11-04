#pragma once
// Pixel geometry only. These candidates are not calibrated vehicle positions.
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <string>
#include <vector>

struct Point { double x,y; };
struct Curve {
    bool valid=false;
    std::array<double,3> coef{};
    double ymin=0,ymax=0,inlier_ratio=0;
    double x(double y) const { double t=y/360.; return coef[0]*t*t+coef[1]*t+coef[2]; }
};
inline bool solve3(double a[3][4],std::array<double,3>& answer) {
    for(int i=0;i<3;++i) {
        int pivot=i; for(int j=i+1;j<3;++j) if(std::abs(a[j][i])>std::abs(a[pivot][i])) pivot=j;
        if(std::abs(a[pivot][i])<1e-9) return false;
        for(int k=i;k<4;++k) std::swap(a[i][k],a[pivot][k]);
        double divisor=a[i][i]; for(int k=i;k<4;++k) a[i][k]/=divisor;
        for(int j=0;j<3;++j) if(j!=i) { double v=a[j][i];for(int k=i;k<4;++k)a[j][k]-=v*a[i][k]; }
    }
    for(int i=0;i<3;++i)answer[i]=a[i][3];
    return true;
}
inline bool least_squares(const std::vector<Point>& points,std::array<double,3>& coef) {
    double a[3][4]{};
    for(auto p:points) {
        double t=p.y/360.;double v[3]={t*t,t,1};
        for(int i=0;i<3;++i) {for(int j=0;j<3;++j)a[i][j]+=v[i]*v[j];a[i][3]+=v[i]*p.x;}
    }
    return solve3(a,coef);
}
inline Curve fit_curve(const std::vector<Point>& points,double tolerance=6.,double min_span=70.) {
    Curve result;
    if(points.size()<12)return result;
    std::mt19937 rng(42);std::uniform_int_distribution<size_t> pick(0,points.size()-1);
    std::vector<Point> best;
    for(int i=0;i<96;++i) {
        std::vector<Point> sample{points[pick(rng)],points[pick(rng)],points[pick(rng)]};
        std::array<double,3> coef{};if(!least_squares(sample,coef))continue;
        Curve candidate;candidate.coef=coef;std::vector<Point> inliers;
        for(auto p:points)if(std::abs(p.x-candidate.x(p.y))<=tolerance)inliers.push_back(p);
        if(inliers.size()>best.size())best=std::move(inliers);
    }
    if(best.size()<12 || best.size()<points.size()*.45 || !least_squares(best,result.coef))return result;
    result.ymin=360;result.ymax=0;
    for(auto p:best){result.ymin=std::min(result.ymin,p.y);result.ymax=std::max(result.ymax,p.y);}
    if(result.ymax-result.ymin<min_span)return Curve{};
    for(double y=result.ymin;y<=result.ymax;y+=5)if(result.x(y)<0 || result.x(y)>=640)return Curve{};
    result.valid=true;result.inlier_ratio=double(best.size())/points.size();return result;
}
struct StableCurve {
    Curve previous; double last_time=-1;int last_style=0,streak=0;
    struct Kalman {
        double position=0,velocity=0,p00=100,p01=0,p10=0,p11=100;
        void reset(double value){position=value;velocity=0;p00=100;p01=0;p10=0;p11=100;}
        double update(double measurement,double dt){
            position+=velocity*dt;
            double a=p00+dt*(p01+p10)+dt*dt*p11+5*dt*dt;
            double b=p01+dt*p11,c=p10+dt*p11,d=p11+5*dt;
            double k0=a/(a+25),k1=c/(a+25),innovation=measurement-position;
            position+=k0*innovation;velocity+=k1*innovation;
            p00=(1-k0)*a;p01=(1-k0)*b;p10=c-k1*a;p11=d-k1*b;return position;
        }
    };
    std::array<Kalman,3> kalman;
    Curve update(Curve now,double timestamp,double alpha=.6,double max_gap=.2,const std::string& method="ema") {
        if(!now.valid){previous=Curve{};last_style=0;streak=0;last_time=timestamp;return now;}
        bool smooth=false;
        if(previous.valid && timestamp>last_time && timestamp-last_time<=max_gap) {
            double lo=std::max(now.ymin,previous.ymin),hi=std::min(now.ymax,previous.ymax);
            double jump=0;
            for(int i=0;i<5;++i){double y=lo+(hi-lo)*i/4.;jump=std::max(jump,std::abs(now.x(y)-previous.x(y)));}
            if(hi-lo>40 && jump<45)smooth=true;
            else {last_style=0;streak=0;}
        } else {last_style=0;streak=0;}
        for(int i=0;i<3;++i){
            if(!smooth)kalman[i].reset(now.coef[i]);
            else if(method=="ema")now.coef[i]=alpha*now.coef[i]+(1-alpha)*previous.coef[i];
            else if(method=="kalman")now.coef[i]=kalman[i].update(now.coef[i],timestamp-last_time);
        }
        previous=now;last_time=timestamp;return now;
    }
    int stable_style(int candidate) {
        if(candidate==0){last_style=0;streak=0;return 0;}
        streak=candidate==last_style?streak+1:1;last_style=candidate;return streak>=3?candidate:0;
    }
};
inline bool pair_geometry(const Curve& left,const Curve& right,double ego_x,double reference_y,double& offset) {
    if(!left.valid || !right.valid || reference_y<left.ymin || reference_y>left.ymax ||
       reference_y<right.ymin || reference_y>right.ymax)return false;
    double l=left.x(reference_y),r=right.x(reference_y),width=r-l;
    if(!(l<ego_x && ego_x<r && width>76.8 && width<576))return false;
    double lo=std::max(left.ymin,right.ymin),hi=std::min(left.ymax,right.ymax);
    for(double y=lo;y<=hi;y+=5)if(left.x(y)+15>=right.x(y))return false;
    offset=(ego_x-(l+r)/2)/(width/2);return std::isfinite(offset);
}
inline int appearance_style(const std::vector<bool>& occupied) {
    // Raw paint evidence, not gaps in the fitted/model curve. No occlusion inference.
    if(occupied.size()<80)return 0;
    int on=0,long_gaps=0,runs=0,gap=0,max_gap=0;bool before=false;
    for(bool value:occupied){
        if(value){++on;if(!before)++runs;if(gap>=8)++long_gaps;max_gap=std::max(max_gap,gap);gap=0;}
        else ++gap;
        before=value;
    }
    // Trailing gap cannot establish a repeated dashed pattern.
    double fraction=double(on)/occupied.size();
    if(fraction>.86 && max_gap<8)return 1;
    if(fraction>.15 && fraction<.70 && long_gaps>=2 && runs>=3)return 2;
    return 0;
}
