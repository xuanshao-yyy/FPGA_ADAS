#include "lane_geometry.hpp"
#include "test_helpers.hpp"
#include <iostream>
std::vector<uint8_t> road(bool left=true,bool right=true,int shift=0,bool dashed=false){
    std::vector<uint8_t> mask(576*324);
    for(int y=162;y<308;++y){
        if(dashed && (y/24)%2)continue;
        int l=int(260-.7*(y-162))+shift,r=int(316+.7*(y-162))+shift;
        for(int dx=-2;dx<=2;++dx){if(left)mask[size_t(y*576+l+dx)]=1;if(right)mask[size_t(y*576+r+dx)]=1;}
    }
    return mask;
}
int main(){try{
    auto a=ai::extract_lane_geometry(road(),576,324);CHECK(a.valid && std::abs(a.offset)<.03f);
    CHECK(!ai::extract_lane_geometry(road(false,true),576,324).valid);
    CHECK(!ai::extract_lane_geometry(std::vector<uint8_t>(576*324),576,324).valid);
    std::vector<uint8_t> crossing(576*324);
    for(int y:{210,230,250})for(int dy=0;dy<6;++dy)for(int x=80;x<500;++x)crossing[size_t((y+dy)*576+x)]=1;
    CHECK(!ai::extract_lane_geometry(crossing,576,324).valid);
    auto with_crosswalk=road();
    for(int y=240;y<248;++y)for(int x=80;x<500;++x)with_crosswalk[size_t(y*576+x)]=1;
    CHECK(ai::extract_lane_geometry(with_crosswalk,576,324).valid);
    CHECK(ai::extract_lane_geometry(road(true,true,0,true),576,324).valid);
    ai::LaneGeometryFilter filter;
    auto g=filter.update(a,1,324);CHECK(g.valid&&!g.confirmed);
    g=filter.update(a,2,324);CHECK(g.confirmed && std::string(g.state)=="CENTER CANDIDATE");
    g=filter.update({},3,324);CHECK(!g.valid); // missing boundaries invalidate immediately
    g=filter.update(a,4,324);CHECK(!g.confirmed);
    g=filter.update(a,8,324);CHECK(!g.confirmed); // long stale gap resets evidence
    auto shifted=ai::extract_lane_geometry(road(true,true,-70),576,324);CHECK(shifted.valid && shifted.offset>.45f);
    ai::LaneGeometryFilter off;off.update(shifted,1,324);g=off.update(shifted,2,324);
    CHECK(std::string(g.state)=="OFFSET RIGHT");
    auto ego=ai::extract_lane_geometry(road(),576,324,.6f);CHECK(ego.valid && ego.offset>.3f);
    std::cout<<"PASS: two boundaries, missing/empty rejection, horizontal markings, dashed candidates, confirmation/reset and uncalibrated offset\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
