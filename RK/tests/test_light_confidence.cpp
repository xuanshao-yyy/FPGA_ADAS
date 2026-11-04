#include "live_tracking.hpp"
#include "visual.hpp"
#include <cassert>
#include <iostream>
int main(){
    ai::LiveTracker legacy(.25f),lower(.25f,.15f);
    ai::Box light{100,100,160,150,.20f,7},car{300,100,360,150,.20f,2};
    assert(legacy.update({light,car},1,0).empty());
    auto tracks=lower.update({light,car},1,0);assert(tracks.size()==1&&tracks[0].box.category==7);
    light.score=.12f;tracks=lower.update({light,car},1.1,0);assert(tracks.size()==1&&tracks[0].box.category==7);
    assert(lower.update({},1.2,0).empty()); // no fabricated/predicted observation
    ai::LiveTracker unchanged(.25f),changed(.25f,.15f);
    for(int step=0;step<10;++step){std::vector<ai::Box> boxes;for(int c=0;c<7;++c)boxes.push_back({100.f+c*100,100,150.f+c*100,160,step==4?.16f:.8f,c});
        boxes.push_back(light);auto a=unchanged.update(boxes,3+step*.1,0),b=changed.update(boxes,3+step*.1,0);
        b.erase(std::remove_if(b.begin(),b.end(),[](const auto& t){return t.box.category==7;}),b.end());
        assert(a.size()==b.size());for(size_t i=0;i<a.size();++i)assert(a[i].id==b[i].id&&a[i].box.score==b[i].box.score&&a[i].box.x1==b[i].box.x1);
    }
    ai::Result result;result.finished=result.received=qv::now_seconds();result.display_threshold=.25f;result.light_display_threshold=.15f;
    result.boxes={{100,100,200,200,.2f,7},{400,100,500,200,.2f,2}};
    std::vector<uint8_t> image(1280*720*3/2,16);ui::panel(image.data(),0,"TEST",true,true,true,result,10,500);
    assert(image[100*1280+77]==ui::Green.y);assert(image[100*1280+212]==16);
    bool rejected=false;try{ai::LiveTracker invalid(.25f,.05f);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
    std::cout<<"PASS traffic-light-only threshold, low-score maintenance, no fabricated boxes, unchanged seven-class tracking and raw-box display\n";
}
