#pragma once
#include "light_rgb_core.hpp"
#include "protocol.hpp"
#include <rknn_api.h>
#include <fstream>
namespace ai {
class LightRGB {
    rknn_context context_=0;
    float threshold_,margin_;
    static void checked(const char* label,int rc){if(rc)throw std::runtime_error(std::string("LightRGB ")+label+": "+std::to_string(rc));}
public:
    LightRGB(const std::string& path,const std::string& output,float threshold,float margin):threshold_(threshold),margin_(margin){
        try{
            std::ifstream f(path,std::ios::binary|std::ios::ate);
            if(!f||f.tellg()<=0||f.tellg()>std::streamoff(64*1024*1024))throw std::runtime_error("Invalid RGB model");
            std::vector<char> data(size_t(f.tellg()));f.seekg(0);f.read(data.data(),data.size());if(!f)throw std::runtime_error("Short RGB model read");
            checked("init",rknn_init(&context_,data.data(),data.size(),0,nullptr));
            rknn_input_output_num count{};checked("count",rknn_query(context_,RKNN_QUERY_IN_OUT_NUM,&count,sizeof(count)));
            if(count.n_input!=1||count.n_output!=1)throw std::runtime_error("RGB model requires one input and three logits");
            rknn_tensor_attr in{},out{};checked("input",rknn_query(context_,RKNN_QUERY_INPUT_ATTR,&in,sizeof(in)));
            checked("output",rknn_query(context_,RKNN_QUERY_OUTPUT_ATTR,&out,sizeof(out)));
            bool valid=in.n_dims==4&&in.dims[0]==1&&((in.fmt==RKNN_TENSOR_NCHW&&in.dims[1]==3&&in.dims[2]==128&&in.dims[3]==128)||(in.fmt==RKNN_TENSOR_NHWC&&in.dims[1]==128&&in.dims[2]==128&&in.dims[3]==3));
            if(!valid||out.n_elems!=3)throw std::runtime_error("RGB model shape mismatch");
            if(out.type!=RKNN_TENSOR_INT8||out.qnt_type!=RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC)throw std::runtime_error("RGB delivery requires INT8 output");
            std::ofstream info(output+"/light_rgb_model_info.txt");info<<"model="<<path<<"\ninput=RGB uint8 NHWC 128x128\nclasses=RED,YELLOW,GREEN\noutput_type="<<get_type_string(out.type)<<"\nthreshold="<<threshold_<<"\nmargin="<<margin_<<"\nmode=RGB_PIXEL_CONSENSUS\n";
            if(!info)throw std::runtime_error("Cannot write RGB model metadata");
        }catch(...){if(context_)rknn_destroy(context_);context_=0;throw;}
    }
    ~LightRGB(){if(context_)rknn_destroy(context_);}
    RGBDecision infer(const uint8_t* pixels,const Box& box,const std::string& hsv){
        double start=qv::now_seconds();auto image=rgb_model_input(pixels,box);RGBDecision decision;
        if(image.empty()){decision.reason="RGB_INVALID_OR_TINY_ROI";return decision;}
        double prepared=qv::now_seconds();rknn_input input{};input.buf=image.data;input.size=128*128*3;input.type=RKNN_TENSOR_UINT8;input.fmt=RKNN_TENSOR_NHWC;
        checked("input_set",rknn_inputs_set(context_,1,&input));checked("run",rknn_run(context_,nullptr));
        rknn_output output{};output.want_float=1;checked("output_get",rknn_outputs_get(context_,1,&output,nullptr));
        try{
            if(!output.buf||output.size<3*sizeof(float))throw std::runtime_error("Short RGB logits");
            const float* data=static_cast<const float*>(output.buf);decision=rgb_decision({data[0],data[1],data[2]},hsv,threshold_,margin_);
        }catch(...){rknn_outputs_release(context_,1,&output);throw;}
        checked("release",rknn_outputs_release(context_,1,&output));
        decision.prep_ms=(prepared-start)*1000;decision.api_ms=(qv::now_seconds()-prepared)*1000;return decision;
    }
};
}
