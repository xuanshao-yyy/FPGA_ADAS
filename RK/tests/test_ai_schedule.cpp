#include "ai_schedule.hpp"
#include <iostream>
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
unsigned simulated_completed(double fps){
    double now=0,due=0;unsigned completed=0;
    while(true){now=std::max(now,due);if(now+.058>1+1e-9)break;
        due=ai::next_due(now,fps);now+=.058;++completed;
    }
    return completed;
}
int main(){try{
    require(ai::parse_fps("0")==0 && ai::parse_fps("12")==12 && ai::parse_fps("30")==30,"Valid rates rejected");
    for(const char* value:{"-1","nan","inf","31","12x",""}){
        bool rejected=false;try{ai::parse_fps(value);}catch(const std::exception&){rejected=true;}
        require(rejected,"Invalid rate accepted");
    }
    require(ai::next_due(100,0)==100,"Unlimited scheduling must add no delay or divide by zero");
    require(std::abs(ai::next_due(100,12)-(100+1.0/12))<1e-9,"Limited scheduling changed");
    require(simulated_completed(12)==12 && simulated_completed(0)==17,"58ms consumer should not retain the 12Hz cap when unlimited");
    std::cout<<"PASS: zero/unlimited, finite caps, invalid rates, no division by zero, consumer-limited scheduling\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
