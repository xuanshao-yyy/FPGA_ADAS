#include "lane4_post.hpp"
#include "visual.hpp"
#include <cassert>
#include <iostream>
int main(int argc,char** argv){
    std::vector<float> logits(4*640*384,-4);
    for(int y=0;y<384;++y)for(int x=0;x<640;++x){int c=y<12||y>=372?3:x<200?1:x<400?2:0;logits[(c*384+y)*640+x]=4;}
    rknn_tensor_attr attr{};attr.n_dims=4;attr.dims[0]=1;attr.dims[1]=4;attr.dims[2]=384;attr.dims[3]=640;attr.n_elems=logits.size();attr.fmt=RKNN_TENSOR_NCHW;
    auto classes=lane4::decode_seg_fast(logits.data(),attr,"four",640,384,{0,12,640,360},.5);
    for(int y=0;y<360;++y)for(int x=0;x<640;++x)assert(classes.at<uint8_t>(y,x)==(x<200?1:x<400?2:0));
    logits[0]=std::numeric_limits<float>::infinity();assert(!lane4::finite_output(logits.data(),logits.size()));
    std::vector<ai::Box> boxes{{200,200,400,400,.9f,2},{700,200,800,400,.9f,7}};
    auto blocked=lane4::vehicle_mask(boxes,{});assert(blocked.at<uint8_t>(150,150)==255 && blocked.at<uint8_t>(150,375)==0);
    lane4::Post post(argc>1?argv[1]:"camera_live.json",.5f);
    auto visible=post.process(classes,blocked,1.,0,false);assert(visible.four_class && visible.blocked_pixels>0 && visible.data.size()==576*324);
    assert(visible.data[135*576+135]==0);
    std::vector<uint8_t> canvas(1280*720*3/2+64,16);std::fill(canvas.end()-64,canvas.end(),0xa5);
    visible.received=qv::now_seconds();visible.finished=visible.received;
    visible.left_curve.valid=true;visible.left_curve.coef={0,0,150};visible.left_curve.ymin=0;visible.left_curve.ymax=359;
    ui::lane_overlay(canvas.data(),0,true,&visible,4,750);
    for(auto it=canvas.end()-64;it!=canvas.end();++it)assert(*it==0xa5);
    assert(canvas[(32+135)*1280+32+135]==16); // blocked area must remain unpainted
    auto before=canvas;visible.received-=2;ui::lane_overlay(canvas.data(),0,true,&visible,4,750);
    // Stale status text is allowed; no out-of-bounds paint.
    for(auto it=canvas.end()-64;it!=canvas.end();++it)assert(*it==0xa5);
    cv::Mat blank=cv::Mat::zeros(360,640,CV_8U);auto unknown=post.process(blank,blank,1.1,0,true);assert(!unknown.geometry.valid && !unknown.left_curve.valid && !unknown.right_curve.valid);
    // Clear per-boundary model classes do not wait three low-rate observations.
    lane4::Post clear_post("camera_wide.json",.5f);cv::Mat lanes=cv::Mat::zeros(360,640,CV_8U);
    for(int y=180;y<340;++y){for(int x=178;x<=182;++x)lanes.at<uint8_t>(y,x)=1;for(int x=458;x<=462;++x)lanes.at<uint8_t>(y,x)=2;}
    auto clear=clear_post.process(lanes,blank,100.,0,true);
    assert(clear.left_style==1&&clear.right_style==2&&clear.left_evidence.direct&&clear.right_evidence.direct);
    // A gap longer than the geometric smoothing gap does not restart class waiting for clear evidence.
    clear=clear_post.process(lanes,blank,101.,0,true);assert(clear.left_style==1&&clear.right_style==2);
    cv::Mat occlusion=cv::Mat::zeros(360,640,CV_8U);occlusion(cv::Rect(175,230,12,65)).setTo(255);
    auto hidden=clear_post.process(lanes,occlusion,101.5,1,true);assert(hidden.left_style==0&&!hidden.left_evidence.direct&&hidden.right_style==2);
    auto mixed=lanes.clone();for(int y=180;y<260;++y)for(int x=178;x<=182;++x)mixed.at<uint8_t>(y,x)=2;
    auto uncertain=clear_post.process(mixed,blank,102.,2,true);assert(uncertain.left_style==0&&!uncertain.left_evidence.direct);
    std::cout<<"PASS Small4 decode, finite checks, masking, immediate high-support classes at low cadence, mixed/occluded rejection and overlay bounds\n";
}
