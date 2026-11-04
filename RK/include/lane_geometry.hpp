#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace ai {
struct LaneBoundary {
    bool valid=false;
    float slope=0,intercept=0,support=0;
    float x(float y) const{return slope*y+intercept;}
};
struct LaneGeometry {
    LaneBoundary left,right;
    bool valid=false,confirmed=false;
    float offset=0,ego_x=.5f;
    const char* state="UNKNOWN";
};
// A local straight-line candidate in the lower road ROI. No metric calibration,
// marking-style classification, road semantics or safe-change authorization.
inline LaneGeometry extract_lane_geometry(const std::vector<uint8_t>& mask,int w,int h,float ego=.5f){
    if(w<32||h<32||mask.size()!=size_t(w*h))throw std::runtime_error("lane geometry mask dimensions");
    if(!std::isfinite(ego)||ego<=0||ego>=1)throw std::runtime_error("ego reference must be in (0,1)");
    struct Point {float x,y;int row;};std::vector<Point> points;
    int y0=int(h*.5f),y1=int(h*.94f),rows=0;
    for(int y=y0;y<=y1;y+=4,++rows){
        for(int x=0;x<w;){
            if(!mask[size_t(y*w+x)]){++x;continue;}
            int start=x;while(x<w && mask[size_t(y*w+x)])++x;
            int length=x-start;
            // Broad horizontal paint and blobs should not vote as lane boundaries.
            if(length>=2 && length<=std::max(10,w/30))points.push_back({(start+x-1)*.5f,float(y),rows});
        }
    }
    LaneGeometry out;out.ego_x=ego;
    if(points.empty())return out;
    auto fit=[&](bool left){
        LaneBoundary best;float best_score=0;
        const float reference=float(y1),axis=ego*w;
        const int bins=w/4+1;
        for(int s=-45;s<=45;++s){
            float slope=s*.04f;
            if((left && slope>.12f)||(!left && slope<-.12f))continue;
            std::vector<int> votes(size_t(bins),0);
            for(const auto& p:points){
                float bottom=p.x+slope*(reference-p.y);
                if(bottom<0||bottom>=w||(left?bottom>=axis-12:bottom<=axis+12))continue;
                ++votes[size_t(std::clamp(int(bottom/4),0,bins-1))];
            }
            int bin=int(std::max_element(votes.begin(),votes.end())-votes.begin());
            if(votes[size_t(bin)]<8)continue;
            float bottom=bin*4+2.f;
            std::vector<Point> inliers;
            std::vector<bool> seen(size_t(rows),false);
            for(const auto& p:points)if(std::abs(p.x-(bottom+slope*(p.y-reference)))<=5){
                // One point per sampled row: multiple stripes cannot inflate support.
                if(!seen[size_t(p.row)]){inliers.push_back(p);seen[size_t(p.row)]=true;}
            }
            if(inliers.size()<size_t(std::max(12,int(rows*.45f))))continue;
            if(inliers.back().y-inliers.front().y<h*.28f)continue;
            double sy=0,sx=0,syy=0,syx=0,n=double(inliers.size());
            for(const auto& p:inliers){sy+=p.y;sx+=p.x;syy+=p.y*p.y;syx+=p.y*p.x;}
            double denominator=n*syy-sy*sy;if(denominator<=0)continue;
            float a=float((n*syx-sy*sx)/denominator),b=float((sx-a*sy)/n);
            float xb=a*reference+b,xt=a*y0+b;
            if(xb<0||xb>=w||xt<0||xt>=w||(left?xb>=axis-12:xb<=axis+12))continue;
            // Boundaries should converge, rather than diverge, toward the upper road.
            if(left?xt<xb-12:xt>xb+12)continue;
            double error=0;for(const auto& p:inliers)error+=std::abs(p.x-(a*p.y+b));
            if(error/n>4)continue;
            float support=float(n/rows),score=support-.08f*std::abs(xb-axis)/w;
            if(score>best_score){best_score=score;best={true,a,b,support};}
        }
        return best;
    };
    out.left=fit(true);out.right=fit(false);
    if(!out.left.valid||!out.right.valid)return out;
    float l=out.left.x(float(y1)),r=out.right.x(float(y1)),width=r-l;
    if(width<w*.22f||width>w*.9f||out.left.x(float(y0))+4>=out.right.x(float(y0)))return out;
    out.valid=true;out.offset=(ego*w-(l+r)*.5f)/(width*.5f);out.state="CHECKING";return out;
}
class LaneGeometryFilter {
    LaneGeometry previous_;
    double stamp_=0;
    unsigned count_=0;
public:
    LaneGeometry update(LaneGeometry g,double stamp,int h,int w=576){
        bool reset=!g.valid||!previous_.valid||stamp<=stamp_||stamp-stamp_>2.5;
        if(!reset){
            float y=h*.94f;
            reset=std::abs(g.left.x(y)-previous_.left.x(y))>w*.15f || std::abs(g.right.x(y)-previous_.right.x(y))>w*.15f;
        }
        if(reset)count_=0;
        if(g.valid){
            if(!reset){
                auto blend=[](LaneBoundary b,const LaneBoundary& old){
                    b.slope=.65f*b.slope+.35f*old.slope;b.intercept=.65f*b.intercept+.35f*old.intercept;return b;};
                g.left=blend(g.left,previous_.left);g.right=blend(g.right,previous_.right);
            }
            float y=h*.94f,l=g.left.x(y),r=g.right.x(y);
            g.offset=(g.ego_x*w-(l+r)*.5f)/((r-l)*.5f);
            g.confirmed=++count_>=2;
            g.state=!g.confirmed?"CHECKING":std::abs(g.offset)<=.45f?"CENTER CANDIDATE":g.offset>0?"OFFSET RIGHT":"OFFSET LEFT";
        }
        previous_=g;stamp_=stamp;return g;
    }
};
}
