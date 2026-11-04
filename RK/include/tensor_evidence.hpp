#pragma once
#include <rknn_api.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <string>

namespace evidence {
inline void ppm(const std::string& path,const uint8_t* rgb,int width,int height){
    std::ofstream f(path,std::ios::binary);f<<"P6\n"<<width<<' '<<height<<"\n255\n";
    f.write(reinterpret_cast<const char*>(rgb),size_t(width)*height*3);
    if(!f)throw std::runtime_error("Cannot write evidence RGB: "+path);
}
// Logical OUTPUT_ATTR / outputs_get layout, never native stride/padding memory.
inline void tensor(std::ostream& meta,const std::string& folder,unsigned index,const rknn_tensor_attr& a,const void* data,bool native){
    const auto file="output"+std::to_string(index)+".bin";
    const size_t bytes=size_t(a.n_elems)*(native?1:sizeof(float));
    std::ofstream f(folder+"/"+file,std::ios::binary);f.write(static_cast<const char*>(data),bytes);
    if(!f)throw std::runtime_error("Cannot write tensor evidence");
    meta<<std::setprecision(std::numeric_limits<float>::max_digits10)
        <<"{\"index\":"<<index<<",\"file\":\""<<file<<"\",\"storage\":\""<<(native?"int8":"float32")
        <<"\",\"format\":\""<<get_format_string(a.fmt)<<"\",\"n_elems\":"<<a.n_elems
        <<",\"bytes\":"<<bytes<<",\"zero_point\":"<<a.zp<<",\"scale\":"<<a.scale<<",\"dims\":[";
    for(unsigned k=0;k<a.n_dims;++k){if(k)meta<<',';meta<<a.dims[k];}meta<<"]}";
}
}
