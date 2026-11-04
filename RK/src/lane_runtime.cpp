#include "lane_runtime.hpp"
#include "tensor_evidence.hpp"
#include "lane4_post.hpp"
#include "quant_lane.hpp"
#include "protocol.hpp"
#include <fstream>
#include <filesystem>
#include <iostream>
namespace ai {
namespace {
void status(const char* operation,int code){if(code!=RKNN_SUCC)throw std::runtime_error(std::string("Small4 ")+operation+": "+std::to_string(code));}
bool shape(const rknn_tensor_attr& a,unsigned c){return a.n_dims==4 && a.dims[0]==1 && a.n_elems==c*640*384 &&
    ((a.fmt==RKNN_TENSOR_NCHW && a.dims[1]==c && a.dims[2]==384 && a.dims[3]==640)||
     (a.fmt==RKNN_TENSOR_NHWC && a.dims[1]==384 && a.dims[2]==640 && a.dims[3]==c));}
double elapsed(double begin){return (qv::now_seconds()-begin)*1000;}
}
struct LaneSegmenter::Impl {
    rknn_context context=0;rknn_tensor_attr attr{};LaneOptions options;lane4::Post post;
    Impl(const LaneOptions& opt,const std::string& output):options(opt),post(opt.camera,opt.ego_x){try{
        cv::setNumThreads(2); // same postprocessing worker count as the validated offline benchmark
        std::ifstream file(opt.model,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Missing Small4 model");
        auto size=file.tellg();if(size<=0 || size>1024LL*1024*1024)throw std::runtime_error("Invalid Small4 size");
        std::vector<char> data(static_cast<size_t>(size));file.seekg(0);file.read(data.data(),size);if(!file)throw std::runtime_error("Short Small4 read");
        status("init",rknn_init(&context,data.data(),uint32_t(data.size()),0,nullptr));
        rknn_input_output_num count{};status("count",rknn_query(context,RKNN_QUERY_IN_OUT_NUM,&count,sizeof(count)));
        if(count.n_input!=1 || count.n_output!=1)throw std::runtime_error("Small4 needs one input/output");
        rknn_tensor_attr in{};status("input",rknn_query(context,RKNN_QUERY_INPUT_ATTR,&in,sizeof(in)));
        status("output",rknn_query(context,RKNN_QUERY_OUTPUT_ATTR,&attr,sizeof(attr)));
        if(!shape(in,3)||!shape(attr,4))throw std::runtime_error("Small4 requires RGB640x384 / four logits640x384; YOLOP is incompatible");
        if(opt.native_output && (attr.type!=RKNN_TENSOR_INT8 || attr.qnt_type!=RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC || !std::isfinite(attr.scale) || attr.scale<=0))
            throw std::runtime_error("Native lane output requires affine INT8");
        rknn_sdk_version version{};status("version",rknn_query(context,RKNN_QUERY_SDK_VERSION,&version,sizeof(version)));
        std::ofstream info(output+"/lane_model_info.txt");info<<"model="<<opt.model<<"\nRuntime="<<version.api_version<<"\ndriver="<<version.drv_version
            <<"\ninput=RGB NHWC640x384, pad114 top12/bottom12\nnative_output="<<opt.native_output<<"\noutput=four logits, owned snapshot for safe pipeline\n";
        for(const auto& a:{in,attr}){info<<a.name<<' '<<get_format_string(a.fmt);for(unsigned k=0;k<a.n_dims;++k)info<<' '<<a.dims[k];info<<'\n';}
        if(!info)throw std::runtime_error("Cannot write lane model info");
        std::cout<<"Small4 loaded, RGB640x384, four classes, Runtime="<<version.api_version<<"\n";
    }catch(...){if(context)rknn_destroy(context);context=0;throw;}}
    ~Impl(){if(context)rknn_destroy(context);}
    RawLane capture(uint8_t* rgb){
        if(!rgb)throw std::runtime_error("Null lane input");
        RawLane result;result.attr=attr;
        double stage=qv::now_seconds();
        rknn_input input{};input.buf=rgb;input.size=640*384*3;input.type=RKNN_TENSOR_UINT8;input.fmt=RKNN_TENSOR_NHWC;
        status("inputs",rknn_inputs_set(context,1,&input));result.inputs_ms=elapsed(stage);
        stage=qv::now_seconds();status("run",rknn_run(context,nullptr));result.run_ms=elapsed(stage);
        rknn_output out{};out.want_float=!options.native_output;stage=qv::now_seconds();status("get",rknn_outputs_get(context,1,&out,nullptr));result.get_ms=elapsed(stage);
        try{
            stage=qv::now_seconds();
            if(!out.buf || out.size<attr.n_elems*(options.native_output?1:sizeof(float)))throw std::runtime_error("Short lane output");
            if(options.native_output){auto p=static_cast<const int8_t*>(out.buf);result.quantized.assign(p,p+attr.n_elems);}
            else {auto p=static_cast<const float*>(out.buf);result.floating.assign(p,p+attr.n_elems);}
            result.copy_ms=elapsed(stage);
        }catch(...){rknn_outputs_release(context,1,&out);throw;}
        stage=qv::now_seconds();status("release",rknn_outputs_release(context,1,&out));result.release_ms=elapsed(stage);
        return result;
    }
    LaneMask process(const RawLane& raw,float confidence,const std::vector<Box>& boxes,const std::vector<TrackedBox>& tracks,double stamp,unsigned channel){
        double stage=qv::now_seconds();
        if(raw.quantized.empty() && (raw.floating.size()!=attr.n_elems || !lane4::finite_output(raw.floating.data(),attr.n_elems)))throw std::runtime_error("Invalid Small4 logits");
        if(!raw.quantized.empty() && raw.quantized.size()!=attr.n_elems)throw std::runtime_error("Short native lane snapshot");
        double validate=elapsed(stage);stage=qv::now_seconds();
        auto classes=raw.quantized.empty()?lane4::decode_seg_fast(raw.floating.data(),raw.attr,"four",640,384,{0,12,640,360},confidence):lane4::decode_four_i8(raw.quantized.data(),raw.attr,confidence);
        double decode=elapsed(stage);stage=qv::now_seconds();
        auto blocked=options.occlusion?lane4::vehicle_mask(boxes,tracks):cv::Mat::zeros(360,640,CV_8U);
        double occlusion=elapsed(stage);stage=qv::now_seconds();
        auto result=post.process(classes,blocked,stamp,channel,options.geometry);
        result.geometry_ms=elapsed(stage);result.inputs_ms=raw.inputs_ms;result.run_ms=raw.run_ms;result.get_ms=raw.get_ms;
        result.output_copy_ms=raw.copy_ms;result.validate_ms=validate;result.decode_ms=decode;result.occlusion_ms=occlusion;
        return result;
    }
};
LaneSegmenter::LaneSegmenter(const LaneOptions& options,const std::string& output):impl_(std::make_unique<Impl>(options,output)){}
LaneSegmenter::~LaneSegmenter()=default;
void LaneSegmenter::reset(unsigned channel){impl_->post.reset(channel);}
RawLane LaneSegmenter::capture(uint8_t* rgb){return impl_->capture(rgb);}
LaneMask LaneSegmenter::process(const RawLane& raw,float confidence,const std::vector<Box>& boxes,const std::vector<TrackedBox>& tracks,double timestamp,unsigned channel){return impl_->process(raw,confidence,boxes,tracks,timestamp,channel);}
LaneMask LaneSegmenter::infer(uint8_t* rgb,float confidence,double& api,double& post,const std::vector<Box>& boxes,const std::vector<TrackedBox>& tracks,double stamp,unsigned channel){
    auto raw=capture(rgb);auto result=process(raw,confidence,boxes,tracks,stamp,channel);
    api=raw.inputs_ms+raw.run_ms+raw.get_ms+raw.release_ms;
    post=raw.copy_ms+result.validate_ms+result.decode_ms+result.geometry_ms+result.occlusion_ms;
    return result;
}
void verify_lane(const LaneOptions& options,const std::string& output,unsigned warmup,const std::string& probe,bool parity){
    LaneSegmenter model(options,output);std::vector<uint8_t> rgb(640*384*3,114);
    if(!probe.empty()){
        std::ifstream file(probe,std::ios::binary);std::vector<uint8_t> detector(640*640*3);file.read(reinterpret_cast<char*>(detector.data()),detector.size());
        if(!file || file.peek()!=std::char_traits<char>::eof())throw std::runtime_error("Expected RGB640x640 detector probe");
        std::copy(detector.begin()+140*640*3,detector.begin()+500*640*3,rgb.begin()+12*640*3);
    }
    for(unsigned k=0;k<std::max(1u,warmup);++k){double api=0,post=0;auto mask=model.infer(rgb.data(),options.confidence,api,post);
        std::cout<<"Small4 CHECK pixels="<<mask.pixels<<" API="<<api<<" post="<<post<<"ms\n";}
    if(parity){
        auto reference_options=options;reference_options.native_output=!options.native_output;
        std::filesystem::create_directories(output+"/lane_parity");
        LaneSegmenter other(reference_options,output+"/lane_parity");
        auto primary=model.capture(rgb.data()),reference=other.capture(rgb.data());
        auto decode=[](const RawLane& r,double conf){return r.quantized.empty()?lane4::decode_seg_fast(r.floating.data(),r.attr,"four",640,384,{0,12,640,360},conf):lane4::decode_four_i8(r.quantized.data(),r.attr,conf);};
        auto dump=[&](const RawLane& raw){
            bool native=!raw.quantized.empty();auto folder=output+"/lane_parity/"+(native?"native":"float");std::filesystem::create_directories(folder);
            evidence::ppm(folder+"/input.ppm",rgb.data(),640,384);
            std::ofstream meta(folder+"/tensors.json");meta<<"{\"native_output\":"<<(native?"true":"false")<<",\"width\":640,\"height\":384,\"tensors\":[";
            const void* data=native?static_cast<const void*>(raw.quantized.data()):static_cast<const void*>(raw.floating.data());
            evidence::tensor(meta,folder,0,raw.attr,data,native);meta<<"]}\n";
            auto classes=decode(raw,options.confidence);std::ofstream mask(folder+"/classes.pgm",std::ios::binary);mask<<"P5\n640 360\n255\n";
            mask.write(reinterpret_cast<const char*>(classes.data),classes.total());
            if(!meta || !mask)throw std::runtime_error("Cannot write lane parity evidence");
        };dump(primary);dump(reference);
        auto a=decode(primary,options.confidence),b=decode(reference,options.confidence);
        int mismatch=cv::countNonZero(a!=b);
        std::ofstream report(output+"/lane_parity.txt");report<<"same_input_class_mask_mismatches="<<mismatch<<" of230400, conf="<<options.confidence<<"\n";
        if(!report)throw std::runtime_error("Cannot write lane parity");
        if(mismatch)throw std::runtime_error("Float/native lane class-mask mismatch; use float until differences reviewed");
    }
    std::cout<<"PASS Small4 initialization/inference; FPGA live reception/accuracy not tested\n";
}
}
