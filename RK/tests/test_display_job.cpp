#include "display_job.hpp"
#include <cassert>
#include <iostream>
#include <vector>
int main(){
    // Actual scheduling helper, fake SDK functions: checks ordering/lifetime,
    // not whether the physical RGA driver supports task jobs.
    for(int failure:{-1,0,1,2,3,4,5}){
        std::vector<int> calls;
        auto ok=surround_ui::submit_display_job([&](){calls.push_back(10);return failure==0?0u:7u;},
            [&](unsigned job,unsigned i){assert(job==7);calls.push_back(int(i));return failure!=int(i)+1;},
            [&](unsigned job){assert(job==7);calls.push_back(20);return failure!=5;},
            [&](unsigned job){assert(job==7);calls.push_back(30);});
        assert(ok==(failure==-1));
        if(failure==0)assert(calls==std::vector<int>{10});
        if(failure>=1&&failure<=4){assert(calls.back()==30);assert(calls.size()==size_t(failure+2));}
        if(failure==-1||failure==5)assert((calls==std::vector<int>{10,0,1,2,3,20}));
    }
    std::cout<<"PASS display job: four tasks in one sync end, no reuse before completion, missing handle, partial-add cancellation, end failure without double cancellation. Driver support pending.\n";
}
