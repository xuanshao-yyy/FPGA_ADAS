#include "live_tracking.hpp"
#include <fstream>
#include <iostream>
#include <cassert>
int main(int argc,char**argv){
    if(argc!=3)return 2;
    std::ifstream input(argv[1]);std::ofstream output(argv[2]);assert(input&&output);
    output<<"frame_id,received_s,legacy_lights,lower_lights,legacy_straight,lower_straight,other_seven_identical\n";
    ai::LiveTracker legacy(.25f),lower(.25f,.15f);double stamp;unsigned frame,count,frames=0,old_straight=0,new_straight=0;
    while(input>>stamp>>frame>>count){std::vector<ai::Box> boxes(count);for(auto& b:boxes)assert(bool(input>>b.x1>>b.y1>>b.x2>>b.y2>>b.score>>b.category));
        auto a=legacy.update(boxes,stamp,0),b=lower.update(boxes,stamp,0);
        auto lights=[](const auto& tracks){return std::count_if(tracks.begin(),tracks.end(),[](const auto& t){return t.box.category==7;});};
        auto straight=[](const auto& tracks){return std::any_of(tracks.begin(),tracks.end(),[](const auto& t){return t.box.category==7&&(t.box.x1+t.box.x2)/2>=525&&(t.box.x1+t.box.x2)/2<=645&&(t.box.y1+t.box.y2)/2>=135&&(t.box.y1+t.box.y2)/2<=212;});};
        bool x=straight(a),y=straight(b);old_straight+=x;new_straight+=y;
        output<<frame<<','<<stamp<<','<<lights(a)<<','<<lights(b)<<','<<x<<','<<y<<",1\n";
        auto remove=[](auto& tracks){tracks.erase(std::remove_if(tracks.begin(),tracks.end(),[](const auto& t){return t.box.category==7;}),tracks.end());};
        remove(a);remove(b);assert(a.size()==b.size());for(size_t i=0;i<a.size();++i)assert(a[i].id==b[i].id&&a[i].box.x1==b[i].box.x1&&a[i].box.x2==b[i].box.x2&&a[i].box.score==b[i].box.score);
        ++frames;
    }
    assert(frames>0);
    std::cout<<"PASS logged detections: frames="<<frames<<" legacy_straight_region="<<old_straight<<" lower_straight_region="<<new_straight<<" other seven class tracks identical. Not accuracy or board FPS.\n";
}
