#pragma once
#include "lane4_core.hpp"
#include <array>
#include <cstdint>

namespace lane4 {
// Same dequantized float values, channel tie rule, exp and threshold as float API.
inline cv::Mat decode_four_i8(const int8_t* values,const rknn_tensor_attr& a,double minimum){
    bool nchw=a.n_dims==4 && a.dims[0]==1 && a.dims[1]==4 && a.dims[2]==384 && a.dims[3]==640 && a.fmt==RKNN_TENSOR_NCHW;
    bool nhwc=a.n_dims==4 && a.dims[0]==1 && a.dims[1]==384 && a.dims[2]==640 && a.dims[3]==4 && a.fmt==RKNN_TENSOR_NHWC;
    if(!values || (!nchw&&!nhwc) || a.n_elems!=4*384*640 || a.type!=RKNN_TENSOR_INT8 ||
       a.qnt_type!=RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC || !std::isfinite(a.scale) || a.scale<=0)
        throw std::runtime_error("Invalid native INT8 lane contract");
    std::array<float,256> dequant{};
    for(int q=-128;q<=127;++q)dequant[q+128]=(q-a.zp)*a.scale;
    if(!finite_output(dequant.data(),dequant.size()))throw std::runtime_error("Non-finite INT8 dequantization scale");
    cv::Mat labels=cv::Mat::zeros(360,640,CV_8U);
    cv::parallel_for_(cv::Range(0,360),[&](const cv::Range& range){
        for(int y=range.start;y<range.end;++y){auto* dst=labels.ptr<uint8_t>(y);
            for(int x=0;x<640;++x){
                float scores[4];int best=0;
                for(int c=0;c<4;++c){int at=nchw?(c*384+y+12)*640+x:((y+12)*640+x)*4+c;
                    scores[c]=dequant[int(values[at])+128];if(c && scores[c]>scores[best])best=c;}
                if(!best)continue;
                if(minimum<=.25){dst[x]=uint8_t(best);continue;}
                double sum=0;for(int c=0;c<4;++c)sum+=std::exp(scores[c]-scores[best]);
                if(1/sum>=minimum)dst[x]=uint8_t(best);
            }
        }
    });
    return labels;
}
}
