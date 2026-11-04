#include "detection.hpp"
#include <fstream>
#include <iostream>
#include <iomanip>
#include <string>
int main(int argc,char**argv){try{
    if(argc!=2 && argc!=3)throw std::runtime_error("usage: verify_detection /directory/with/output0.bin...output8.bin [mosaic]");
    std::array<std::vector<float>,9> buffers;std::array<ai::Tensor,9> outputs;
    for(int k=0;k<9;++k){int h=80>>(k/3),c=k%3==0?64:k%3==1?8:1;buffers[k].resize(c*h*h);
        std::ifstream f(std::string(argv[1])+"/output"+std::to_string(k)+".bin",std::ios::binary);
        f.read(reinterpret_cast<char*>(buffers[k].data()),std::streamsize(buffers[k].size()*sizeof(float)));
        if(!f)throw std::runtime_error("Cannot read golden output "+std::to_string(k));outputs[k]={buffers[k].data(),c,h,h,false};}
    if(argc==3){if(std::string(argv[2])!="mosaic")throw std::runtime_error("Expected mosaic argument");
        auto channels=ai::split_mosaic(ai::decode_candidates(outputs,.25f),{{true,true,true,true}},.45f);
        std::cout<<std::setprecision(9);
        for(unsigned k=0;k<4;++k)for(const auto& b:channels[k])std::cout<<k<<','<<b.category<<','<<b.score<<','<<b.x1<<','<<b.y1<<','<<b.x2<<','<<b.y2<<'\n';
        return 0;
    }
    auto boxes=ai::decode(outputs,.25f,.45f);
    std::cout<<std::setprecision(9);for(const auto& b:boxes)std::cout<<b.category<<','<<b.score<<','<<b.x1<<','<<b.y1<<','<<b.x2<<','<<b.y2<<'\n';
    return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
