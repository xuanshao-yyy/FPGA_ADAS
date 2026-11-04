#include "tracking.hpp"
#include "lane_geometry.hpp"
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

int main(int argc,char**argv){try{
    if(argc!=4)throw std::runtime_error("replay_algorithms detections.jsonl lane_mask.pgm output.json");
    std::ifstream input(argv[1]);if(!input)throw std::runtime_error("Missing detections log");
    std::regex channel_re("\"channel\":([0-9]+)"),stamp_re("\"received_monotonic_s\":([0-9.]+)");
    std::regex box_re("\"class\":([0-9]+),\"score\":([0-9.]+),\"xyxy\":\\[([0-9.eE+-]+),([0-9.eE+-]+),([0-9.eE+-]+),([0-9.eE+-]+)\\]");
    std::array<ai::RoadTracker,4> trackers;std::array<size_t,4> records{},observed{},predicted{};
    std::array<std::set<uint64_t>,4> ids;double total_ms=0,max_ms=0;std::string line;
    while(std::getline(input,line)){
        std::smatch cm,tm;if(!std::regex_search(line,cm,channel_re)||!std::regex_search(line,tm,stamp_re))throw std::runtime_error("Invalid log record");
        unsigned channel=unsigned(std::stoul(cm[1]));if(channel>3)throw std::runtime_error("Invalid channel");
        double stamp=std::stod(tm[1]);std::vector<ai::Box> boxes;
        for(auto i=std::sregex_iterator(line.begin(),line.end(),box_re);i!=std::sregex_iterator();++i){const auto& m=*i;
            boxes.push_back({std::stof(m[3]),std::stof(m[4]),std::stof(m[5]),std::stof(m[6]),std::stof(m[2]),std::stoi(m[1])});}
        auto start=std::chrono::steady_clock::now();auto tracks=trackers[channel].update(boxes,stamp);
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();total_ms+=ms;max_ms=std::max(max_ms,ms);
        ++records[channel];for(const auto& t:tracks){++(t.predicted?predicted[channel]:observed[channel]);ids[channel].insert(t.id);}
    }
    std::ifstream pgm(argv[2],std::ios::binary);std::string magic;int w=0,h=0,maximum=0;pgm>>magic>>w>>h>>maximum;pgm.get();
    if(magic!="P5"||w!=576||h!=324||maximum!=255)throw std::runtime_error("Expected saved P5 576x324 lane mask");
    std::vector<uint8_t> mask(size_t(w*h));pgm.read(reinterpret_cast<char*>(mask.data()),std::streamsize(mask.size()));
    if(!pgm)throw std::runtime_error("Short lane mask");
    auto start=std::chrono::steady_clock::now();auto g=ai::extract_lane_geometry(mask,w,h);
    double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::ofstream out(argv[3]);out<<std::fixed<<std::setprecision(6)<<"{\n\"scope\":\"offline replay of historical board outputs on x86; not RK3568 latency or accuracy\",\n\"channels\":[";
    size_t count=0;for(unsigned k=0;k<4;++k){if(k)out<<',';count+=records[k];out<<"{\"channel\":"<<k<<",\"updates\":"<<records[k]<<",\"observed_track_outputs\":"<<observed[k]
        <<",\"predicted_track_outputs\":"<<predicted[k]<<",\"distinct_confirmed_ids\":"<<ids[k].size()<<'}';}
    out<<"],\n\"mean_tracking_ms_x86\":"<<(count?total_ms/count:0)<<",\n\"max_tracking_ms_x86\":"<<max_ms<<",\n\"saved_lane_mask\":{\"valid_pair\":"<<(g.valid?"true":"false")
        <<",\"left_valid\":"<<(g.left.valid?"true":"false")<<",\"right_valid\":"<<(g.right.valid?"true":"false")<<",\"geometry_ms_x86\":"<<ms
        <<",\"left_slope\":"<<g.left.slope<<",\"left_intercept\":"<<g.left.intercept<<",\"right_slope\":"<<g.right.slope<<",\"right_intercept\":"<<g.right.intercept<<"}}\n";
    if(!out)throw std::runtime_error("Cannot write replay report");
    std::cout<<"Replayed "<<count<<" records; saved lane pair="<<g.valid<<"; report="<<argv[3]<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
