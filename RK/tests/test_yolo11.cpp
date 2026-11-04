#include "detection.hpp"
#include <cassert>
#include <iostream>
int main(){
    std::array<ai::Tensor,9> tensors{};std::array<std::vector<float>,9> memory;
    for(int b=0;b<3;++b){int w=640/(8<<b),h=384/(8<<b);
        for(int k=0;k<3;++k){auto& t=tensors[b*3+k];t.channels=k==0?64:k==1?10:1;t.width=w;t.height=h;memory[b*3+k].assign(w*h*t.channels,0);t.data=memory[b*3+k].data();}}
    int cell=20*80+40;memory[1][7*80*48+cell]=.8f;memory[1][8*80*48+cell]=.99f;memory[1][9*80*48+cell]=.98f;
    auto boxes=ai::decode_candidates(tensors,.5,640,384);assert(boxes.size()==1&&boxes[0].category==7&&boxes[0].score==.8f);
    memory[1][7*80*48+cell]=0;assert(ai::decode_candidates(tensors,.5,640,384).empty());
    assert(std::string(ai::Classes[7])=="TRAFFIC_LIGHT");
    std::cout<<"PASS YOLO11 ten-channel decode preserves exactly eight application classes before NMS\n";
}
