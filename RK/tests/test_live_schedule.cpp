#include "live_schedule.hpp"
#include <cassert>
#include <iostream>
int main(){
    ai::LiveSchedule scheduler;std::array<bool,6> ready{{true,true,false,true,true,true}};
    std::array<double,6> time{};std::array<unsigned,6> count{};
    const double costs[]={47,136,47,47,47,47},weights[]={4,3,1,1,1,1};
    for(int n=0;n<4000;++n){int j=scheduler.choose(ready);assert(j>=0 && ready[j]);
        scheduler.complete(j,costs[j],weights[j]);time[j]+=costs[j];++count[j];}
    double sum=0;for(auto t:time)sum+=t;
    assert(std::abs(time[0]/sum-.4)<.01);assert(std::abs(time[1]/sum-.3)<.01);
    for(int j:{3,4,5})assert(std::abs(time[j]/sum-.1)<.01 && count[j]>100);
    ready.fill(false);assert(scheduler.choose(ready)==-1);
    ready[0]=true;assert(scheduler.choose(ready)==0);scheduler.complete(0,47,6);
    ready[4]=true;bool served=false;
    for(int n=0;n<30;++n){int j=scheduler.choose(ready);served|=j==4;scheduler.complete(j,47,j==0?6:1);}
    assert(served);
    // A 12Hz latest-frame source with a shared front slot and expensive model.
    // Force overdue model jobs at job boundaries, without capping other work.
    ai::LiveSchedule bounded;double now=0,next_source=0,last_start=-1,max_gap=0;
    bool front=false,others=false;unsigned models=0,side_jobs=0;
    while(now<60){
        if(next_source<=now){front=others=true;while(next_source<=now)next_source+=1./12;}
        std::array<bool,6> slots{{front,front,others,false,false,false}};
        bool due=ai::model_deadline_due(now,last_start,500);
        // Healthy hybrid suppresses redundant semantic work until the deadline.
        slots[1]=slots[1]&&due;
        int job=bounded.choose(slots,due&&slots[1]?1:-1);
        if(job<0){now=next_source;continue;}
        if(job==1){if(last_start>=0)max_gap=std::max(max_gap,now-last_start);last_start=now;++models;}
        if(job<=1)front=false;else {others=false;++side_jobs;}
        double cost=job==1?.135:job==0?.04:.05;
        bounded.complete(job,cost*1000,job==1?3:6);now+=cost;
    }
    assert(models>80 && side_jobs>300 && max_gap<.72);
    // An overdue deep job may have a large virtual service mark. It must not
    // push the wake-up floor ahead of the actually least-served ready job.
    ai::LiveSchedule wake;
    std::array<bool,6> intermittent{{true,true,true,false,false,false}};
    assert(wake.choose(intermittent)==0);wake.complete(1,1000,1);
    assert(wake.choose(intermittent,1)==1);
    intermittent[0]=intermittent[1]=false;assert(wake.choose(intermittent)==2);
    wake.complete(2,10,1);intermittent[0]=true;
    assert(wake.choose(intermittent)==0); // Old forced-floor update starved waking front input.
    assert(!ai::model_deadline_due(10,0,0));
    std::cout<<"PASS soft deadline under intermittent 12Hz shared-slot input; simulated max-start-gap="<<max_gap<<"s (not board timing)\n";
    std::cout<<"PASS measured-time 40/30/10/10/10 shares, no Hz cap, idle wake/fairness\n";
}
