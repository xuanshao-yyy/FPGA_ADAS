#include "detection.hpp"
#include <iostream>
#include <limits>

void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(){try{
    std::array<std::vector<float>,9> buffers;std::array<ai::Tensor,9> outputs;
    for(int k=0;k<9;++k){int h=80>>(k/3),c=k%3==0?64:k%3==1?8:1;
        buffers[k].resize(c*h*h);outputs[k]={buffers[k].data(),c,h,h,false};}
    require(ai::decode(outputs,.25f,.45f).empty(),"Blank outputs must not create detections");
    int x=30,y=30,h=80;
    buffers[1][(7*h+y)*h+x]=.9f;
    // Concentrate each distance distribution at 2 cells.
    for(int side=0;side<4;++side)for(int k=0;k<16;++k)buffers[0][((side*16+k)*h+y)*h+x]=k==2?100.f:-100.f;
    auto boxes=ai::decode(outputs,.25f,.45f);
    require(boxes.size()==1 && boxes[0].category==7,"Class index 7 must remain traffic_light");
    require(std::abs(boxes[0].x1-456)<.001 && std::abs(boxes[0].x2-520)<.001 &&
            std::abs(boxes[0].y1-176)<.001 && std::abs(boxes[0].y2-240)<.001,"DFL + 140px padding inversion is wrong");
    ai::Box a{0,0,100,100,.9f,2},b{1,1,99,99,.8f,2},c{1,1,99,99,.7f,7};
    require(ai::nms({a,b,c},.45f).size()==2,"NMS must suppress only within the same class");
    outputs[1].channels=80;bool rejected=false;
    try{ai::decode(outputs,.25f,.45f);}catch(const std::runtime_error&){rejected=true;}
    require(rejected,"80-class model must be rejected");outputs[1].channels=8;
    buffers[1][0]=std::numeric_limits<float>::quiet_NaN();rejected=false;
    try{ai::decode(outputs,.25f,.45f);}catch(const std::runtime_error&){rejected=true;}
    require(rejected,"NaN model output must be rejected");
    std::cout<<"PASS: empty outputs, DFL/letterbox, class mapping, class-separated NMS, invalid shapes and NaN\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
