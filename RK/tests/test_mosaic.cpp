#include "detection.hpp"
#include <iostream>
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main(){try{
    std::array<bool,4> active{{true,true,true,true}};std::vector<ai::Box> candidates;
    for(unsigned k=0;k<4;++k){float x=float(k%2*320),y=float(k/2*320+70);
        candidates.push_back({x+10,y+20,x+50,y+60,.9f,2});
        candidates.push_back({x+11,y+21,x+49,y+59,.8f,2});
        candidates.push_back({x+11,y+21,x+49,y+59,.7f,7});
    }
    auto result=ai::split_mosaic(candidates,active,.45f);
    for(unsigned k=0;k<4;++k){require(result[k].size()==2,"Per-channel/class NMS failed");const auto& b=result[k][0];
        require(b.x1==40 && b.y1==80 && b.x2==200 && b.y2==240,"Channel offset/scale inversion failed");}
    active[1]=false;result=ai::split_mosaic(candidates,active,.45f);
    require(result[1].empty() && result[0].size()==2,"Absent channel must remain empty");
    candidates={{300,100,340,200,.99f,2},{100,300,200,340,.99f,2},
                {10,10,50,30,.99f,2},{330,330,370,350,.99f,2}};
    result=ai::split_mosaic(candidates,active,.45f);
    for(const auto& channel:result)require(channel.empty(),"Seam/padding boxes must be rejected");
    candidates={{10,60,50,100,.9f,2},{10,210,50,270,.9f,2},{-5,80,20,100,.9f,2}};
    result=ai::split_mosaic(candidates,active,.45f);
    require(result[0].size()==3,"Valid image-edge boxes should be clipped");
    require(result[0][0].y1==0 && result[0][1].y2==720 && result[0][2].x1==0,"Source bounds clipping failed");
    std::array<std::vector<float>,9> buffers;std::array<ai::Tensor,9> outputs;
    for(int k=0;k<9;++k){int h=80>>(k/3),c=k%3==0?64:k%3==1?8:1;
        buffers[k].resize(c*h*h);outputs[k]={buffers[k].data(),c,h,h,false};}
    int x=10,y=15,h=80;buffers[1][(2*h+y)*h+x]=.9f;
    for(int side=0;side<4;++side)for(int k=0;k<16;++k)buffers[0][((side*16+k)*h+y)*h+x]=k==2?100.f:-100.f;
    auto raw=ai::decode_candidates(outputs,.25f);
    require(raw.size()==1 && raw[0].x1==68 && raw[0].y1==108,"Decoder must preserve 640x640 input coordinates");
    result=ai::split_mosaic(raw,active,.45f);
    require(result[0].size()==1 && result[0][0].x1==272 && result[0][0].y1==152,"DFL-to-mosaic source mapping failed");
    std::cout<<"PASS: four offsets, scale, independent NMS, inactive regions, seams, padding, clipping, DFL input coordinates\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
