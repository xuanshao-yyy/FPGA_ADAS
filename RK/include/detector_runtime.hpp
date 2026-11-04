#pragma once
#include "detection.hpp"
#include "lane4_core.hpp"
#include "protocol.hpp"
#include "tensor_evidence.hpp"
#include <rknn_api.h>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace ai {
struct DetectorStages {double inputs=0,run=0,get=0,validate=0,decode=0,release=0;};
class Detector {
    rknn_context context_=0;
    std::array<rknn_tensor_attr,9> attrs_{};
    bool native_=false;
    int width_=0,height_=0,classes_=8;
    float peak_=0;
    DetectorStages stages_;
    static void checked(const char* name,int rc){if(rc)throw std::runtime_error(std::string(name)+": "+std::to_string(rc));}
    static double elapsed(double start){return (qv::now_seconds()-start)*1000;}
public:
    Detector(const std::string& model,const std::string& output,bool native=false,const std::string& title="model_info.txt") :native_(native){
        try{
            std::ifstream f(model,std::ios::binary|std::ios::ate);
            if(!f || f.tellg()<=0 || f.tellg()>std::streamoff(1024ull*1024*1024))throw std::runtime_error("Invalid detector model: "+model);
            std::vector<char> data(size_t(f.tellg()));f.seekg(0);f.read(data.data(),data.size());
            if(!f)throw std::runtime_error("Short model read");
            checked("init",rknn_init(&context_,data.data(),uint32_t(data.size()),0,nullptr));
            rknn_input_output_num count{};checked("count",rknn_query(context_,RKNN_QUERY_IN_OUT_NUM,&count,sizeof(count)));
            if(count.n_input!=1 || count.n_output!=9)throw std::runtime_error("Expected 9-output YOLOv8/YOLO11 detector");
            rknn_tensor_attr input{};checked("input",rknn_query(context_,RKNN_QUERY_INPUT_ATTR,&input,sizeof(input)));
            if(input.n_dims!=4 || input.dims[0]!=1)throw std::runtime_error("Invalid detector batch/input rank");
            if(input.fmt==RKNN_TENSOR_NCHW && input.dims[1]==3){height_=input.dims[2];width_=input.dims[3];}
            else if(input.fmt==RKNN_TENSOR_NHWC && input.dims[3]==3){height_=input.dims[1];width_=input.dims[2];}
            else throw std::runtime_error("Invalid RGB input layout");
            if(width_!=640 || (height_!=640 && height_!=384))throw std::runtime_error("Only square640 or rectangle640x384 is supported");
            rknn_sdk_version version{};checked("version",rknn_query(context_,RKNN_QUERY_SDK_VERSION,&version,sizeof(version)));
            std::ofstream info(output+"/"+title);
            info<<"model="<<model<<"\nRuntime="<<version.api_version<<"\ndriver="<<version.drv_version<<"\ninput_width="<<width_<<"\ninput_height="<<height_<<"\nnative_output="<<native_<<'\n';
            rknn_tensor_attr classification{};classification.index=1;
            checked("classes",rknn_query(context_,RKNN_QUERY_OUTPUT_ATTR,&classification,sizeof(classification)));
            if(classification.n_dims!=4)throw std::runtime_error("Invalid class output rank");
            classes_=classification.fmt==RKNN_TENSOR_NCHW?classification.dims[1]:classification.dims[3];
            if(classes_!=8&&classes_!=10)throw std::runtime_error("Expected 8 or 10 detector channels");
            info<<"model_class_channels="<<classes_<<"\napplication_classes=0..7\n";
            for(unsigned k=0;k<9;++k){auto& a=attrs_[k];a.index=k;checked("attr",rknn_query(context_,RKNN_QUERY_OUTPUT_ATTR,&a,sizeof(a)));
                int c=k%3==0?64:k%3==1?classes_:1,h=height_/(8<<(k/3)),w=width_/(8<<(k/3));
                bool nchw=a.fmt==RKNN_TENSOR_NCHW && a.dims[1]==unsigned(c) && a.dims[2]==unsigned(h) && a.dims[3]==unsigned(w);
                bool nhwc=a.fmt==RKNN_TENSOR_NHWC && a.dims[1]==unsigned(h) && a.dims[2]==unsigned(w) && a.dims[3]==unsigned(c);
                if(a.n_dims!=4 || a.dims[0]!=1 || a.n_elems!=unsigned(c*h*w) || (!nchw&&!nhwc))throw std::runtime_error("Detector output grid mismatch");
                if(native_ && (a.type!=RKNN_TENSOR_INT8 || a.qnt_type!=RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC || !std::isfinite(a.scale) || a.scale<=0))
                    throw std::runtime_error("Native output requires affine INT8 tensors");
                info<<a.name<<" format="<<get_format_string(a.fmt)<<" dims=";for(unsigned n=0;n<4;++n)info<<a.dims[n]<<' ';
                info<<" type="<<get_type_string(a.type)<<" zp="<<a.zp<<" scale="<<a.scale<<'\n';
            }
            if(!info)throw std::runtime_error("Cannot write detector attributes");
            std::cout<<"YOLO input "<<width_<<'x'<<height_<<", outputs="<<(native_?"native INT8":"float")<<", Runtime="<<version.api_version<<'\n';
        }catch(...){if(context_)rknn_destroy(context_);context_=0;throw;}
    }
    ~Detector(){if(context_)rknn_destroy(context_);}
    int width() const{return width_;}int height() const{return height_;}
    int class_count() const{return classes_;}
    float peak_score() const{return peak_;}
    const DetectorStages& stages() const{return stages_;}
    // Non-mosaic output is canonical 1280x720 SOURCE space, not model-input pixels.
    std::vector<Box> infer(uint8_t* rgb,float confidence,float iou,double& api,double& post,bool mosaic=false,const std::string& dump=""){
        if(mosaic && height_!=640)throw std::runtime_error("Mosaic requires square input");
        stages_={};double start=qv::now_seconds();
        rknn_input in{};in.buf=rgb;in.size=width_*height_*3;in.type=RKNN_TENSOR_UINT8;in.fmt=RKNN_TENSOR_NHWC;
        checked("inputs",rknn_inputs_set(context_,1,&in));stages_.inputs=elapsed(start);
        start=qv::now_seconds();checked("run",rknn_run(context_,nullptr));stages_.run=elapsed(start);
        std::array<rknn_output,9> outputs{};for(unsigned k=0;k<9;++k){outputs[k].index=k;outputs[k].want_float=!native_;}
        start=qv::now_seconds();checked("get",rknn_outputs_get(context_,9,outputs.data(),nullptr));stages_.get=elapsed(start);
        std::vector<Box> boxes;
        try{
            start=qv::now_seconds();std::array<Tensor,9> tensors{};
            for(unsigned k=0;k<9;++k){const auto& a=attrs_[k];auto& t=tensors[k];
                if(!outputs[k].buf || outputs[k].size<a.n_elems*(native_?1:sizeof(float)))throw std::runtime_error("Short detector output");
                t.channels=k%3==0?64:k%3==1?classes_:1;t.height=height_/(8<<(k/3));t.width=width_/(8<<(k/3));t.nhwc=a.fmt==RKNN_TENSOR_NHWC;
                if(native_){t.quantized=static_cast<const int8_t*>(outputs[k].buf);t.scale=a.scale;t.zero_point=a.zp;}
                else {t.data=static_cast<const float*>(outputs[k].buf);if(!lane4::finite_output(t.data,a.n_elems))throw std::runtime_error("Non-finite detector outputs");}
            }
            stages_.validate=elapsed(start);start=qv::now_seconds();
            boxes=mosaic?decode_candidates(tensors,confidence,width_,height_):decode(tensors,confidence,iou,width_,height_);
            peak_=0;
            for(unsigned k:{1u,4u,7u}){const auto& t=tensors[k];
                for(int c=0;c<8;++c)for(int y=0;y<t.height;++y)for(int x=0;x<t.width;++x)
                    peak_=std::max(peak_,t.at(c,y,x));
            }
            stages_.decode=elapsed(start);
            if(!dump.empty()){
                std::filesystem::create_directories(dump);
                evidence::ppm(dump+"/input.ppm",rgb,width_,height_);
                std::ofstream meta(dump+"/tensors.json");meta<<"{\"native_output\":"<<(native_?"true":"false")<<",\"width\":"<<width_<<",\"height\":"<<height_<<",\"tensors\":[";
                for(unsigned k=0;k<9;++k){if(k)meta<<',';evidence::tensor(meta,dump,k,attrs_[k],outputs[k].buf,native_);}meta<<"]}\n";
                auto write_boxes=[&](const std::string& name,const std::vector<Box>& values){
                    std::ofstream f(dump+"/"+name);f<<std::setprecision(9)<<'[';
                    for(size_t k=0;k<values.size();++k){const auto& b=values[k];if(k)f<<',';
                        f<<"{\"class\":"<<b.category<<",\"score\":"<<b.score<<",\"xyxy\":["<<b.x1<<','<<b.y1<<','<<b.x2<<','<<b.y2<<"]}";}f<<"]\n";
                    if(!f)throw std::runtime_error("Cannot dump boxes");};
                write_boxes("decoded_boxes.json",boxes);
                write_boxes("pre_nms_candidates.json",decode_candidates(tensors,confidence,width_,height_));
                if(!meta)throw std::runtime_error("Cannot dump tensor metadata");
            }
        }catch(...){rknn_outputs_release(context_,9,outputs.data());throw;}
        start=qv::now_seconds();checked("release",rknn_outputs_release(context_,9,outputs.data()));stages_.release=elapsed(start);
        api=stages_.inputs+stages_.run+stages_.get+stages_.release;post=stages_.validate+stages_.decode;
        return boxes;
    }
};
}
