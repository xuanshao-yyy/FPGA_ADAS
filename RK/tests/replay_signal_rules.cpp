#include "signal_control.hpp"
#include <filesystem>
// Replay saved detector/color/shape observations, not new NPU inference.
int main(int argc,char** argv){try{
    if(argc!=3)return 2;
    namespace fs=std::filesystem;fs::path root=argv[2];if(!fs::create_directory(root))throw std::runtime_error("Preserve replay output");
    signal_demo::Config strict;strict.automatic=true;strict.practical_association=false;strict.settings.stop_hold_ms=1500;
    auto practical=strict;practical.practical_association=true;
    fs::create_directory(root/"arrow");fs::create_directory(root/"practical");
    signal_demo::Control a(strict,(root/"arrow").string()),b(practical,(root/"practical").string());
    std::ifstream file(argv[1]);if(!file)throw std::runtime_error("Missing replay stream");
    std::ofstream out(root/"comparison.csv");out<<"received_s,now_s,frame_id,source,direction,arrow,practical,practical_signal,practical_detail,practical_action\n";
    double received,now;uint32_t id,source;unsigned reset,n;uint64_t count=0,violations=0;
    while(file>>received>>now>>id>>source>>reset>>n){
        if(n>64||received>now)throw std::runtime_error("Invalid replay metadata");
        if(reset){a.reset_scene(strict,received);b.reset_scene(practical,received);}
        ai::Result frame;frame.received=received;frame.finished=now;frame.frame_id=id;frame.source=source;
        for(unsigned i=0;i<n;++i){ai::SignalCandidate c;c.box.category=7;
            if(!(file>>c.box.x1>>c.box.y1>>c.box.x2>>c.box.y2>>c.box.score>>c.track_id>>c.direction>>c.color>>c.shape>>c.reason))throw std::runtime_error("Incomplete replay candidate");
            frame.signal_candidates.push_back(c);}
        a.observe_automatic(frame,now);b.observe_automatic(frame,now);a.observe_objects(frame,now);b.observe_objects(frame,now);a.tick(now);b.tick(now);
        auto explanation=b.explanation(now);
        for(unsigned d=0;d<3;++d){
            auto direction=signal_demo::Direction(d);auto value=b.permission(direction,now);
            out<<std::fixed<<std::setprecision(9)<<received<<','<<now<<','<<id<<','<<source<<','<<signal_demo::name(direction)<<','<<a.permission(direction,now)<<','<<value<<','
                <<explanation.signal[d]<<','<<explanation.signal_detail[d]<<','<<explanation.action<<'\n';
            for(const auto& c:frame.signal_candidates)if(c.direction==int(d)&&c.color=="RED"&&std::string(value)=="ALLOW")++violations;
        }
        ++count;
    }
    if(!count||violations)throw std::runtime_error("No replay observations or explicit red overridden");
    std::cout<<"REPLAY frames="<<count<<" explicit_red_overridden="<<violations<<" accuracy_ground_truth=false\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
