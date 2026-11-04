#include "light_rgb_core.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
int main(int argc,char** argv){
    auto red=ai::rgb_decision({8,0,0},"RED");assert(red.color=="RED");
    assert(ai::rgb_decision({0,8,0},"YELLOW").color=="YELLOW");
    assert(ai::rgb_decision({0,0,8},"UNKNOWN").color=="UNKNOWN");
    assert(ai::rgb_decision({0,0,8},"RED").reason=="RGB_PIXEL_DISAGREE");
    assert(ai::rgb_decision({0,0,0},"GREEN").reason=="RGB_LOW_CONFIDENCE");
    assert(ai::rgb_decision({NAN,0,0},"RED").color=="UNKNOWN");
    assert(std::abs(red.probabilities[0]+red.probabilities[1]+red.probabilities[2]-1)<1e-6);
    if(argc==2){
        std::string root=argv[1];std::ifstream index(root+"/color_probe.txt");assert(index);
        std::string stem;ai::Box box;
        while(index>>stem>>box.x1>>box.y1>>box.x2>>box.y2){
            std::vector<uint8_t> pixels(1280*720*2),golden(128*128*3);
            std::ifstream raw(root+"/"+stem+".rgb565",std::ios::binary),ref(root+"/"+stem+".rgb128",std::ios::binary);
            raw.read(reinterpret_cast<char*>(pixels.data()),pixels.size());ref.read(reinterpret_cast<char*>(golden.data()),golden.size());assert(raw&&ref);
            auto result=ai::rgb_model_input(pixels.data(),box);assert(result.total()*3==golden.size());
            assert(std::equal(golden.begin(),golden.end(),result.data));
            std::ifstream values(root+"/"+stem+".logits");std::array<float,3> original{},quantized{};
            for(auto& v:original)values>>v;for(auto& v:quantized)values>>v;assert(values);
            auto hsv=ai::classify_light(ai::light_roi_rgb565(pixels.data(),box),box,100,120,160,.005);
            auto before=ai::rgb_decision(original,hsv.color),after=ai::rgb_decision(quantized,hsv.color);
            assert(before.color==after.color);
            assert(after.color==(stem=="red"?"RED":stem=="green"?"GREEN":"UNKNOWN"));
            std::cout<<stem<<" HSV="<<hsv.color<<" FP32_consensus="<<before.color<<" INT8_consensus="<<after.color<<'\n';

        }
    }
    std::cout<<"PASS RGB gates and optional Python/C++ crop parity\n";
}
