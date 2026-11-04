#pragma once
#include "protocol.hpp"
#include <gst/gst.h>
#include <rga/im2d.hpp>
#include <array>
#include <string>

namespace qv {
class DmaBuffer final:public FrameMemory {
    int fd_=-1;
    uint8_t* map_=nullptr;
    size_t bytes_=0;
    rga_buffer_handle_t handle_=0;
    bool writing_=false;
    void sync(uint64_t flags);
public:
    DmaBuffer(const std::string& heap,size_t bytes);
    ~DmaBuffer() override;
    DmaBuffer(const DmaBuffer&)=delete;
    DmaBuffer& operator=(const DmaBuffer&)=delete;
    uint8_t* data() const override {return map_;}
    size_t size() const {return bytes_;}
    int fd() const {return fd_;}
    void begin_write() override;
    void finish_write() override;
    void begin_read();
    void finish_read();
    rga_buffer_t image(int w,int h,int format) const;
};
void rga_blit(DmaBuffer& src,int sw,int sh,int sfmt,DmaBuffer& dst,
              int dw,int dh,int dfmt,im_rect destination);
// Synchronous display-only crop/scale; source pixels and inference are unchanged.
void rga_blit_region(DmaBuffer& src,int sw,int sh,int sfmt,DmaBuffer& dst,
                     int dw,int dh,int dfmt,im_rect source,im_rect destination);
// Optional job API is resolved at runtime. One sync job for four disjoint tiles;
// unsupported libraries/drivers fall back once to the ordinary synchronous path.
void rga_display_regions(DmaBuffer& src,DmaBuffer& dst,const std::array<im_rect,4>& sources,
                         const std::array<im_rect,4>& destinations,bool& batch_allowed);
// Runs a real RGA operation and checks known RGB colors in NV12; fails rather
// than silently using CPU conversion or choosing a format solely by its name.
int verify_rga_rgb565(const std::string& heap);
int verify_rga_rgb888(const std::string& heap,int rgb565_format);
void verify_rga_mosaic(const std::string& heap,int rgb565_format,int rgb888_format);
void initialize_nv12(DmaBuffer& buffer,unsigned w,unsigned h);
GstBuffer* wrap_nv12_dma(std::shared_ptr<DmaBuffer> buffer,unsigned w,unsigned h);
}
