#include "lane.hpp"
#include <fstream>
#include <iostream>
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("Usage: verify_lane output_float_nchw.bin mask.bin");
    std::vector<float> output(2*640*640);std::ifstream file(argv[1],std::ios::binary|std::ios::ate);
    if(!file || file.tellg()!=std::streamoff(output.size()*sizeof(float)))throw std::runtime_error("Wrong tensor file size");
    file.seekg(0);file.read(reinterpret_cast<char*>(output.data()),output.size()*sizeof(float));
    if(!file)throw std::runtime_error("Tensor read failed");
    auto mask=ai::decode_lane({output.data(),2,640,640,false},.5f);
    std::ofstream result(argv[2],std::ios::binary);result.write(reinterpret_cast<char*>(mask.data.data()),mask.data.size());
    if(!result)throw std::runtime_error("Mask write failed");std::cout<<mask.pixels<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
