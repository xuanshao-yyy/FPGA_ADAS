#include "panorama_layout.hpp"
#include "camera_roles.hpp"
#include <array>
#include <cassert>
#include <iostream>
int main(){
    std::array<int,2560> uses{};
    for(int x=0;x<2560;++x){int source=panorama_display::source_x(x);assert(source>=0&&source<2560);++uses[source];
        assert(panorama_display::source_x(source)==x);
        if((x<500)||(x>=968&&x<1592)||x>=2060)assert(source==x);}
    for(int count:uses)assert(count==1);
    for(auto r:panorama_display::SideCopies){assert((r.source_x|r.destination_x|r.width)%4==0);
        for(int n=0;n<r.width;++n)assert(panorama_display::source_x(r.destination_x+n)==r.source_x+n);}
    assert(panorama_display::source_x(700)>1592&&panorama_display::source_x(1800)<968);
    assert(camera_roles::Left==2&&camera_roles::Right==1&&camera_roles::Front==0&&camera_roles::Rear==3);
    std::cout<<"PASS RK-only panorama side permutation, disjoint RGA rectangles, preserved front/rear and CH2-left/CH1-right roles\n";
}
