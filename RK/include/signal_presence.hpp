#pragma once
#include <cmath>
namespace signal_demo {
// Counts new, timely detector observations, never UI ticks or repeated held frames.
// Leaving this observation episode does NOT imply a passed intersection or GREEN.
class SignalPresence {
    double frame=-1,good=-1,missing=-1;
    unsigned misses=0;
public:
    static constexpr unsigned ExitMs=1200,MinimumAbsentSamples=3,MaximumFrameGapMs=400;
    void reset(){frame=good=missing=-1;misses=0;}
    void observe(double received,bool qualified){
        if(!std::isfinite(received)||received<0||received<=frame)return;
        bool contiguous=frame>=0&&(received-frame)*1000<=MaximumFrameGapMs+1e-7;frame=received;
        if(qualified){good=received;missing=-1;misses=0;}
        else if(good>=0){if(!contiguous||missing<0){missing=received;misses=0;}++misses;}
    }
    bool recent(double now)const{return good>=0&&now>=good&&now-good<=2.;}
    bool may_exit(double now)const{return frame>=0&&now>=frame&&(now-frame)*1000<=MaximumFrameGapMs&&missing>=0&&
        misses>=MinimumAbsentSamples&&(frame-missing)*1000>=ExitMs-1e-7;}
    double age_ms(double now)const{return good<0?-1:(now-good)*1000;}
    double absence_ms()const{return missing<0?-1:(frame-missing)*1000;}
    unsigned absent_samples()const{return misses;}
};
}
