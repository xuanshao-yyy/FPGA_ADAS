#include "lane4_post.hpp"
#include "lane_position.hpp"
#include "lane_rules.hpp"
#include <fstream>
#include <iostream>
int main(int argc,char**argv){
    if(argc!=3)return 2;
    std::string root=argv[1];std::ifstream inputs(root+"/inputs.tsv");std::ofstream out(argv[2]);
    if(!inputs||!out)throw std::runtime_error("Missing replay input/output");
    out<<"frame_id,received_s,position_valid,offset,left_style,right_style,motion,trend,request_direction,rule,marking_permits,side_verified,allowed,alarm,raw_valid,raw_motion,filtered_offset,position_held,geometry_age_ms,reason,unknown_run_ms\n";
    lane4::Post post("camera_live.json",.5);lane_demo::State rules;unsigned id,count=0;double stamp;
    while(inputs>>id>>stamp){
        std::ifstream file(root+"/"+std::to_string(id)+".pgm",std::ios::binary);std::string magic;int w,h,max;file>>magic>>w>>h>>max;file.get();
        if(magic!="P5"||w!=640||h!=360||max!=255)throw std::runtime_error("Invalid class mask");
        cv::Mat classes(h,w,CV_8U);file.read(reinterpret_cast<char*>(classes.data),classes.total());if(!file)throw std::runtime_error("Short mask");
        auto mask=post.process(classes,cv::Mat::zeros(h,w,CV_8U),stamp,0,true);ai::lane_position(mask,320,.88*360);
        lane_demo::Input in;in.frame_id=in.source=id;in.received=stamp;in.valid=mask.position_valid&&mask.geometry.valid;
        in.left=-mask.left_margin;in.right=mask.right_margin;in.ego=0;in.offset=mask.relative_offset;in.left_style=mask.left_style;in.right_style=mask.right_style;
        in.left_valid=mask.left_curve.valid&&std::string(mask.left_evidence.reason)=="CONFIRMED_CANDIDATE";in.right_valid=mask.right_curve.valid&&std::string(mask.right_evidence.reason)=="CONFIRMED_CANDIDATE";
        rules.observe(in,stamp+.1);rules.select((count/12)%2?2:0);rules.request(); // scripted intent; never alters lane pixels or labels
        auto d=rules.decision(stamp+.1);
        out<<id<<','<<stamp<<','<<in.valid<<','<<in.offset<<','<<in.left_style<<','<<in.right_style<<','<<d.motion<<','<<d.trend<<','<<d.direction<<','<<d.rule<<','<<d.marking_permits<<','<<d.side_verified<<','<<d.allowed<<','<<d.alarm<<','<<d.raw_valid<<','<<d.raw_motion<<','<<d.offset<<','<<d.position_held<<','<<d.geometry_age_ms<<','<<d.reason<<','<<d.unknown_run_ms<<'\n';++count;
        if(d.allowed||d.side_verified)throw std::runtime_error("Replay has no adjacent-lane clearance evidence");
    }
    if(!count)throw std::runtime_error("Empty replay");
    std::cout<<"PASS multi-scene lane replay frames="<<count<<"; PC FP32 masks, no YOLO occlusion or NPU/accuracy claim; intent scripted, clearance unverified\n";
}
