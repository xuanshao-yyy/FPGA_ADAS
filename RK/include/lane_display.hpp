#pragma once
#include "curve.hpp"
#include "lane_rules.hpp"
namespace lane_demo {
// View-only curves. Three x samples avoid filtering badly scaled polynomial coefficients.
class CurveView {
    Curve last;
    std::array<OffsetFilter,3> filters;
    double stamp=-1,hold_ms;
public:
    explicit CurveView(double hold=1000):hold_ms(hold){}
    void reset(){last={};stamp=-1;for(auto& f:filters)f.reset();}
    void observe(const Curve& curve,double received){
        if(received<=stamp||!curve.valid)return;
        constexpr double rows[3]={180,270,350};
        double jump=0;for(double y:rows){if(!std::isfinite(curve.x(y)))return;if(last.valid)jump=std::max(jump,std::abs(curve.x(y)-last.x(y)));}
        if(!last.valid||received-stamp>1||jump>45)for(auto& f:filters)f.reset();
        for(unsigned i=0;i<3;++i)filters[i].update(curve.x(rows[i])/120,received);
        last=curve;stamp=received;
    }
    Curve at(double now,bool raw_valid,bool& held)const{
        held=false;double age=(now-stamp)*1000;
        if(!last.valid||age<0||age>(raw_valid?750:hold_ms)||(!raw_valid&&hold_ms==0))return {};
        held=!raw_valid;Curve out=last;double a[3][4]{};constexpr double rows[3]={180,270,350};
        for(unsigned i=0;i<3;++i){double t=rows[i]/360;a[i][0]=t*t;a[i][1]=t;a[i][2]=1;a[i][3]=filters[i].predict(now)*120;}
        if(!solve3(a,out.coef))return {};
        for(double y=out.ymin;y<=out.ymax;y+=10)if(!std::isfinite(out.x(y))||out.x(y)<0||out.x(y)>=640)return {};
        return out;
    }
};
}
