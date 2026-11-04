#include "rk_dma.hpp"
#include "display_job.hpp"
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <linux/dma-heap.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <fstream>
#include <dlfcn.h>
#include <rga/im2d_task.h>

namespace qv {
static std::runtime_error system_error(const std::string& op){return std::runtime_error(op+": "+std::strerror(errno));}
DmaBuffer::DmaBuffer(const std::string& heap,size_t bytes):bytes_(bytes) {
    int heapfd=open(heap.c_str(),O_RDWR|O_CLOEXEC);
    if(heapfd<0)throw system_error("open "+heap);
    dma_heap_allocation_data request{};request.len=bytes;request.fd_flags=O_RDWR|O_CLOEXEC;
    int rc=ioctl(heapfd,DMA_HEAP_IOCTL_ALLOC,&request);int saved=errno;close(heapfd);errno=saved;
    if(rc<0)throw system_error("DMA_HEAP_IOCTL_ALLOC "+heap);
    fd_=int(request.fd);
    void* p=mmap(nullptr,bytes_,PROT_READ|PROT_WRITE,MAP_SHARED,fd_,0);
    if(p==MAP_FAILED){saved=errno;close(fd_);fd_=-1;errno=saved;throw system_error("mmap DMA buffer");}
    map_=static_cast<uint8_t*>(p);
    handle_=importbuffer_fd(fd_,int(bytes_));
    if(!handle_){munmap(map_,bytes_);close(fd_);map_=nullptr;fd_=-1;throw std::runtime_error("RGA importbuffer_fd failed; check RGA/heap compatibility");}
}
DmaBuffer::~DmaBuffer(){
    if(writing_){dma_buf_sync s{};s.flags=DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE;ioctl(fd_,DMA_BUF_IOCTL_SYNC,&s);}
    if(handle_)releasebuffer_handle(handle_);
    if(map_)munmap(map_,bytes_);
    if(fd_>=0)close(fd_);
}
void DmaBuffer::sync(uint64_t flags){
    dma_buf_sync request{};request.flags=flags;
    int rc;do{rc=ioctl(fd_,DMA_BUF_IOCTL_SYNC,&request);}while(rc<0 && errno==EINTR);
    if(rc<0)throw system_error("DMA_BUF_IOCTL_SYNC");
}
void DmaBuffer::begin_write(){if(!writing_){sync(DMA_BUF_SYNC_START|DMA_BUF_SYNC_WRITE);writing_=true;}}
void DmaBuffer::finish_write(){if(writing_){sync(DMA_BUF_SYNC_END|DMA_BUF_SYNC_WRITE);writing_=false;}}
void DmaBuffer::begin_read(){sync(DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ);}
void DmaBuffer::finish_read(){sync(DMA_BUF_SYNC_END|DMA_BUF_SYNC_READ);}
rga_buffer_t DmaBuffer::image(int w,int h,int format) const {return wrapbuffer_handle(handle_,w,h,format);}
void rga_blit(DmaBuffer& src,int sw,int sh,int sfmt,DmaBuffer& dst,
              int dw,int dh,int dfmt,im_rect destination){
    rga_blit_region(src,sw,sh,sfmt,dst,dw,dh,dfmt,{0,0,sw,sh},destination);
}
void rga_blit_region(DmaBuffer& src,int sw,int sh,int sfmt,DmaBuffer& dst,
                     int dw,int dh,int dfmt,im_rect source,im_rect destination){
    auto s=src.image(sw,sh,sfmt);auto d=dst.image(dw,dh,dfmt);
    if(sfmt!=RK_FORMAT_YCbCr_420_SP && dfmt==RK_FORMAT_YCbCr_420_SP)
        d.color_space_mode=IM_RGB_TO_YUV_BT601_LIMIT;
    IM_STATUS result=improcess(s,d,{},source,destination,{},-1,nullptr,nullptr,IM_SYNC);
    if(result!=IM_STATUS_SUCCESS)throw std::runtime_error(std::string("RGA improcess: ")+imStrError(result));
}
void initialize_nv12(DmaBuffer& b,unsigned w,unsigned h){
    b.begin_write();std::memset(b.data(),16,size_t(w)*h);std::memset(b.data()+size_t(w)*h,128,size_t(w)*h/2);b.finish_write();
}
void rga_display_regions(DmaBuffer& src,DmaBuffer& dst,const std::array<im_rect,4>& sources,
                         const std::array<im_rect,4>& destinations,bool& batch_allowed){
    // C++ symbol names match the installed GCC/AArch64 librga ABI. No new hard
    // linker dependency: old board libraries can still load the executable.
    static auto begin=reinterpret_cast<decltype(&imbeginJob)>(dlsym(RTLD_DEFAULT,"_Z10imbeginJobm"));
    static auto end=reinterpret_cast<decltype(&imendJob)>(dlsym(RTLD_DEFAULT,"_Z8imendJobjiiPi"));
    static auto task=reinterpret_cast<decltype(&improcessTask)>(dlsym(RTLD_DEFAULT,"_Z13improcessTaskj12rga_buffer_tS_S_7im_rectS0_S0_P6im_opti"));
    static auto cancel=reinterpret_cast<decltype(&imcancelJob)>(dlsym(RTLD_DEFAULT,"_Z11imcancelJobj"));
    if(batch_allowed){
        auto s=src.image(1280,720,RK_FORMAT_YCbCr_420_SP),d=dst.image(1280,720,RK_FORMAT_YCbCr_420_SP);
        bool ok=surround_ui::submit_display_job([&](){return begin&&end&&task&&cancel?begin(0):0;},
            [&](auto job,unsigned i){return task(job,s,d,{},sources[i],destinations[i],{},nullptr,0)==IM_STATUS_SUCCESS;},
            [&](auto job){return end(job,IM_SYNC,-1,nullptr)==IM_STATUS_SUCCESS;},[&](auto job){cancel(job);});
        if(ok)return;
        batch_allowed=false;
        std::cerr<<"RGA display batch unavailable; using serial synchronous crops (no CPU fallback).\n";
    }
    // If a job failed after partial output, all four destinations are replaced.
    for(unsigned i=0;i<4;++i)rga_blit_region(src,1280,720,RK_FORMAT_YCbCr_420_SP,dst,1280,720,
                                          RK_FORMAT_YCbCr_420_SP,sources[i],destinations[i]);
}
void verify_rga_mosaic(const std::string& heap,int rgb565_format,int rgb888_format){
    DmaBuffer source(heap,1280*720*2),target(heap,640*640*3);
    const uint16_t words[]={0xf800,0x07e0,0x001f,0xffff};
    // RGB565 -> RGB888 may zero-fill low bits (248/252) rather than produce 255.
    // Use this device's already-validated, unscaled conversion as the reference.
    DmaBuffer reference_source(heap,128*32*2),reference_rgb(heap,128*32*3);
    reference_source.begin_write();
    for(unsigned n=0;n<128*32;++n){auto word=words[(n%128)/32];
        reference_source.data()[2*n]=uint8_t(word);reference_source.data()[2*n+1]=uint8_t(word>>8);}
    reference_source.finish_write();
    rga_blit(reference_source,128,32,rgb565_format,reference_rgb,128,32,rgb888_format,{0,0,128,32});
    int colors[4][3]{};reference_rgb.begin_read();
    for(unsigned k=0;k<4;++k){std::cout<<"RGA mosaic reference CH"<<k<<" RGB=";
        for(int c=0;c<3;++c){colors[k][c]=reference_rgb.data()[(16*128+k*32+16)*3+c];std::cout<<(c?",":"")<<colors[k][c];}
        std::cout<<"\n";}
    reference_rgb.finish_read();
    target.begin_write();std::memset(target.data(),114,target.size());target.finish_write();
    for(unsigned k=0;k<4;++k){
        source.begin_write();
        for(unsigned n=0;n<1280*720;++n){source.data()[2*n]=uint8_t(words[k]);source.data()[2*n+1]=uint8_t(words[k]>>8);}
        source.finish_write();
        rga_blit(source,1280,720,rgb565_format,target,640,640,rgb888_format,
                 {int((k%2)*320),int((k/2)*320+70),320,180});
    }
    target.begin_read();bool ok=true;unsigned failures=0;
    auto check=[&](unsigned channel,int px,int py,int c,int expected,int tolerance){
        int actual=target.data()[(py*640+px)*3+c];
        if(std::abs(actual-expected)>tolerance){ok=false;
            if(failures++<12)std::cerr<<"RGA mosaic mismatch CH"<<channel<<" xy="<<px<<','<<py<<" component="<<c<<" expected="<<expected<<" actual="<<actual<<"\n";}
    };
    for(unsigned k=0;k<4;++k){int x=int(k%2*320),y=int(k/2*320);
        for(int dy:{0,90,179})for(int dx:{0,160,319})for(int c=0;c<3;++c)
            check(k,x+dx,y+70+dy,c,colors[k][c],2);
        for(int dy:{0,69,250,319})for(int c=0;c<3;++c)
            check(k,x+160,y+dy,c,114,0);
    }
    if(!ok){std::ofstream ppm("rga_mosaic_failed.ppm",std::ios::binary);
        ppm<<"P6\n640 640\n255\n";ppm.write(reinterpret_cast<const char*>(target.data()),640*640*3);
        std::cerr<<"RGA mismatch components="<<failures<<"; wrote rga_mosaic_failed.ppm in working directory\n";}
    target.finish_read();
    if(!ok)throw std::runtime_error("RGA mosaic self-check FAILED: four RGB quadrants or padding incorrect");
    std::cout<<"RGA mosaic self-check PASS: 4x1280x720 -> 4x320x180, RGB colors, offsets and padding114.\n";
}
int verify_rga_rgb565(const std::string& heap){
    constexpr int w=128,h=32;
    DmaBuffer input(heap,w*h*2),output(heap,w*h*3/2);
    const uint16_t words[]={0xf800,0x07e0,0x001f,0xffff};
    input.begin_write();
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){const auto v=words[x/32];input.data()[(y*w+x)*2]=uint8_t(v);input.data()[(y*w+x)*2+1]=uint8_t(v>>8);}
    input.finish_write();
    // RGA names/order differ from GStreamer RGB16. Verify, do not guess.
    for(int format:{RK_FORMAT_BGR_565,RK_FORMAT_RGB_565}){
        try{
            rga_blit(input,w,h,format,output,w,h,RK_FORMAT_YCbCr_420_SP,{0,0,w,h});
            output.begin_read();bool ok=true;
            const int expected[4][3]={{82,90,240},{145,54,34},{41,240,110},{235,128,128}};
            for(int i=0;i<4;++i){const int x=i*32+16,y=16;
                const int actual[]={output.data()[y*w+x],output.data()[w*h+(y/2)*w+x],output.data()[w*h+(y/2)*w+x+1]};
                for(int c=0;c<3;++c)if(std::abs(actual[c]-expected[i][c])>8)ok=false;
            }
            output.finish_read();
            if(ok){std::cout<<"RGA color self-check PASS: RGB565 LE -> NV12 BT.601 limited; input format=0x"<<std::hex<<format<<std::dec<<"\n";return format;}
        }catch(const std::exception& e){std::cerr<<"RGA color candidate 0x"<<std::hex<<format<<std::dec<<": "<<e.what()<<"\n";}
    }
    throw std::runtime_error("RGA RGB565 color self-check FAILED. No CPU fallback; report this output before using live video.");
}
GstBuffer* wrap_nv12_dma(std::shared_ptr<DmaBuffer> owner,unsigned w,unsigned h){
    int fd=fcntl(owner->fd(),F_DUPFD_CLOEXEC,0);if(fd<0)throw system_error("dup DMA fd");
    GstAllocator* allocator=gst_dmabuf_allocator_new();
    GstMemory* memory=gst_dmabuf_allocator_alloc(allocator,fd,size_t(w)*h*3/2);
    gst_object_unref(allocator);
    if(!memory){close(fd);throw std::runtime_error("gst_dmabuf_allocator_alloc failed");}
    // Lease is attached to MEMORY, not only the buffer: downstream may retain
    // that memory after discarding its original buffer (DRM scanout, MPP, etc.).
    static GQuark lease_key=g_quark_from_static_string("qv02-dma-lease");
    gst_mini_object_set_qdata(GST_MINI_OBJECT(memory),lease_key,new std::shared_ptr<DmaBuffer>(std::move(owner)),
        [](gpointer p){delete static_cast<std::shared_ptr<DmaBuffer>*>(p);});
    GstBuffer* buffer=gst_buffer_new();gst_buffer_append_memory(buffer,memory);
    gsize offsets[GST_VIDEO_MAX_PLANES]={0,size_t(w)*h,0,0};
    gint strides[GST_VIDEO_MAX_PLANES]={int(w),int(w),0,0};
    gst_buffer_add_video_meta_full(buffer,GST_VIDEO_FRAME_FLAG_NONE,GST_VIDEO_FORMAT_NV12,w,h,2,offsets,strides);
    return buffer;
}
int verify_rga_rgb888(const std::string& heap,int rgb565_format){
    constexpr int w=128,h=32;DmaBuffer source(heap,w*h*2),destination(heap,w*h*3);
    const uint16_t words[]={0xf800,0x07e0,0x001f,0xffff};
    source.begin_write();
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){uint16_t v=words[x/32];source.data()[(y*w+x)*2]=uint8_t(v);source.data()[(y*w+x)*2+1]=uint8_t(v>>8);}
    source.finish_write();
    for(int format:{RK_FORMAT_RGB_888,RK_FORMAT_BGR_888}){
        rga_blit(source,w,h,rgb565_format,destination,w,h,format,{0,0,w,h});
        destination.begin_read();bool valid=true;
        const int expected[4][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,255}};
        for(int color=0;color<4;++color)for(int component=0;component<3;++component)
            if(std::abs(int(destination.data()[(16*w+color*32+16)*3+component])-expected[color][component])>8)valid=false;
        destination.finish_read();
        if(valid){std::cout<<"RGA NPU input self-check PASS: RGB565 LE -> RGB byte order; output format=0x"<<std::hex<<format<<std::dec<<"\n";return format;}
    }
    throw std::runtime_error("RGA -> RGB888 byte order self-check failed; refusing incorrect NPU colors");
}
}
