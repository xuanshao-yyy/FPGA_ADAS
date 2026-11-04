// Decoder equivalence tests; synthetic tensors, no NPU accuracy claims.
#include "quant_lane.hpp"
#include "detection.hpp"
#include <cassert>
#include <random>
#include <iostream>
int main(){
    cv::setNumThreads(2);std::mt19937 rng(71);std::uniform_int_distribution<int> d(-128,127);
    for(bool nhwc:{false,true})for(float scale:{.03125f,.17f,1.f}){
        rknn_tensor_attr a{};a.n_dims=4;a.dims[0]=1;a.fmt=nhwc?RKNN_TENSOR_NHWC:RKNN_TENSOR_NCHW;
        a.dims[1]=nhwc?384:4;a.dims[2]=nhwc?640:384;a.dims[3]=nhwc?4:640;a.n_elems=4*384*640;
        a.type=RKNN_TENSOR_INT8;a.qnt_type=RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC;a.zp=-17;a.scale=scale;
        std::vector<int8_t> q(a.n_elems);std::vector<float> f(a.n_elems);
        for(auto& v:q)v=int8_t(d(rng));
        // Equal logits deliberately exercise first-class ties and exact .25 threshold.
        for(int x=0;x<640;++x)for(int c=0;c<4;++c)q[nhwc?((12*640+x)*4+c):((c*384+12)*640+x)]=3;
        for(size_t i=0;i<q.size();++i)f[i]=(int(q[i])-a.zp)*a.scale;
        for(double threshold:{.1,.25,.250001,.5,.7,1.}){
            auto reference=lane4::decode_seg_fast(f.data(),a,"four",640,384,{0,12,640,360},threshold);
            auto native=lane4::decode_four_i8(q.data(),a,threshold);
            assert(cv::countNonZero(reference!=native)==0);
        }
        auto bad=a;bad.scale=0;bool rejected=false;try{lane4::decode_four_i8(q.data(),bad,.5);}catch(...){rejected=true;}assert(rejected);
    }
    for(int height:{640,384})for(bool nhwc:{false,true}){
        std::array<ai::Tensor,9> fl{},qu{};std::array<std::vector<float>,9> f;
        std::array<std::vector<int8_t>,9> q;
        for(int k=0;k<9;++k){int w=640/(8<<(k/3)),h=height/(8<<(k/3)),c=k%3==0?64:k%3==1?8:1;
            q[k].assign(w*h*c,k%3==0?-90:-128);
            auto at=[&](int channel,int y,int x){return nhwc?(y*w+x)*c+channel:(channel*h+y)*w+x;};
            if(k==0)for(int side=0;side<4;++side)q[k][at(side*16+4,h/2,w/2)]=100;
            if(k==1)q[k][at(2,h/2,w/2)]=60;
            float scale=k%3==0?.1f:1.f/256;int zp=k%3==0?0:-128;
            for(auto v:q[k])f[k].push_back((int(v)-zp)*scale);
            fl[k]={f[k].data(),c,h,w,nhwc};qu[k]={nullptr,c,h,w,nhwc,q[k].data(),scale,zp};
        }
        auto a=ai::decode(fl,.1,.45,640,height),b=ai::decode(qu,.1,.45,640,height);
        assert(a.size()==1&&a.size()==b.size());assert(a[0].category==2);
        assert(a[0].score==b[0].score&&a[0].x1==b[0].x1&&a[0].y1==b[0].y1&&a[0].x2==b[0].x2&&a[0].y2==b[0].y2);
        assert(a[0].y1<360&&a[0].y2>360);
    }
    std::cout<<"PASS 36 full lane masks (NCHW/NHWC, ties, scales, thresholds) exact parity; square/rectangle detector DFL exact parity.\n";
}
