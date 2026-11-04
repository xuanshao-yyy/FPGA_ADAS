#include "lane_hybrid.hpp"
#include <fstream>
#include <iostream>
cv::Mat pgm(const std::string& path){
    std::ifstream f(path,std::ios::binary);std::string magic;int w,h,max;f>>magic>>w>>h>>max;f.get();
    if(magic!="P5"||w!=640||h!=360||max!=255)throw std::runtime_error("PGM contract");
    cv::Mat m(h,w,CV_8U);f.read(reinterpret_cast<char*>(m.data),m.total());if(!f)throw std::runtime_error("Short PGM");return m;
}
Curve curve(const cv::FileNode& n){
    Curve c;c.valid=int(n["valid"]);std::vector<double> values;n["coefficients"]>>values;
    if(values.size()!=3)throw std::runtime_error("Curve coefficients");std::copy(values.begin(),values.end(),c.coef.begin());
    c.ymin=double(n["ymin"]);c.ymax=double(n["ymax"]);c.inlier_ratio=double(n["inlier_ratio"]);return c;
}
int main(int argc,char**argv){
    if(argc!=3)return 2;std::string root=argv[1];cv::FileStorage file(root+"/events.json",cv::FileStorage::READ);
    std::ifstream gray_file(root+"/gray.bin",std::ios::binary);std::ofstream out(argv[2]);
    out<<"frame_id,accepted,reason,model_frame_id,position,offset,left_a,left_b,left_c,left_min,left_max,right_a,right_b,right_c,right_min,right_max\n";
    ai::HybridLane tracker("camera_live.json",.5,750);double seeded=-1;unsigned attempts=0,accepted=0;
    for(auto event:file["events"]){int fid=int(event["fid"]);double stamp=double(event["received"]);std::string kind;event["kind"]>>kind;
        cv::Mat gray(180,320,CV_8U);gray_file.seekg((fid-1)*320*180);gray_file.read(reinterpret_cast<char*>(gray.data),gray.total());
        if(!gray_file)throw std::runtime_error("Short gray source");
        if(kind=="seed"){
            ai::LaneMask m;m.received=m.model_received=stamp;m.frame_id=m.model_frame_id=fid;m.source=fid;m.four_class=true;
            m.geometry.valid=int(event["pair_valid"]);m.left_curve=curve(event["left"]);m.right_curve=curve(event["right"]);
            m.left_style=int(event["left"]["style"]);m.right_style=int(event["right"]["style"]);
            m.left_evidence.confirmation_streak=int(event["left"]["confirmation_streak"]);m.right_evidence.confirmation_streak=int(event["right"]["confirmation_streak"]);
            auto classes=pgm(root+"/classes_"+std::to_string(fid)+".pgm"),blocked=pgm(root+"/seed_blocked_"+std::to_string(fid)+".pgm");
            m.native_classes.assign(classes.data,classes.data+classes.total());m.native_blocked.assign(blocked.data,blocked.data+blocked.total());tracker.seed(gray,m);seeded=stamp;continue;
        }
        if(stamp<=seeded)continue;
        auto update=tracker.update(gray,pgm(root+"/blocked_"+std::to_string(fid)+".pgm"),stamp);++attempts;accepted+=update.accepted;
        if(update.accepted && (double(event["available"])-update.mask.model_received)*1000>750)throw std::runtime_error("Accepted replay model expired at availability");
        auto m=update.mask;out<<fid<<','<<update.accepted<<','<<update.reason<<','<<m.model_frame_id<<','<<(update.accepted?m.position:"UNKNOWN")<<','<<m.relative_offset;
        for(auto c:{m.left_curve,m.right_curve})out<<','<<c.coef[0]<<','<<c.coef[1]<<','<<c.coef[2]<<','<<c.ymin<<','<<c.ymax;
        out<<'\n';
    }
    if(!out||attempts==0)throw std::runtime_error("No replay output");
    std::cout<<"Recorded-anchor CPU replay: attempts="<<attempts<<" accepted="<<accepted<<"; no NPU, accuracy or board speed measurement\n";
}
