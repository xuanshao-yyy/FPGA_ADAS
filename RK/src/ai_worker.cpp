#include "ai_worker.hpp"
#include "ai_schedule.hpp"
#include "lane_runtime.hpp"
#include "rk_dma.hpp"
#include "live_tracking.hpp"
#include "lane4_core.hpp"
#include "detector_runtime.hpp"
#include "light_candidate.hpp"
#include "signal_shape.hpp"
#include "light_rgb_runtime.hpp"
#include "lane_hybrid.hpp"
#include <rknn_api.h>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace ai {
namespace {
double milliseconds(double start){return (qv::now_seconds()-start)*1000;}

}

std::string select_model(const std::string& directory,const std::string& explicit_path){
    namespace fs=std::filesystem;
    if(!explicit_path.empty()){
        if(!fs::is_regular_file(explicit_path))throw std::runtime_error("Model not found: "+explicit_path);
        return fs::absolute(explicit_path).string();
    }
    if(!fs::is_directory(directory))throw std::runtime_error("Model directory not found: "+directory);
    if(fs::is_regular_file(fs::path(directory)/"traffic_yolo11n_640x640_i8.rknn"))return (fs::path(directory)/"traffic_yolo11n_640x640_i8.rknn").string();
    for(const char* ending:{"i8","fp"}){
        std::string expected="bdd8_yolov8n_640_rk3568_"+std::string(ending)+".rknn";
        if(fs::is_regular_file(fs::path(directory)/expected))return (fs::path(directory)/expected).string();
    }
    throw std::runtime_error("Expected bdd8_yolov8n_640_rk3568_i8.rknn or _fp.rknn in "+directory+"; inspect full filenames and use --model /actual/file.rknn");
}
void verify_model(const std::string& model,const std::string& output,unsigned warmup,const std::string& probe,bool native,bool parity){
    Detector detector(model,output,native);std::vector<uint8_t> rgb(detector.width()*detector.height()*3,114);
    if(!probe.empty()){
        std::ifstream file(probe,std::ios::binary|std::ios::ate);
        if(!file || file.tellg()!=640*640*3)throw std::runtime_error("Probe requires square640 RGB reference");
        std::vector<uint8_t> square(640*640*3);file.seekg(0);file.read(reinterpret_cast<char*>(square.data()),square.size());
        if(!file)throw std::runtime_error("Incomplete RGB probe");
        int from=(640-detector.height())/2;
        std::copy(square.begin()+from*640*3,square.begin()+(from+detector.height())*640*3,rgb.begin());
    }
    std::vector<Box> boxes;double api=0,post=0;
    for(unsigned k=0;k<std::max(1u,warmup);++k)boxes=detector.infer(rgb.data(),.1f,.45f,api,post);
    if(parity){
        const auto folder=output+"/detector_parity";std::filesystem::create_directories(folder);
        boxes=detector.infer(rgb.data(),.1f,.45f,api,post,false,folder+(native?"/native":"/float"));
        Detector other(model,output,!native,"parity_model_info.txt");
        auto reference=other.infer(rgb.data(),.1f,.45f,api,post,false,folder+(native?"/float":"/native"));
        bool pass=reference.size()==boxes.size();size_t mismatch=0;
        for(size_t i=0;i<std::min(boxes.size(),reference.size());++i){auto a=boxes[i],b=reference[i];
            if(a.category!=b.category || std::abs(a.score-b.score)>1e-6 || std::abs(a.x1-b.x1)>.02 || std::abs(a.y1-b.y1)>.02 || std::abs(a.x2-b.x2)>.02 || std::abs(a.y2-b.y2)>.02){pass=false;++mismatch;}}
        std::ofstream report(output+"/detector_parity.txt");report<<(pass?"PASS":"FAIL")<<" strict ordered same-input decoded boxes, score<=1e-6 coordinate<=.02 pixels; primary="<<boxes.size()<<" reference="<<reference.size()<<" ordered_mismatch="<<mismatch<<"\nRaw inputs/tensors/candidates/boxes: detector_parity/; inspect with inspect_parity.py. Tolerances unchanged.\n";
        report.flush();if(!report)throw std::runtime_error("Cannot write parity report");
        if(!pass)throw std::runtime_error("Float/native detection parity failed; diagnostic evidence saved");
    }
    if(!probe.empty() && boxes.empty())throw std::runtime_error("Known road probe produced zero boxes; inspect saved parity evidence if requested");
    std::cout<<"PASS RKNN input/output and inference; "<<detector.width()<<"x"<<detector.height()<<" boxes="<<boxes.size()<<" native="<<native<<" parity="<<parity<<". Not live accuracy/speed validation.\n";
}
struct Worker::Impl {
    Detector detector;
    std::unique_ptr<Detector> front_detector;
    qv::DmaBuffer input,lane_input,front_input;
    int rgb_format,input_format;
    std::array<bool,4> channels;
    float confidence,iou;unsigned max_age_ms;
    mutable std::mutex mutex;std::condition_variable ready;
    std::array<std::shared_ptr<qv::Frame>,4> pending;
    std::array<std::shared_ptr<qv::Frame>,4> mosaic_cache;
    std::array<Result,4> results;std::array<Stats,4> statistics,lane_statistics;
    std::array<std::shared_ptr<const LaneMask>,4> lane_results;
    std::thread thread;bool stopping=false,mosaic=false,diagnose=false;
    uint64_t inferences=0,job_sequence=0;std::string failure,output_directory;
    LaneOptions lane_options;TrackingOptions tracking_options;LiveOptions live;
    std::unique_ptr<LaneSegmenter> lane_model;
    std::unique_ptr<LightRGB> light_rgb;
    std::ofstream light_rgb_records;
    std::array<LiveTracker,4> trackers;
    std::array<uint32_t,4> track_frames{};std::array<bool,4> track_started{};
    std::array<bool,5> dumped{};bool lane_dumped=false;
    std::ofstream records,track_records,lane_records,schedule_records,detector_records;
    uint64_t deep_count=0,mosaic_count=0;
    std::array<uint64_t,4> detection_evidence_count{};
    std::ofstream light_records,front_evidence_records,automatic_signal_records;
    std::ofstream sample_records;
    LiveSchedule schedule;
    double shallow_weight=4,deep_weight=3,last_lane_start=-1;
    std::atomic<bool> hybrid_needs_model{true},hybrid_model_pending{false};
    std::unique_ptr<lane4::Config> position_config;
    std::unique_ptr<HybridLane> hybrid_tracker;
    struct HybridTask {std::shared_ptr<qv::Frame> frame;Result detection;std::shared_ptr<const LaneMask> seed;};
    std::mutex hybrid_mutex;std::condition_variable hybrid_ready;std::thread hybrid_thread;
    std::unique_ptr<HybridTask> hybrid_pending,hybrid_seed;
    bool hybrid_stopping=false;
    HybridStats hybrid_statistics;
    std::shared_ptr<const LaneMask> hybrid_result;
    std::ofstream hybrid_records;
    uint64_t hybrid_sample_count=0;
    struct CpuLaneTask {
        unsigned channel=0;std::shared_ptr<qv::Frame> frame;Result detection;RawLane raw;
        std::vector<uint8_t> rgb;double begin=0,prep=0,enqueued=0;
    };
    std::mutex cpu_mutex;std::condition_variable cpu_ready;std::thread cpu_thread;
    std::unique_ptr<CpuLaneTask> cpu_pending;bool cpu_stopping=false;
    Impl(const std::string& model,const std::string& heap,int format,const std::array<bool,4>& enabled,
         double rate,float conf,float threshold,unsigned age,const std::string& output,unsigned warmup,bool mosaic_mode,bool diagnostic_mode,
         const LaneOptions& lane,const TrackingOptions& tracking,const LiveOptions& priority)
        :detector(model,output,priority.native_output),input(heap,640*640*3),lane_input(heap,640*384*3),front_input(heap,640*384*3),rgb_format(format),
         input_format(qv::verify_rga_rgb888(heap,format)),channels(enabled),confidence(conf),iou(threshold),max_age_ms(age),
         mosaic(mosaic_mode),diagnose(diagnostic_mode),output_directory(output),lane_options(lane),tracking_options(tracking),live(priority){
        if(rate!=0 || lane.fps!=0)throw std::runtime_error("v9 uses unlimited time-share scheduling: ai-fps/lane-fps must be 0");
        if(detector.height()!=640)throw std::runtime_error("Main/side detector must use square640; use front-model for rectangle");
        if(!live.front_model.empty()){
            front_detector=std::make_unique<Detector>(live.front_model,output,live.native_output,"front_model_info.txt");
            if(front_detector->height()!=384)throw std::runtime_error("Front model must be 640x384");
        }
        if(!live.light_rgb_model.empty()){
            light_rgb=std::make_unique<LightRGB>(live.light_rgb_model,output,live.light_rgb_probability,live.light_rgb_margin);
            light_rgb_records.open(output+"/light_rgb.csv");
            light_rgb_records<<"frame_id,received_s,track_id,detector_score,hsv,network,final,red_probability,yellow_probability,green_probability,probability,margin,reason,prep_ms,api_ms\n";
            if(!light_rgb_records)throw std::runtime_error("Cannot write RGB classification log");
        }
        shallow_weight=live.shallow_weight;deep_weight=live.deep_weight;
        for(auto& t:trackers)t=LiveTracker(confidence,tracking_options.light_confidence);
        records.open(output+"/detections.jsonl");schedule_records.open(output+"/schedule.csv");
        detector_records.open(output+"/detector_metrics.csv");
        detector_records<<"inference_id,input_width,input_height,mosaic,native_output,inputs_ms,run_ms,get_ms,validate_ms,decode_ms,release_ms\n";
        if(!records||!schedule_records||!detector_records)throw std::runtime_error("Cannot open detection/schedule logs");
        if(live.save_every){std::filesystem::create_directories(output+"/frames");sample_records.open(output+"/frames/index.csv");
            sample_records<<"sample,channel,frame_id,source,received_monotonic_s\n";if(!sample_records)throw std::runtime_error("Cannot open saved frame index");}
        if(live.light_candidates){light_records.open(output+"/light_candidates.jsonl");if(!light_records)throw std::runtime_error("Cannot open light evidence");}
        if(live.automatic_signal){automatic_signal_records.open(output+"/automatic_signal_candidates.jsonl");if(!automatic_signal_records)throw std::runtime_error("Cannot open automatic signal evidence");}
        if(live.save_every){std::filesystem::create_directories(output+"/detection_frames");front_evidence_records.open(output+"/detection_frames/index.csv");
            front_evidence_records<<"sample,inference_id,channel,frame_id,source,received_monotonic_s\n";if(!front_evidence_records)throw std::runtime_error("Cannot open front evidence index");}
        schedule_records<<"job_sequence,job,channel,frame_id,queue_age_ms,service_ms,weight,detection_frames,lane_frames,forced_deadline\n";
        if(tracking.enabled){track_records.open(output+"/tracks.jsonl");if(!track_records)throw std::runtime_error("Cannot open track log");}
        if(!lane.model.empty()){
            lane_model=std::make_unique<LaneSegmenter>(lane,output);
            position_config=std::make_unique<lane4::Config>(lane.camera);
            if(live.lane_hybrid){
                hybrid_tracker=std::make_unique<HybridLane>(lane.camera,lane.ego_x,live.hybrid_semantic_ms);
                hybrid_records.open(output+"/lane_hybrid.csv");
                hybrid_records<<"frame_id,source,received_monotonic_s,finished_monotonic_s,accepted,reason,model_frame_id,model_received_s,semantic_age_ms,queue_ms,gray_ms,flow_ms,fit_ms,cpu_ms,features,inliers,position,offset_half_width,left_margin_px,right_margin_px,motion,left_a,left_b,left_c,left_min,left_max,right_a,right_b,right_c,right_min,right_max\n";
                if(!hybrid_records)throw std::runtime_error("Cannot open hybrid evidence log");
            }
            lane_records.open(output+"/lane_metrics.csv");
            lane_records<<"channel,frame_id,source,received_monotonic_s,finished_monotonic_s,preprocess_ms,api_ms,postprocess_ms,pipeline_ms,receive_to_result_ms,pixels,crosswalk_pixels,blocked_pixels,pair_valid,offset_half_width,left_style,right_style,inputs_ms,run_ms,get_ms,validate_ms,decode_ms,geometry_ms,occlusion_ms,same_frame_occlusion,output_copy_ms,post_queue_ms,left_reason,right_reason,pair_reason,left_solid_votes,left_dashed_votes,right_solid_votes,right_dashed_votes,left_raw_style,right_raw_style,left_confirmation_streak,right_confirmation_streak\n";
            if(!lane_records)throw std::runtime_error("Cannot open lane log");
        }
        if(mosaic)qv::verify_rga_mosaic(heap,rgb_format,input_format);
        input.begin_write();std::memset(input.data(),114,input.size());input.finish_write();
        lane_input.begin_write();std::memset(lane_input.data(),114,lane_input.size());lane_input.finish_write();
        input.begin_read();try{for(unsigned k=0;k<warmup;++k){double a=0,b=0;detector.infer(input.data(),confidence,iou,a,b);}}
        catch(...){input.finish_read();throw;}input.finish_read();
        if(front_detector){front_input.begin_write();std::memset(front_input.data(),114,front_input.size());front_input.finish_write();
            front_input.begin_read();try{for(unsigned k=0;k<warmup;++k){double a=0,b=0;front_detector->infer(front_input.data(),confidence,iou,a,b);}}
            catch(...){front_input.finish_read();throw;}front_input.finish_read();}
        if(lane_model){lane_input.begin_read();try{for(unsigned k=0;k<warmup;++k){double a=0,b=0;lane_model->infer(lane_input.data(),lane.confidence,a,b);}}
            catch(...){lane_input.finish_read();throw;}lane_input.finish_read();}
    }
    void dump_input(unsigned index,qv::DmaBuffer& buffer,int width,int height){
        if(!diagnose || dumped.at(index))return;
        auto folder=output_directory+"/debug";std::filesystem::create_directories(folder);
        std::ofstream file(folder+"/input_"+std::to_string(index)+".ppm",std::ios::binary);file<<"P6\n"<<width<<' '<<height<<"\n255\n";
        file.write(reinterpret_cast<char*>(buffer.data()),buffer.size());if(!file)throw std::runtime_error("Cannot save RGB diagnosis");dumped[index]=true;
    }
    void record_detector(const Detector& model,uint64_t inference,bool mosaic_input){
        const auto& t=model.stages();detector_records<<std::fixed<<std::setprecision(6)<<inference<<','<<model.width()<<','<<model.height()<<','<<mosaic_input<<','<<live.native_output
            <<','<<t.inputs<<','<<t.run<<','<<t.get<<','<<t.validate<<','<<t.decode<<','<<t.release<<'\n';
        if(!detector_records)throw std::runtime_error("Cannot write detector timing");
    }
    std::pair<double,double> audit_detection(unsigned channel,const std::shared_ptr<qv::Frame>& frame,const std::vector<Box>& boxes,uint64_t inference,const std::vector<TrackedBox>& tracks,std::vector<SignalCandidate>& automatic){
        if(!live.save_every && !((live.light_candidates||live.automatic_signal) && channel==live.front))return {0,0};
        const uint64_t sample=++detection_evidence_count[channel];
        const bool capture=live.save_every && (sample-1)%live.save_every==0;
        const auto stem=output_directory+"/detection_frames/ch"+std::to_string(channel)+"_"+std::to_string(sample);
        std::vector<LightCandidate> lights;double light_ms=0,io_ms=0;
        auto source=std::static_pointer_cast<qv::DmaBuffer>(frame->memory);
        source->begin_read();
        try{
            double start=qv::now_seconds();
            if(live.light_candidates && channel==live.front)for(const auto& b:boxes){
                if(b.category!=7 || b.score<(tracking_options.light_confidence>0?tracking_options.light_confidence:.25f) || lights.size()>=32)continue;
                auto roi=light_roi_rgb565(source->data(),b);
                LightCandidate value;value.box=b;
                // Diagnostic HSV agrees with the automatic signal classifier.
                if(roi.empty())value.reason="INVALID_OR_OVERSIZE_ROI";else value=classify_light(roi,b,100,120,160,.005);
                lights.push_back(value);
            }
            if(live.automatic_signal&&channel==live.front)automatic=automatic_signal_candidates(source->data(),boxes,tracks,tracking_options.light_confidence>0?tracking_options.light_confidence:confidence,live.packed_signal_shape);
            if(light_rgb&&channel==live.front){
                std::vector<size_t> order(automatic.size());std::iota(order.begin(),order.end(),0);
                std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){
                    bool ak=automatic[a].color!="UNKNOWN",bk=automatic[b].color!="UNKNOWN";
                    return ak!=bk?ak:automatic[a].box.score>automatic[b].box.score;});
                unsigned used=0;
                for(size_t index:order){auto& item=automatic[index];item.hsv_color=item.color;
                    RGBDecision rgb;
                    if(used>=live.light_rgb_max)rgb.reason="RGB_BUDGET_EXHAUSTED";
                    else {rgb=light_rgb->infer(source->data(),item.box,item.hsv_color);++used;}
                    item.rgb_color=rgb.raw;item.rgb_reason=rgb.reason;item.rgb_probabilities=rgb.probabilities;
                    item.rgb_probability=rgb.probability;item.rgb_margin=rgb.margin;item.color=rgb.color;
                    if(rgb.color=="UNKNOWN")item.reason=rgb.reason;
                    light_rgb_records<<std::fixed<<std::setprecision(9)<<frame->id<<','<<frame->received<<','<<item.track_id<<','<<item.box.score<<','
                      <<item.hsv_color<<','<<rgb.raw<<','<<rgb.color<<','<<rgb.probabilities[0]<<','<<rgb.probabilities[1]<<','<<rgb.probabilities[2]<<','
                      <<rgb.probability<<','<<rgb.margin<<','<<rgb.reason<<','<<rgb.prep_ms<<','<<rgb.api_ms<<'\n';
                }
                if(!light_rgb_records)throw std::runtime_error("RGB log write failed");
            }
            light_ms=milliseconds(start);start=qv::now_seconds();
            if(live.automatic_signal&&channel==live.front){
                automatic_signal_records<<std::fixed<<std::setprecision(9)<<"{\"frame_id\":"<<frame->id<<",\"source\":"<<frame->source<<",\"received_s\":"<<frame->received<<",\"same_original_frame\":true,\"ego_lane_verified\":false,\"candidates\":[";
                for(size_t k=0;k<automatic.size();++k){const auto& v=automatic[k];if(k)automatic_signal_records<<',';
                    automatic_signal_records<<"{\"track_id\":\""<<v.track_id<<"\",\"score\":"<<v.box.score<<",\"xyxy\":["<<v.box.x1<<','<<v.box.y1<<','<<v.box.x2<<','<<v.box.y2<<"],\"color\":\""<<v.color<<"\",\"shape\":\""<<v.shape<<"\",\"direction\":"<<v.direction<<",\"shape_iou\":"<<v.shape_score<<",\"shape_margin\":"<<v.shape_margin<<",\"hsv_color\":\""<<v.hsv_color<<"\",\"rgb_color\":\""<<v.rgb_color<<"\",\"rgb_reason\":\""<<v.rgb_reason<<"\",\"rgb_probability\":"<<v.rgb_probability<<",\"rgb_margin\":"<<v.rgb_margin<<",\"reason\":\""<<v.reason<<"\"}";}
                automatic_signal_records<<"]}\n";if(!automatic_signal_records)throw std::runtime_error("Cannot write automatic signal candidates");
            }
            if(capture){
                std::ofstream f(stem+"_rgb565.bin",std::ios::binary);
                f.write(reinterpret_cast<const char*>(source->data()),qv::FrameBytes);
                if(!f)throw std::runtime_error("Cannot save original front RGB565");
                for(size_t k=0;k<lights.size();++k){auto roi=light_roi_rgb565(source->data(),lights[k].box);
                    if(!roi.empty())evidence::ppm(stem+"_light_"+std::to_string(k)+".ppm",roi.data,roi.cols,roi.rows);}
                front_evidence_records<<sample<<','<<inference<<','<<channel<<','<<frame->id<<','<<frame->source<<','<<std::fixed<<std::setprecision(9)<<frame->received<<'\n';
                front_evidence_records.flush();if(!front_evidence_records)throw std::runtime_error("Cannot write original frame index");
            }
            if(live.light_candidates && channel==live.front){
                light_records<<std::fixed<<std::setprecision(9)<<"{\"inference_id\":"<<inference<<",\"channel\":"<<channel<<",\"frame_id\":"<<frame->id
                    <<",\"source\":"<<frame->source<<",\"received_monotonic_s\":"<<frame->received<<",\"control_enabled\":false,\"associated_with_ego_lane\":false"
                    <<",\"algorithm\":\"ROI_HSV_auto_aligned_v2\",\"min_s\":100,\"min_v\":120,\"red_high\":160,\"min_ratio\":0.005,\"light_ms\":"<<light_ms<<",\"saved_sample\":"<<(capture?sample:0)<<",\"limit\":32,\"candidates\":[";
                for(size_t k=0;k<lights.size();++k){const auto& v=lights[k];if(k)light_records<<',';
                    light_records<<"{\"color\":\""<<v.color<<"\",\"reason\":\""<<v.reason<<"\",\"score\":"<<v.box.score<<",\"xyxy\":["<<v.box.x1<<','<<v.box.y1<<','<<v.box.x2<<','<<v.box.y2
                        <<"],\"analysis_width\":"<<v.analysis_width<<",\"analysis_height\":"<<v.analysis_height<<",\"components_rgb_order\":["<<v.components[0]<<','<<v.components[1]<<','<<v.components[2]<<"],\"dominance\":"<<v.dominance<<'}';}
                light_records<<"]}\n";if(!light_records)throw std::runtime_error("Cannot write light candidates");
            }
            io_ms=milliseconds(start);
        }catch(...){source->finish_read();throw;}
        source->finish_read();return {light_ms,io_ms};
    }
    Result save(unsigned channel,const std::shared_ptr<qv::Frame>& frame,const std::vector<Box>& boxes,double preprocessing,double api_ms,double post_ms,double pipeline,double finished,uint64_t inference,float peak){
        std::vector<TrackedBox> tracked;double tracking_ms=0;
        if(tracking_options.enabled){
            double begin=qv::now_seconds();
            if(track_started[channel] && uint32_t(frame->id-track_frames[channel])>=0x80000000u)trackers[channel].reset();
            track_started[channel]=true;track_frames[channel]=frame->id;
            tracked=trackers[channel].update(boxes,frame->received,channel);tracking_ms=milliseconds(begin);
            finished=qv::now_seconds();pipeline+=tracking_ms;
            track_records<<std::fixed<<std::setprecision(6)<<"{\"channel\":"<<channel<<",\"frame_id\":"<<frame->id<<",\"received_monotonic_s\":"<<frame->received
                <<",\"finished_monotonic_s\":"<<finished<<",\"tracking_ms\":"<<tracking_ms<<",\"retained\":"<<trackers[channel].retained()<<",\"tracks\":[";
            bool first=true;for(const auto& t:tracked){if(!first)track_records<<',';first=false;
                track_records<<"{\"id\":\""<<t.id<<"\",\"class\":"<<t.box.category<<",\"predicted\":false,\"observed_monotonic_s\":"<<t.observed
                    <<",\"score\":"<<t.box.score<<",\"xyxy\":["<<t.box.x1<<','<<t.box.y1<<','<<t.box.x2<<','<<t.box.y2<<"]}";}
            track_records<<"]}\n";if(!track_records)throw std::runtime_error("Cannot write tracks");
        }
        std::vector<SignalCandidate> automatic;
        const double audit_start=qv::now_seconds();auto audit=audit_detection(channel,frame,boxes,inference,tracked,automatic);
        pipeline+=milliseconds(audit_start);finished=qv::now_seconds();
        double age=(finished-frame->received)*1000;
        Result result{frame->id,frame->source,frame->received,finished,boxes,tracked,
                      tracking_options.enabled,confidence,tracking_options.light_confidence,std::move(automatic)};
        {std::lock_guard<std::mutex> lock(mutex);auto& s=statistics[channel];++s.completed;if(age>max_age_ms)++s.late;
            s.preprocess_ms+=preprocessing;s.api_ms+=api_ms;s.postprocess_ms+=post_ms;s.pipeline_ms+=pipeline;s.frame_age_ms+=age;
            s.max_class_score=peak;s.tracking_ms+=tracking_ms;results[channel]=result;}
        records<<std::fixed<<std::setprecision(6)<<"{\"inference_id\":"<<inference<<",\"channel\":"<<channel<<",\"frame_id\":"<<frame->id<<",\"source\":"<<frame->source<<",\"still_image_test\":"<<(frame->still_image_test?"true":"false")<<",\"content_received_s\":"<<frame->content_received
            <<",\"received_monotonic_s\":"<<frame->received<<",\"finished_monotonic_s\":"<<finished<<",\"preprocess_ms\":"<<preprocessing
            <<",\"rknn_api_ms\":"<<api_ms<<",\"postprocess_ms\":"<<post_ms<<",\"tracking_ms\":"<<tracking_ms<<",\"pipeline_ms\":"<<pipeline
            <<",\"receive_to_result_ms\":"<<age<<",\"light_candidate_ms\":"<<audit.first<<",\"evidence_io_ms\":"<<audit.second<<",\"boxes\":[";
        bool first=true;for(const auto& b:boxes){if(!first)records<<',';first=false;records<<"{\"class\":"<<b.category<<",\"score\":"<<b.score<<",\"xyxy\":["<<b.x1<<','<<b.y1<<','<<b.x2<<','<<b.y2<<"]}";}records<<"]}\n";
        if(!records)throw std::runtime_error("Cannot write detections");
        if(channel==live.front && live.lane_hybrid){
            auto task=std::make_unique<HybridTask>();task->frame=frame;task->detection=result;
            {std::lock_guard<std::mutex> hlock(hybrid_mutex);
                if(hybrid_pending){std::lock_guard<std::mutex> lock(mutex);++hybrid_statistics.replaced;}
                hybrid_pending=std::move(task);}
            hybrid_ready.notify_one();
        }
        if(channel==live.front){
            bool priority=false;for(const auto& b:boxes)if(b.score>=confidence &&
                ((b.category==7 && (b.x2-b.x1)<60) || (b.category>=2 && b.category<=6 && b.y2>576 && b.x2-b.x1>200)))priority=true;
            shallow_weight=live.shallow_weight*(priority?1.5:1.0);
        }
        return result;
    }
    Result detect(unsigned channel,const std::shared_ptr<qv::Frame>& frame){
        double begin=qv::now_seconds();auto source=std::static_pointer_cast<qv::DmaBuffer>(frame->memory);
        auto& model=channel==live.front && front_detector?*front_detector:detector;
        auto& buffer=channel==live.front && front_detector?front_input:input;int height=model.height();
        buffer.begin_write();std::memset(buffer.data(),114,buffer.size());buffer.finish_write();
        qv::rga_blit(*source,1280,720,rgb_format,buffer,640,height,input_format,{0,(height-360)/2,640,360});
        buffer.begin_read();double preprocessing=milliseconds(begin),api=0,post=0;std::vector<Box> boxes;
        try{boxes=model.infer(buffer.data(),.1f,iou,api,post);dump_input(channel,buffer,640,height);}
        catch(...){buffer.finish_read();throw;}buffer.finish_read();
        double finished=qv::now_seconds();uint64_t inference;
        {std::lock_guard<std::mutex> lock(mutex);inference=++inferences;}
        record_detector(model,inference,false);
        return save(channel,frame,boxes,preprocessing,api,post,(finished-begin)*1000,finished,inference,model.peak_score());
    }
    void save_mosaic(uint64_t inference,const std::array<std::shared_ptr<qv::Frame>,4>& frames,const std::array<bool,4>& active,const std::array<std::vector<Box>,4>& boxes){
        if(!live.full_mosaic || !live.save_every || (++mosaic_count-1)%live.save_every)return;
        auto folder=output_directory+"/mosaic_frames";std::filesystem::create_directories(folder);
        input.begin_read();cv::Mat rgb;
        try{rgb=cv::Mat(640,640,CV_8UC3,input.data()).clone();}catch(...){input.finish_read();throw;}input.finish_read();
        auto write=[&](const std::string& suffix,const cv::Mat& image){std::ofstream f(folder+"/"+std::to_string(inference)+suffix,std::ios::binary);
            f<<"P6\n640 640\n255\n";f.write(reinterpret_cast<const char*>(image.data),640*640*3);if(!f)throw std::runtime_error("Cannot save mosaic frame");};
        write("_input.ppm",rgb);
        std::ofstream metadata(folder+"/"+std::to_string(inference)+"_sources.csv");metadata<<"channel,frame_id,received_monotonic_s,fresh_output\n";
        for(unsigned k=0;k<4;++k){int x=(k%2)*320,y=(k/2)*320;
            metadata<<k<<','<<(frames[k]?std::to_string(frames[k]->id):"")<<','<<std::fixed<<std::setprecision(9)<<(frames[k]?frames[k]->received:0)<<','<<active[k]<<'\n';
            cv::putText(rgb,"CH"+std::to_string(k)+(active[k]?" NEW":" CACHED/EMPTY"),{x+4,y+22},cv::FONT_HERSHEY_SIMPLEX,.45,{255,255,255},1);
            if(active[k])for(const auto& b:boxes[k])if(b.score>=(b.category==7&&tracking_options.light_confidence>0?tracking_options.light_confidence:confidence)){
                cv::rectangle(rgb,{cvRound(x+b.x1/4),cvRound(y+70+b.y1/4)},{cvRound(x+b.x2/4),cvRound(y+70+b.y2/4)},{255,255,0},1);
            }
        }
        if(!metadata)throw std::runtime_error("Cannot save mosaic source index");
        write("_overlay.ppm",rgb);
    }
    void save_sample(const LaneMask& mask,const Result& detection,const std::vector<uint8_t>& rgb_snapshot,const std::shared_ptr<qv::Frame>& frame){
        ++deep_count;if(!live.save_every || (deep_count-1)%live.save_every)return;
        auto stem=output_directory+"/frames/"+std::to_string(deep_count);
        auto original=std::static_pointer_cast<qv::DmaBuffer>(frame->memory);original->begin_read();
        try{
            std::ofstream raw(stem+"_rgb565.bin",std::ios::binary);raw.write(reinterpret_cast<const char*>(original->data()),qv::FrameBytes);
            if(!raw)throw std::runtime_error("Cannot save original deep frame");
        }catch(...){original->finish_read();throw;}original->finish_read();
        if(rgb_snapshot.size()!=640*384*3)throw std::runtime_error("Missing same-frame RGB snapshot");
        cv::Mat rgb(384,640,CV_8UC3,const_cast<uint8_t*>(rgb_snapshot.data())),view=rgb(cv::Rect(0,12,640,360)).clone();
        auto ppm=[&](const std::string& suffix,const cv::Mat& image){std::ofstream f(stem+suffix,std::ios::binary);f<<"P6\n640 360\n255\n";f.write(reinterpret_cast<const char*>(image.data),640*360*3);if(!f)throw std::runtime_error("Cannot save sample PPM");};
        auto pgm=[&](const std::string& suffix,const cv::Mat& image){std::ofstream f(stem+suffix,std::ios::binary);f<<"P5\n640 360\n255\n";f.write(reinterpret_cast<const char*>(image.data),640*360);if(!f)throw std::runtime_error("Cannot save sample PGM");};
        ppm("_input.ppm",view);
        cv::Mat classes(360,640,CV_8U,const_cast<uint8_t*>(mask.native_classes.data()));
        cv::Mat blocked(360,640,CV_8U,const_cast<uint8_t*>(mask.native_blocked.data()));pgm("_classes.pgm",classes);pgm("_blocked.pgm",blocked);
        const cv::Vec3b colors[4]={{0,0,0},{40,220,40},{40,110,230},{210,40,210}};
        for(int y=0;y<360;++y)for(int x=0;x<640;++x){auto c=classes.at<uint8_t>(y,x);if(!c||blocked.at<uint8_t>(y,x))continue;
            for(int k=0;k<3;++k)view.at<cv::Vec3b>(y,x)[k]=uint8_t(view.at<cv::Vec3b>(y,x)[k]*.55+colors[c][k]*.45);}
        for(auto side:{std::make_pair(mask.left_curve,mask.left_style),std::make_pair(mask.right_curve,mask.right_style)}){
            auto color=side.second==1?cv::Scalar(0,255,0):side.second==2?cv::Scalar(0,100,255):cv::Scalar(255,255,255);
            if(side.first.valid)for(double y=side.first.ymin;y<=side.first.ymax;y+=.5){int x=cvRound(side.first.x(y)),yy=cvRound(y);
                if(x>=0&&x<639&&yy>=0&&yy<360&&!blocked.at<uint8_t>(yy,x))cv::line(view,{x,yy},{x+1,yy},color,1);}}
        for(const auto& t:detection.tracks){const auto& b=t.box;cv::rectangle(view,{cvRound(b.x1*.5),cvRound(b.y1*.5)},{cvRound(b.x2*.5),cvRound(b.y2*.5)},{255,255,0},1);}
        ppm("_overlay.ppm",view);
        std::ofstream geometry(stem+"_geometry.json");
        geometry<<std::setprecision(9)<<"{\"channel\":"<<live.front<<",\"frame_id\":"<<mask.frame_id<<",\"source\":"<<mask.source<<",\"received_monotonic_s\":"<<mask.received
            <<",\"coordinate_system\":\"640x360 road content; original xy = 2 * xy\",\"pair_valid\":"<<(mask.geometry.valid?"true":"false")
            <<",\"pair_reason\":\""<<mask.pair_reason<<"\",\"offset_half_width\":"<<mask.geometry.offset;
        auto curve=[&](const char* side,const Curve& c,int style,const LaneMask::StyleEvidence& e){
            geometry<<",\""<<side<<"\":{\"valid\":"<<(c.valid?"true":"false")<<",\"coefficients\":["<<c.coef[0]<<','<<c.coef[1]<<','<<c.coef[2]
                <<"],\"formula\":\"x=a*(y/360)^2+b*(y/360)+c\",\"ymin\":"<<c.ymin<<",\"ymax\":"<<c.ymax<<",\"inlier_ratio\":"<<c.inlier_ratio
                <<",\"style\":"<<style<<",\"raw_style\":"<<e.raw_style<<",\"reason\":\""<<e.reason<<"\",\"solid_votes\":"<<e.solid_votes
                <<",\"dashed_votes\":"<<e.dashed_votes<<",\"confirmation_streak\":"<<e.confirmation_streak<<'}';};
        curve("left",mask.left_curve,mask.left_style,mask.left_evidence);curve("right",mask.right_curve,mask.right_style,mask.right_evidence);geometry<<"}\n";
        if(!geometry)throw std::runtime_error("Cannot save lane fit evidence");
        sample_records<<deep_count<<','<<live.front<<','<<mask.frame_id<<','<<mask.source<<','<<std::fixed<<std::setprecision(9)<<mask.received<<'\n';sample_records.flush();
        if(!sample_records)throw std::runtime_error("Cannot write sample index");
    }
    void finish_lane(CpuLaneTask& task){
        unsigned channel=task.channel;auto frame=task.frame;
        double queue_ms=std::max(0.,(qv::now_seconds()-task.enqueued)*1000);
        if((qv::now_seconds()-frame->received)*1000>lane_options.max_age_ms){
            std::lock_guard<std::mutex> lock(mutex);++lane_statistics[channel].cpu_stale;
            hybrid_model_pending=false;hybrid_needs_model=true;return;
        }
        auto mask=lane_model->process(task.raw,lane_options.confidence,task.detection.boxes,task.detection.tracks,frame->received,channel);
        if(diagnose && !lane_dumped){
            auto folder=output_directory+"/debug";std::filesystem::create_directories(folder);
            std::ofstream rgb(folder+"/lane_input.ppm",std::ios::binary);rgb<<"P6\n640 384\n255\n";
            rgb.write(reinterpret_cast<const char*>(task.rgb.data()),task.rgb.size());
            std::ofstream pgm(folder+"/lane_visible.pgm",std::ios::binary);pgm<<"P5\n576 324\n255\n";pgm.write(reinterpret_cast<const char*>(mask.data.data()),mask.data.size());
            std::ofstream occ(folder+"/lane_blocked.pgm",std::ios::binary);occ<<"P5\n576 324\n255\n";occ.write(reinterpret_cast<const char*>(mask.blocked.data()),mask.blocked.size());
            if(!rgb||!pgm||!occ)throw std::runtime_error("Cannot write lane diagnostic");
            lane_dumped=true;
        }
        mask.frame_id=frame->id;mask.source=frame->source;mask.received=frame->received;
        mask.model_frame_id=frame->id;mask.model_received=frame->received;
        lane_position(mask,lane_options.ego_x*640,position_config->reference*360);
        save_sample(mask,task.detection,task.rgb,frame);
        double finished=qv::now_seconds(),age=(finished-frame->received)*1000,pipe=(finished-task.begin)*1000;
        const auto& raw=task.raw;double api=raw.inputs_ms+raw.run_ms+raw.get_ms+raw.release_ms;
        double post=raw.copy_ms+mask.validate_ms+mask.decode_ms+mask.occlusion_ms+mask.geometry_ms;
        mask.finished=finished;mask.post_queue_ms=queue_ms;
        lane_records<<std::fixed<<std::setprecision(6)<<channel<<','<<frame->id<<','<<frame->source<<','<<frame->received<<','<<finished<<','<<task.prep<<','<<api<<','<<post<<','<<pipe<<','<<age
            <<','<<mask.pixels<<','<<mask.crosswalk_pixels<<','<<mask.blocked_pixels<<','<<mask.geometry.valid<<','<<mask.geometry.offset<<','<<mask.left_style<<','<<mask.right_style
            <<','<<mask.inputs_ms<<','<<mask.run_ms<<','<<mask.get_ms<<','<<mask.validate_ms<<','<<mask.decode_ms<<','<<mask.geometry_ms<<','<<mask.occlusion_ms<<','<<lane_options.occlusion
            <<','<<mask.output_copy_ms<<','<<mask.post_queue_ms<<','<<mask.left_evidence.reason<<','<<mask.right_evidence.reason<<','<<mask.pair_reason
            <<','<<mask.left_evidence.solid_votes<<','<<mask.left_evidence.dashed_votes<<','<<mask.right_evidence.solid_votes<<','<<mask.right_evidence.dashed_votes
            <<','<<mask.left_evidence.raw_style<<','<<mask.right_evidence.raw_style<<','<<mask.left_evidence.confirmation_streak<<','<<mask.right_evidence.confirmation_streak<<'\n';
        lane_records.flush();if(!lane_records)throw std::runtime_error("Cannot write lane metrics");
        {std::lock_guard<std::mutex> lock(mutex);auto& st=lane_statistics[channel];++st.completed;if(age>lane_options.max_age_ms)++st.late;
            st.preprocess_ms+=task.prep;st.api_ms+=api;st.postprocess_ms+=post;st.pipeline_ms+=pipe;st.frame_age_ms+=age;
            deep_weight=live.deep_weight*(!mask.geometry.valid?4./3:1.);
            lane_results[channel]=std::make_shared<const LaneMask>(std::move(mask));}
        if(live.lane_hybrid){
            auto seed=std::make_unique<HybridTask>();seed->frame=frame;
            {std::lock_guard<std::mutex> lock(mutex);seed->seed=lane_results[channel];}
            hybrid_needs_model=!seed->seed->geometry.valid;
            {std::lock_guard<std::mutex> lock(hybrid_mutex);hybrid_seed=std::move(seed);}
            hybrid_model_pending=false;hybrid_ready.notify_one();
        }
    }
    void deep(unsigned channel,const std::shared_ptr<qv::Frame>& frame,const Result& detection){
        if(detection.frame_id!=frame->id || detection.received!=frame->received || detection.source!=frame->source)
            throw std::runtime_error("Refusing stale/wrong-frame vehicle occlusion");
        auto task=std::make_unique<CpuLaneTask>();task->channel=channel;task->frame=frame;task->detection=detection;task->begin=qv::now_seconds();
        auto source=std::static_pointer_cast<qv::DmaBuffer>(frame->memory);
        qv::rga_blit(*source,1280,720,rgb_format,lane_input,640,384,input_format,{0,12,640,360});
        lane_input.begin_read();
        try{
            if(live.save_every || diagnose)task->rgb.assign(lane_input.data(),lane_input.data()+lane_input.size());
            task->prep=milliseconds(task->begin);task->raw=lane_model->capture(lane_input.data());
        }catch(...){lane_input.finish_read();throw;}lane_input.finish_read();task->enqueued=qv::now_seconds();
        if(!live.lane_post_async){finish_lane(*task);return;}
        {std::lock_guard<std::mutex> cpu_lock(cpu_mutex);
            if(cpu_pending){std::lock_guard<std::mutex> lock(mutex);++lane_statistics[channel].cpu_replaced;}
            cpu_pending=std::move(task);}
        cpu_ready.notify_one();
    }
    void run_cpu(){try{
        while(true){std::unique_ptr<CpuLaneTask> task;
            {std::unique_lock<std::mutex> lock(cpu_mutex);cpu_ready.wait(lock,[&]{return cpu_stopping || bool(cpu_pending);});
                if(!cpu_pending && cpu_stopping)break;
                task=std::move(cpu_pending);}
            finish_lane(*task);
        }
    }catch(const std::exception& e){std::lock_guard<std::mutex> lock(mutex);failure=e.what();stopping=true;ready.notify_all();}}
    void start_cpu(){if(live.lane_post_async && lane_model)cpu_thread=std::thread([this]{run_cpu();});}
    void stop_cpu(){
        {std::lock_guard<std::mutex> lock(cpu_mutex);cpu_stopping=true;}cpu_ready.notify_all();
        if(cpu_thread.joinable())cpu_thread.join();
    }
    cv::Mat hybrid_gray(const std::shared_ptr<qv::Frame>& frame){
        cv::Mat gray(180,320,CV_8U);auto source=std::static_pointer_cast<qv::DmaBuffer>(frame->memory);
        source->begin_read();
        try{for(int y=0;y<180;++y)for(int x=0;x<320;++x){
            size_t at=((y*4+2)*1280+x*4+2)*2;unsigned v=source->data()[at]|unsigned(source->data()[at+1])<<8;
            unsigned r=(v>>11)*255/31,g=((v>>5)&63)*255/63,b=(v&31)*255/31;
            gray.at<uint8_t>(y,x)=uint8_t((77*r+150*g+29*b)>>8);
        }}catch(...){source->finish_read();throw;}source->finish_read();return gray;
    }
    void save_hybrid(const LaneMask& mask,const std::shared_ptr<qv::Frame>& frame){
        if(!live.save_every || (++hybrid_sample_count-1)%live.save_every)return;
        auto folder=output_directory+"/hybrid_frames";std::filesystem::create_directories(folder);
        auto stem=folder+"/"+std::to_string(frame->id);auto source=std::static_pointer_cast<qv::DmaBuffer>(frame->memory);
        std::vector<uint8_t> bytes(qv::FrameBytes);source->begin_read();
        std::memcpy(bytes.data(),source->data(),bytes.size());source->finish_read();
        std::ofstream raw(stem+"_rgb565.bin",std::ios::binary);raw.write(reinterpret_cast<char*>(bytes.data()),bytes.size());
        cv::Mat rgb(360,640,CV_8UC3);
        for(int y=0;y<360;++y)for(int x=0;x<640;++x){size_t at=((y*2+1)*1280+x*2+1)*2;unsigned v=bytes[at]|unsigned(bytes[at+1])<<8;
            rgb.at<cv::Vec3b>(y,x)={uint8_t((v>>11)*255/31),uint8_t(((v>>5)&63)*255/63),uint8_t((v&31)*255/31)};}
        const auto colors=std::array<cv::Scalar,2>{{{0,255,255},{255,255,0}}};
        int side=0;for(const auto& curve:{mask.left_curve,mask.right_curve}){
            if(curve.valid)for(double y=curve.ymin;y<=curve.ymax;y+=1){int x=cvRound(curve.x(y));
                if(x>=0&&x<640&&y>=0&&y<360&&!mask.native_blocked[int(y)*640+x])cv::circle(rgb,{x,int(y)},1,colors[side],-1);}++side;}
        cv::putText(rgb,std::string("CPU ")+mask.position,{10,24},cv::FONT_HERSHEY_SIMPLEX,.5,{255,255,255},1);
        std::ofstream ppm(stem+"_overlay.ppm",std::ios::binary);ppm<<"P6\n640 360\n255\n";ppm.write(reinterpret_cast<char*>(rgb.data),rgb.total()*3);
        if(!raw||!ppm)throw std::runtime_error("Cannot save CPU hybrid evidence");
    }
    void run_hybrid(){try{
        double seeded=-1;
        while(true){std::unique_ptr<HybridTask> task;
            {std::unique_lock<std::mutex> lock(hybrid_mutex);hybrid_ready.wait(lock,[&]{return hybrid_stopping||bool(hybrid_seed)||bool(hybrid_pending);});
                if(hybrid_seed)task=std::move(hybrid_seed);
                else if(hybrid_pending)task=std::move(hybrid_pending);
                else if(hybrid_stopping)break;}
            if(task->seed){hybrid_tracker->seed(hybrid_gray(task->frame),*task->seed);seeded=task->frame->received;continue;}
            auto frame=task->frame;if(frame->received<=seeded)continue;
            if(task->detection.frame_id!=frame->id||task->detection.received!=frame->received||task->detection.source!=frame->source)
                throw std::runtime_error("CPU hybrid requires same-frame vehicle boxes");
            double begin=qv::now_seconds(),queue_ms=(begin-task->detection.finished)*1000;
            auto gray=hybrid_gray(frame);double gray_ms=milliseconds(begin);
            auto blocked=lane_options.occlusion?lane4::vehicle_mask(task->detection.boxes,task->detection.tracks):cv::Mat::zeros(360,640,CV_8U);
            auto update=hybrid_tracker->update(gray,blocked,frame->received);
            auto mask=std::move(update.mask);mask.frame_id=frame->id;mask.source=frame->source;mask.received=frame->received;
            if((qv::now_seconds()-frame->received)*1000>lane_options.max_age_ms){update.accepted=false;update.reason="CPU_RESULT_STALE";}
            if(update.accepted && (qv::now_seconds()-mask.model_received)*1000>live.hybrid_semantic_ms){update.accepted=false;update.reason="MODEL_RESULT_STALE";}
            if(!update.accepted){auto model_id=mask.model_frame_id;auto model_time=mask.model_received;
                mask=LaneMask{};mask.model_frame_id=model_id;mask.model_received=model_time;
                mask.frame_id=frame->id;mask.source=frame->source;mask.received=frame->received;
                mask.cpu_propagated=true;mask.four_class=true;mask.geometry_enabled=true;mask.pair_reason=update.reason;
                mask.data.assign(LaneWidth*LaneHeight,0);hybrid_needs_model=true;
            }else if(!mask.geometry.valid)hybrid_needs_model=true;
            mask.finished=qv::now_seconds();double cpu_ms=(mask.finished-begin)*1000;
            hybrid_records<<std::fixed<<std::setprecision(6)<<frame->id<<','<<frame->source<<','<<frame->received<<','<<mask.finished<<','<<update.accepted<<','<<update.reason
                <<','<<mask.model_frame_id<<','<<mask.model_received<<','<<update.semantic_age_ms<<','<<queue_ms<<','<<gray_ms<<','<<update.flow_ms<<','<<update.fit_ms<<','<<cpu_ms
                <<','<<update.features<<','<<update.inliers<<','<<mask.position<<','<<mask.relative_offset<<','<<mask.left_margin<<','<<mask.right_margin<<','<<update.motion;
            for(auto c:{mask.left_curve,mask.right_curve})hybrid_records<<','<<c.coef[0]<<','<<c.coef[1]<<','<<c.coef[2]<<','<<c.ymin<<','<<c.ymax;
            hybrid_records<<'\n';
            hybrid_records.flush();if(!hybrid_records)throw std::runtime_error("Cannot write hybrid metrics");
            if(update.accepted)save_hybrid(mask,frame);
            {std::lock_guard<std::mutex> lock(mutex);++hybrid_statistics.attempts;hybrid_statistics.cpu_ms+=cpu_ms;
                if(update.accepted){++hybrid_statistics.completed;
                    if(hybrid_statistics.last_finished>0)hybrid_statistics.max_gap_ms=std::max(hybrid_statistics.max_gap_ms,(mask.finished-hybrid_statistics.last_finished)*1000);
                    hybrid_statistics.last_finished=mask.finished;
                }else ++hybrid_statistics.rejected;
                if(!hybrid_result || mask.received>hybrid_result->received)hybrid_result=std::make_shared<const LaneMask>(std::move(mask));}
        }
    }catch(const std::exception& e){std::lock_guard<std::mutex> lock(mutex);failure=e.what();stopping=true;ready.notify_all();}}
    void start_hybrid(){if(hybrid_tracker)hybrid_thread=std::thread([this]{run_hybrid();});}
    void stop_hybrid(){
        {std::lock_guard<std::mutex> lock(hybrid_mutex);hybrid_stopping=true;}hybrid_ready.notify_all();
        if(hybrid_thread.joinable())hybrid_thread.join();
    }
    void run(){try{
        while(true){
            std::array<std::shared_ptr<qv::Frame>,4> frames;std::array<bool,4> active{},drawn{};int job=-1;unsigned channel=live.front;
            double queue_age=0,weight=1;bool forced_deadline=false;
            {std::unique_lock<std::mutex> lock(mutex);if(stopping)break;double now=qv::now_seconds();
                for(unsigned k=0;k<4;++k)if(pending[k] && (now-pending[k]->received)*1000>max_age_ms){pending[k].reset();++statistics[k].stale;}
                std::array<bool,6> available{};
                available[0]=!live.full_mosaic && bool(pending[live.front]);available[1]=bool(pending[live.front]) && bool(lane_model);
                if(mosaic){for(unsigned k=0;k<4;++k)if((live.full_mosaic || k!=live.front) && pending[k])available[2]=true;}
                else for(unsigned k=0;k<4;++k)available[k+2]=k!=live.front && bool(pending[k]);
                const bool deadline=lane_model && model_deadline_due(now,last_lane_start,live.lane_deadline_ms);
                if(live.lane_hybrid)available[1]=available[1] && !hybrid_model_pending && (hybrid_needs_model || deadline);
                // Soft deadline: considered at each job boundary; no preemption or fixed Hz cap.
                forced_deadline=deadline && available[1];job=schedule.choose(available,forced_deadline?1:-1);
                if(job==1){last_lane_start=now;if(live.lane_hybrid)hybrid_model_pending=true;}
                if(job<0){ready.wait(lock,[&]{if(stopping)return true;for(const auto& p:pending)if(p)return true;return false;});continue;}
                if(live.full_mosaic){
                    weight=job==1?deep_weight:shallow_weight;
                    for(unsigned k=0;k<4;++k){
                        if(pending[k]){mosaic_cache[k]=std::move(pending[k]);active[k]=true;}
                        if(mosaic_cache[k] && (now-mosaic_cache[k]->received)*1000<=max_age_ms){frames[k]=mosaic_cache[k];drawn[k]=true;}
                        else mosaic_cache[k].reset();
                    }
                }else if(job<=1){frames[channel]=std::move(pending[channel]);active[channel]=true;weight=job?deep_weight:shallow_weight;}
                else if(mosaic){weight=0;for(unsigned k=0;k<4;++k)if(k!=live.front && pending[k]){frames[k]=std::move(pending[k]);active[k]=true;weight+=live.other_weight;}}
                else {channel=unsigned(job-2);frames[channel]=std::move(pending[channel]);active[channel]=true;weight=live.other_weight;}
                if(!live.full_mosaic)drawn=active;
                for(unsigned k=0;k<4;++k)if(active[k])queue_age=std::max(queue_age,(now-frames[k]->received)*1000);
            }
            double begin=qv::now_seconds();unsigned detections=0,lanes=0;
            if(!live.full_mosaic && (job<=1 || !mosaic)){auto detection=detect(channel,frames[channel]);++detections;if(job==1){deep(channel,frames[channel],detection);++lanes;}}
            else {
                input.begin_write();std::memset(input.data(),114,input.size());input.finish_write();
                for(unsigned k=0;k<4;++k)if(drawn[k]){auto source=std::static_pointer_cast<qv::DmaBuffer>(frames[k]->memory);
                    qv::rga_blit(*source,1280,720,rgb_format,input,640,640,input_format,{int((k%2)*320),int((k/2)*320+70),320,180});}
                input.begin_read();double prep=milliseconds(begin),api=0,post=0;std::vector<Box> candidates;
                try{candidates=detector.infer(input.data(),.1f,iou,api,post,true);dump_input(4,input,640,640);}catch(...){input.finish_read();throw;}input.finish_read();
                double split=qv::now_seconds();auto boxes=split_mosaic(candidates,drawn,iou);post+=milliseconds(split);double finished=qv::now_seconds();
                uint64_t inference;{std::lock_guard<std::mutex> lock(mutex);inference=++inferences;}
                record_detector(detector,inference,true);
                Result front_detection;
                for(unsigned k=0;k<4;++k)if(active[k]){
                    auto result=save(k,frames[k],boxes[k],prep,api,post,(finished-begin)*1000,finished,inference,detector.peak_score());
                    if(k==live.front)front_detection=std::move(result);
                    ++detections;
                }
                save_mosaic(inference,frames,active,boxes);
                if(live.full_mosaic && job==1){deep(live.front,frames[live.front],front_detection);++lanes;}
            }
            records.flush();detector_records.flush();if(tracking_options.enabled)track_records.flush();
            double elapsed=milliseconds(begin);schedule.complete(job,elapsed,weight);
            schedule_records<<++job_sequence<<','<<job<<','<<(mosaic && (job>=2||live.full_mosaic)?-1:int(channel))<<','<<(frames[channel]?std::to_string(frames[channel]->id):"")<<','<<queue_age<<','<<elapsed<<','<<weight<<','<<detections<<','<<lanes<<','<<forced_deadline<<'\n';
            schedule_records.flush();if(!records||!schedule_records||(tracking_options.enabled && !track_records)||!detector_records)throw std::runtime_error("Runtime log write failed");
        }
    }catch(const std::exception& e){std::lock_guard<std::mutex> lock(mutex);failure=e.what();}}
};
Worker::Worker(const std::string& model,const std::string& heap,int format,const std::array<bool,4>& channels,double fps,float confidence,float iou,unsigned age,const std::string& output,unsigned warmup,bool mosaic,bool diagnose,const LaneOptions& lane,const TrackingOptions& tracking,const LiveOptions& live)
    :impl_(std::make_unique<Impl>(model,heap,format,channels,fps,confidence,iou,age,output,warmup,mosaic,diagnose,lane,tracking,live)){}
Worker::~Worker(){stop();}
void Worker::start(){if(impl_->thread.joinable())throw std::runtime_error("AI worker already started");impl_->start_cpu();impl_->start_hybrid();impl_->thread=std::thread([this](){impl_->run();});}
void Worker::stop(){{std::lock_guard<std::mutex> lock(impl_->mutex);impl_->stopping=true;impl_->ready.notify_all();}if(impl_->thread.joinable())impl_->thread.join();impl_->stop_cpu();impl_->stop_hybrid();}
void Worker::reset_scene(unsigned channel){
    auto& p=*impl_;if(channel>=4 || p.thread.joinable() || p.cpu_thread.joinable() || p.hybrid_thread.joinable())throw std::runtime_error("Scene reset requires joined workers");
    std::lock_guard<std::mutex> lock(p.mutex);
    if(!p.failure.empty())throw std::runtime_error("Cannot restart failed AI worker: "+p.failure);
    p.pending={};p.mosaic_cache={};p.results[channel]={};p.lane_results[channel].reset();
    p.trackers[channel].reset();p.track_started[channel]=false;p.track_frames[channel]=0;
    if(p.lane_model)p.lane_model->reset(channel);
    p.schedule={};p.last_lane_start=-1;p.cpu_pending.reset();p.cpu_stopping=false;
    p.hybrid_pending.reset();p.hybrid_seed.reset();p.hybrid_result.reset();p.hybrid_stopping=false;
    p.hybrid_needs_model=true;p.hybrid_model_pending=false;
    if(p.hybrid_tracker)p.hybrid_tracker=std::make_unique<HybridLane>(p.lane_options.camera,p.lane_options.ego_x,p.live.hybrid_semantic_ms);
    p.stopping=false;
}
void Worker::publish(unsigned channel,std::shared_ptr<qv::Frame> frame){
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if(impl_->stopping)return;
    if(impl_->channels.at(channel)){
        if(impl_->pending[channel]){++impl_->statistics[channel].replaced;if(channel==impl_->live.front && impl_->lane_model)++impl_->lane_statistics[channel].replaced;}
        impl_->pending[channel]=std::move(frame);
    }
    impl_->ready.notify_one();
}
Result Worker::result(unsigned channel)const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->results.at(channel);}
Stats Worker::stats(unsigned channel)const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->statistics.at(channel);}
std::shared_ptr<const LaneMask> Worker::lane_result(unsigned channel)const{
    std::lock_guard<std::mutex> lock(impl_->mutex);auto model=impl_->lane_results.at(channel),hybrid=impl_->hybrid_result;
    if(channel==impl_->live.front && impl_->live.lane_hybrid && hybrid && (!model || hybrid->received>model->received))return hybrid;
    return model;
}
HybridStats Worker::hybrid_stats()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->hybrid_statistics;}
Stats Worker::lane_stats(unsigned channel)const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->lane_statistics.at(channel);}
uint64_t Worker::inference_count()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->inferences;}
std::string Worker::error()const{std::lock_guard<std::mutex> lock(impl_->mutex);return impl_->failure;}
}
