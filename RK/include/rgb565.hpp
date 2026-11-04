#pragma once
#include <cstddef>
#include <cstdint>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace qv {
// Workaround at the encoder boundary only. Network frames remain RGB565 LE.
// Old MPP vepu541_set_fmt drops the LE flag and selects src_endian=1 for 565.
// Do not enable for MPP versions which correctly handle RGB565LE.
inline void swap_rgb565_bytes(uint8_t* pixels, size_t bytes) {
    size_t i=0;
#if defined(__aarch64__)
    for(;i+16<=bytes;i+=16)
        vst1q_u8(pixels+i,vrev16q_u8(vld1q_u8(pixels+i)));
#endif
    for(;i+1<bytes;i+=2){const uint8_t first=pixels[i];pixels[i]=pixels[i+1];pixels[i+1]=first;}
}
}
