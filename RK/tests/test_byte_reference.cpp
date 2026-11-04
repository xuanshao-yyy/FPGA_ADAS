#include "ByteTrack/BYTETracker.h"
#include <array>
#include <cassert>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>
std::vector<double> fields(const std::string& line){std::vector<double> v;std::istringstream row(line);std::string part;while(std::getline(row,part,','))v.push_back(std::stod(part));return v;}
int main(){
    std::map<int,std::vector<byte_track::Object>> inputs;
    std::map<int,std::map<size_t,std::array<double,4>>> reference;
    std::ifstream detections("tests/detection_results.csv"),expected("tests/tracking_results.csv");assert(detections && expected);
    std::string line;int last=0;
    while(std::getline(detections,line)){auto v=fields(line);assert(v.size()==6);last=std::max(last,int(v[0]));
        inputs[int(v[0])].emplace_back(byte_track::Rect<float>(v[2],v[3],v[4],v[5]),0,v[1]);}
    while(std::getline(expected,line)){auto v=fields(line);assert(v.size()==6);last=std::max(last,int(v[0]));reference[int(v[0])][size_t(v[1])]={v[2],v[3],v[4],v[5]};}
    byte_track::BYTETracker tracker;size_t matches=0;
    for(int frame=0;frame<=last;++frame){auto result=tracker.update(inputs[frame]);assert(result.size()==reference[frame].size());
        for(auto t:result){auto it=reference[frame].find(t->getTrackId());assert(it!=reference[frame].end());auto r=t->getRect();
            std::array<double,4> values{r.x(),r.y(),r.width(),r.height()};
            for(size_t k=0;k<4;++k){assert(std::abs(values[k]-it->second[k])<.01);}++matches;}}
    std::cout<<"PASS: upstream ByteTrack reference "<<last+1<<" frames, "<<matches<<" track boxes (IDs/count/coordinates)\n";
}
