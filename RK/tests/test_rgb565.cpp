#include "rgb565.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
void check(bool value){if(!value)throw std::runtime_error("RGB565 byte-order test failed");}
int main(){try{
    // LE red, green, blue, white, black -> the corresponding BE byte stream.
    std::array<uint8_t,10> colors{{0x00,0xf8,0xe0,0x07,0x1f,0x00,0xff,0xff,0x00,0x00}};
    const std::array<uint8_t,10> expected{{0xf8,0x00,0x07,0xe0,0x00,0x1f,0xff,0xff,0x00,0x00}};
    qv::swap_rgb565_bytes(colors.data(),colors.size());check(colors==expected);
    // Every possible RGB565 value, unaligned input and vector tail, guard bytes.
    const size_t size=65536*2+6;
    std::vector<uint8_t> data(size+2,0xa5),original;
    for(size_t i=0;i<size/2;++i){data[1+2*i]=uint8_t(i);data[2+2*i]=uint8_t(i>>8);}
    original=data;
    qv::swap_rgb565_bytes(data.data()+1,size);
    for(size_t i=0;i<size/2;++i){check(data[1+2*i]==uint8_t(i>>8));check(data[2+2*i]==uint8_t(i));}
    check(data.front()==0xa5 && data.back()==0xa5);
    qv::swap_rgb565_bytes(data.data()+1,size);check(data==original);
    std::cout<<"PASS RGB565: primary colors, all 65536 words, unaligned/tail/guards, reversible\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
