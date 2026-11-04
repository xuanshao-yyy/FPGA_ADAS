#pragma once
#include "surround_risk.hpp"
#include "explanation_state.hpp"
#include "rk_dma.hpp"
#include "evidence_writer.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace product {
class Bridge {
    EvidenceWriter evidence;double last_capture=-100;std::string last_simulation,command_id;
    std::string directory,calibration,last_error,last_event,source_identity,near_error;
    std::filesystem::file_time_type near_modified{};
    double polled=-1;std::filesystem::file_time_type modified{};uint64_t event_seq=0;
    static std::string json(cv::FileStorage& fs){return fs.releaseAndGetString();}
    void save(const std::string& name,const std::string& data){auto path=directory+"/"+name;
        std::ofstream file(path+".tmp",std::ios::binary);file<<data;file.close();if(!file)throw std::runtime_error("Cannot write product state");
        std::filesystem::rename(path+".tmp",path);}
public:
    SurroundRisk risk;
    Bridge(const std::string& root,const std::string& profile):evidence(root.empty()?"":root+"/evidence"),directory(root),calibration(profile){
        if(directory.empty())return;
        risk.require_image_quality=true;
        std::filesystem::create_directories(directory);
        if(!calibration.empty())reload();
        reload_near();
    }
    bool enabled()const{return !directory.empty();}
    void display_ready(double now){if(enabled())save("display_ready.json","{\"pid\":"+std::to_string(getpid())+",\"monotonic_s\":"+std::to_string(now)+"}");}
    char take_command(double now){
        if(!enabled())return 0;
        auto path=directory+"/driver_command.json";if(!std::filesystem::exists(path))return 0;
        try{cv::FileStorage file(path,cv::FileStorage::READ);std::string key,id;double sent=0;int pid=0;
            file["key"]>>key;file["id"]>>id;file["sent_s"]>>sent;file["pid"]>>pid;file.release();std::filesystem::remove(path);
            bool ok=pid==int(getpid())&&key.size()==1&&std::string("awdjml scpnv").find(key[0])!=std::string::npos&&std::isfinite(sent)&&now>=sent&&now-sent<=2;
            command_id=id;if(!ok)ack_command(now,false);return ok?key[0]:0;
        }catch(const std::exception&){std::filesystem::remove(path);return 0;}
    }
    void ack_command(double now,bool applied=true){cv::FileStorage ack("ack.json",cv::FileStorage::WRITE|cv::FileStorage::MEMORY|cv::FileStorage::FORMAT_JSON);ack<<"id"<<command_id<<"applied"<<int(applied)<<"monotonic_s"<<now;save("driver_ack.json",json(ack));command_id.clear();}
    void reload(){try{auto time=std::filesystem::last_write_time(calibration);if(time!=modified){risk.configure(load_profile(calibration));modified=time;last_error.clear();check_source();}}
        catch(const std::exception& e){last_error=e.what();risk.configure(Profile{});}}
    void check_source(){if((risk.profile.zones[0].verified||risk.profile.zones[0].metric_verified)&&risk.profile.front_source!=source_identity){risk.profile.zones[0].verified=false;risk.profile.zones[0].metric_verified=false;last_error="前视源与标定不符，请重新确认 CH0 区域与距离标定";}}
    void reload_near(){try{auto path=directory+"/drive_near_rules.json";
        if(!std::filesystem::exists(path))return;
        auto stamp=std::filesystem::last_write_time(path);if(stamp!=near_modified){risk.configure_near_rules(load_near_rules(path));near_modified=stamp;near_error.clear();}}
        catch(const std::exception& e){near_error=e.what();parking::Rules disabled;disabled.enabled=false;risk.configure_near_rules(disabled);}}
    void observe(unsigned channel,const ai::Result& result,double now,bool still){if(enabled())risk.observe(channel,result,now,still);}
    void request_park(){if(enabled())save("mode_request.json","{\"mode\":\"PARK\",\"source\":\"native_ui\"}");}
    void request_panorama(){if(enabled())save("mode_request.json","{\"mode\":\"PANORAMA\",\"source\":\"native_ui\"}");}
    void front_source_changed(std::string source){if(enabled()){source_identity=std::move(source);risk.reset();modified={};reload();check_source();}}
    bool tick(double now,const driver_demo::Explanation& explanation,const std::array<double,4>& hz,
              const std::array<std::shared_ptr<qv::Frame>,4>& frames){
        if(!enabled()||now-polled<.5)return false;
        polled=now;
        if(!calibration.empty())reload();
        reload_near();
        for(unsigned ch=0;ch<4;++ch){auto f=frames[ch];bool good=false;
            if(f&&now>=f->received&&now-f->received<.75){
                auto memory=std::dynamic_pointer_cast<qv::DmaBuffer>(f->memory);if(memory)memory->begin_read();
                int lo=255,hi=0,total=0,count=0;
                for(unsigned y=60;y<720;y+=80)for(unsigned x=50;x<1280;x+=100){auto p=f->pixels.get()+(y*1280+x)*2;unsigned v=p[0]|(unsigned(p[1])<<8);int l=int((3*((v>>11)&31)*255/31+6*((v>>5)&63)*255/63+(v&31)*255/31)/10);lo=std::min(lo,l);hi=std::max(hi,l);total+=l;++count;}
                if(memory)memory->finish_read();
                good=hi-lo>=12&&total>count*5&&total<count*250;
            }
            risk.image_quality(ch,good,f?f->received:-1);
        }
        cv::FileStorage state("state.json",cv::FileStorage::WRITE|cv::FileStorage::MEMORY|cv::FileStorage::FORMAT_JSON);
        state<<"pid"<<int(getpid())<<"monotonic_s"<<now<<"calibration_id"<<risk.profile.id<<"calibration_error"<<(last_error+near_error)<<"source_identity"<<source_identity;
        state<<"road_intent"<<explanation.intent<<"vehicle_intent"<<(explanation.lane_requested?explanation.lane_intent:explanation.intent)<<"vehicle_state"<<explanation.vehicle_state<<"vehicle_label"<<explanation.vehicle_label<<"vehicle_reason"<<explanation.vehicle_reason
            <<"vehicle_arrow"<<explanation.vehicle_arrow<<"vehicle_moving"<<int(explanation.vehicle_moving)<<"vehicle_alarm"<<int(explanation.vehicle_alarm)<<"manual_stop"<<int(explanation.manual_stop)<<"evidence_error"<<evidence.error();
        state<<"action"<<explanation.action<<"reason"<<explanation.reason<<"lane"<<explanation.lane<<"lane_request"<<explanation.lane_request<<"lane_requested"<<int(explanation.lane_requested)<<"lane_intent"<<explanation.lane_intent;
        state<<"channels"<<"[";std::string key;
        for(unsigned i=0;i<4;++i){auto v=risk.decision(i,now);
            state<<"{"<<"channel"<<int(i)<<"hz"<<hz[i]<<"fresh"<<int(v.fresh)<<"calibrated"<<int(v.calibrated)<<"occupied"<<int(v.occupied)<<"approaching"<<int(v.approaching)<<"target"<<std::to_string(v.target)<<"tau_image_s"<<v.tau<<"distance_m"<<v.distance<<"person_level"<<v.person<<"received_s"<<v.received<<"reason"<<v.reason
                <<"image_clear"<<int(v.image_clear)<<"clear_age_ms"<<v.clear_age_ms<<"image_fresh"<<int(v.image_fresh)<<"proximity_level"<<v.proximity<<"proximity_held"<<int(v.proximity_held)<<"proximity_area"<<v.proximity_area<<"image_approaching"<<int(v.image_approaching)<<"rx_frame_id"<<(frames[i]?std::to_string(frames[i]->id):"")<<"rx_source_id"<<(frames[i]?std::to_string(frames[i]->source):"")
                <<"rx_age_ms"<<(frames[i]?(now-frames[i]->received)*1000:-1)<<"alert_active"<<int(driver_demo::proximity_alert_active(explanation,i))<<"}";
            key+=std::to_string(v.occupied)+std::to_string(v.approaching)+std::to_string(v.person)+std::to_string(v.proximity)+std::to_string(v.image_approaching);
        }
        state<<"]";save("perception.json",json(state));
        std::string simulation=explanation.vehicle_state+":"+explanation.vehicle_reason;
        bool manual=std::filesystem::exists(directory+"/evidence.request");if(manual)std::filesystem::remove(directory+"/evidence.request");
        bool changed=key!=last_event||simulation!=last_simulation;
        if(changed||manual){
            std::string image_id;unsigned channel=0;
            for(unsigned ch=1;ch<4;++ch)if(driver_demo::proximity_alert_active(explanation,ch)&&explanation.near_fresh[ch]&&explanation.near_levels[ch]>=2){channel=ch;break;}
            if(manual&&(!frames[channel]||now-frames[channel]->received>.75))for(unsigned ch=0;ch<4;++ch)if(frames[ch]&&now>=frames[ch]->received&&now-frames[ch]->received<=.75){channel=ch;break;}
            bool hazard=explanation.vehicle_alarm||explanation.near_levels[channel]>=2;
            if((manual||(hazard&&now-last_capture>=10))&&frames[channel]&&now>=frames[channel]->received&&now-frames[channel]->received<=.75){
                auto id=std::to_string(getpid())+"-"+std::to_string(uint64_t(now*1000000));
                if(evidence.submit(id,frames[channel])){image_id=id;last_capture=now;}
            }
            if(manual||simulation!=last_simulation||key.find_first_not_of('0')!=std::string::npos||!last_event.empty()){
                cv::FileStorage event("event.json",cv::FileStorage::WRITE|cv::FileStorage::MEMORY|cv::FileStorage::FORMAT_JSON);
                event<<"event_id"<<std::to_string(getpid())+"-"+std::to_string(uint64_t(now*1000000))+"-"+std::to_string(++event_seq)<<"monotonic_s"<<now
                    <<"type"<<(manual?"MANUAL_EVIDENCE":simulation!=last_simulation?"VEHICLE_STATE":"RISK_CHANGE")<<"state"<<key<<"calibration_id"<<risk.profile.id
                    <<"vehicle_state"<<explanation.vehicle_state<<"vehicle_label"<<explanation.vehicle_label<<"severity"<<explanation.severity<<"reason"<<explanation.reason<<"action"<<explanation.action
                    <<"image_id"<<image_id<<"channel"<<int(channel)<<"frame_id"<<(frames[channel]?std::to_string(frames[channel]->id):"")<<"source_id"<<(frames[channel]?std::to_string(frames[channel]->source):"")
                    <<"image_note"<<(image_id.empty()?"没有保存图片：等待新鲜画面、写入空闲或采样间隔":"事件附近接收帧，不保证与推理帧完全相同");
                std::string row=json(event);row.erase(std::remove(row.begin(),row.end(),'\n'),row.end());auto path=directory+"/events.jsonl";
                if(std::filesystem::exists(path)&&std::filesystem::file_size(path)>2*1024*1024)std::filesystem::rename(path,directory+"/events.previous.jsonl");
                std::ofstream log(path,std::ios::app);log<<row<<'\n';
            }
            last_event=key;last_simulation=simulation;
        }
        if(std::filesystem::exists(directory+"/snapshot.request")){
            cv::FileStorage meta("snap.json",cv::FileStorage::WRITE|cv::FileStorage::MEMORY|cv::FileStorage::FORMAT_JSON);
            meta<<"monotonic_s"<<now<<"source_identity"<<source_identity<<"channels"<<"[";
            for(unsigned i=0;i<4;++i){auto f=frames[i];meta<<"{"<<"valid"<<int(f&&now>=f->received&&now-f->received<1)<<"frame"<<(f?std::to_string(f->id):"")<<"source"<<(f?std::to_string(f->source):"")<<"}";}
            meta<<"]";
            for(unsigned i=0;i<4;++i)if(frames[i]){auto& f=frames[i];auto path=directory+"/snapshot"+std::to_string(i)+".rgb565";
                // Frame memory is the original RGB565 source, copied only on an explicit calibration request.
                auto memory=std::dynamic_pointer_cast<qv::DmaBuffer>(f->memory);
                if(memory)memory->begin_read();
                std::ofstream out(path+".tmp",std::ios::binary);out.write(reinterpret_cast<const char*>(f->pixels.get()),qv::FrameBytes);out.close();
                if(memory)memory->finish_read();
                if(out)std::filesystem::rename(path+".tmp",path);
            }
            save("snapshot_meta.json",json(meta));
            std::filesystem::remove(directory+"/snapshot.request");
        }
        return true;
    }
};
}
