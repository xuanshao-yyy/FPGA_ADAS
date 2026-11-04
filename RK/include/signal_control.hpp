#pragma once
#include "signal_state.hpp"
#include "signal_structure.hpp"
#include "signal_layout.hpp"
#include "signal_association.hpp"
#include "signal_presence.hpp"
#include "driver_risk.hpp"
#include "surround_risk.hpp"
#include "camera_roles.hpp"
#include "explanation_state.hpp"
#include "vehicle_simulation.hpp"
#include "lane_rules.hpp"
#include "lane_display.hpp"
#include "light_candidate.hpp"
#include "visual.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <opencv2/core.hpp>

namespace signal_demo {
struct Roi {
    bool enabled=false;
    ai::Box box{};
};
struct Config {
    std::string scene;
    Settings settings;
    std::array<Roi,3> rois{};
    unsigned loop_frames=0;
    int min_s=100,min_v=120,red_high=160;
    double min_ratio=.005;
    bool automatic=false;
    bool practical_association=true; // Simulation advisory: arrows first, stable layout fallback.
    unsigned box_hold_ms=0; // Visual cache only; never fed back to State::observe.
    bool lane_rules=false;
    unsigned lane_position_hold_ms=1000;
};
inline Config effective_config(Config c){
    if(c.automatic&&c.practical_association){
        c.settings.minimum_samples=2;c.settings.confirm_red_ms=80;c.settings.confirm_yellow_ms=80;c.settings.confirm_green_ms=150;
        c.settings.max_gap_ms=300;c.settings.max_age_ms=400;
    }
    return c;
}
inline Config load(const std::string& path){
    cv::FileStorage fs(path,cv::FileStorage::READ);
    if(!fs.isOpened())throw std::runtime_error("Cannot open signal config: "+path);
    Config c;fs["scene"]>>c.scene;
    int width=0,height=0,frames=0;fs["width"]>>width;fs["height"]>>height;fs["loop_frames"]>>frames;
    if(width!=1280||height!=720||frames<0||c.scene.empty())throw std::runtime_error("Signal config needs scene / 1280x720 / nonnegative loop_frames");
    c.loop_frames=unsigned(frames);
    if(!fs["min_s"].empty())fs["min_s"]>>c.min_s;
    if(!fs["min_v"].empty())fs["min_v"]>>c.min_v;
    if(!fs["red_high"].empty())fs["red_high"]>>c.red_high;
    if(!fs["min_ratio"].empty())fs["min_ratio"]>>c.min_ratio;
    if(c.min_s<0||c.min_s>255||c.min_v<0||c.min_v>255||c.red_high<140||c.red_high>179||!std::isfinite(c.min_ratio)||c.min_ratio<=0||c.min_ratio>.1)
        throw std::runtime_error("Invalid HSV thresholds (OpenCV H=0..179)");
    for(unsigned i=0;i<3;++i){
        const auto node=fs["rois"][name(Direction(i))];
        if(node.empty() || node.size()==0)continue;
        if(node.type()!=cv::FileNode::SEQ || node.size()!=4)throw std::runtime_error("ROI must be [] or [x1,y1,x2,y2]");
        std::vector<float> box;node>>box;
        if(!std::all_of(box.begin(),box.end(),[](float x){return std::isfinite(x);}) ||
           box[0]<0||box[1]<0||box[2]>1280||box[3]>720||box[2]-box[0]<4||box[3]-box[1]<6||box[2]-box[0]>512||box[3]-box[1]>512)
            throw std::runtime_error("ROI outside frame or classifier bounds");
        c.rois[i]={true,{box[0],box[1],box[2],box[3],1,7}};
    }
    return c;
}
// Manual mode reads explicitly bound original-image ROIs independently of YOLO.
// Auto mode consumes same-frame detector/arrow candidates; ego-lane relevance is unverified.
class Control {
    Config config;
    State state;
    std::ofstream observations,commands;
    uint32_t frame_id=0,source=0;
    uint64_t previous_loop=0;
    bool have_loop=false;
    double last_received=-1,last_analysis_ms=0;
    std::string last_state,last_instruction;
    double last_auto_received=-1,loop_floor=-1;
    std::array<uint64_t,3> automatic_ids{};
    std::array<ai::Box,3> associated_boxes{};
    std::vector<ai::SignalCandidate> automatic_candidates;
    struct CachedSignal {
        ai::SignalCandidate candidate;double received=-1;
        std::array<lane_demo::OffsetFilter,4> filters;
        void update(const ai::SignalCandidate& c,double stamp){
            if(received>=0&&(c.direction!=candidate.direction||stamp-received>.5||ai::overlap(c.box,candidate.box)<.1f))for(auto& f:filters)f.reset();
            const auto& b=c.box;double values[]={b.x1,b.y1,b.x2,b.y2};
            for(unsigned i=0;i<4;++i)filters[i].update(values[i]/120,stamp);
            candidate=c;received=stamp;
        }
        ai::Box box(double now)const{auto b=candidate.box;b.x1=float(filters[0].predict(now)*120);b.y1=float(filters[1].predict(now)*120);b.x2=float(filters[2].predict(now)*120);b.y2=float(filters[3].predict(now)*120);return b;}
    };
    std::vector<CachedSignal> cached_signals;
    std::array<std::string,3> association_reason{{"NO_AUTO_ARROW","NO_AUTO_ARROW","NO_AUTO_ARROW"}};
    std::ofstream automatic_records;
    lane_demo::State lane_state;
    lane_demo::Input lane_input;
    ai::LaneMask::StyleEvidence left_style_evidence,right_style_evidence;
    lane_demo::CurveView lane_left,lane_right;
    bool lane_left_visible=false,lane_right_visible=false;
    std::vector<uint8_t> lane_blocked;
    std::ofstream lane_records;
    std::string last_lane_rule,last_lane_notice;
    ai::SignalStructure structure;
    ai::SignalLayoutHints layout_hints;
    ai::SignalAssociation association;
    std::ofstream layout_records;
    bool manual_stop=false,forced_alarm=false;
    bool still_image=false;std::array<std::string,3> last_permissions{};std::ofstream permission_records;
    driver_demo::RiskState front_risk;driver_demo::Corridor corridor;
    std::array<product::ViewRisk,4> product_views{};
    std::ofstream risk_records,advice_records,operation_records;double object_received=-1,last_risk_flush=-1;bool local_input=false;
    std::array<unsigned,8> front_counts{};std::string last_advice;
    SignalPresence signal_presence;
    std::ofstream scene_records;
    std::array<bool,3> restriction_pending{};
    static bool qualified_signal(const ai::SignalCandidate& c){
        if(color(c.color)==Color::Unknown)return false;
        if(ai::SignalAssociation::eligible(c))return true;
        // A recognised arrow may legitimately be lower/larger than the layout-prior ROI.
        const auto& b=c.box;float w=b.x2-b.x1,h=b.y2-b.y1;
        return c.track_id&&c.direction>=0&&c.direction<3&&c.shape!="DOWN"&&b.category==7&&
            std::isfinite(b.score)&&b.score>=.12f&&std::isfinite(b.x1)&&std::isfinite(b.y1)&&std::isfinite(b.x2)&&std::isfinite(b.y2)&&
            b.x1>=0&&b.y1>=0&&b.x2<=1280&&b.y2<=720&&w>=4&&h>=6&&w<=512&&h<=512;
    }
public:
    Control(Config c,const std::string& output):config(effective_config(std::move(c))),state(config.settings),lane_state(config.lane_position_hold_ms),lane_left(config.lane_position_hold_ms),lane_right(config.lane_position_hold_ms){
        risk_records.open(output+"/driver_risk.csv");advice_records.open(output+"/driver_advice.csv");
        if(!risk_records||!advice_records)throw std::runtime_error("Cannot open driver explanation logs");
        risk_records<<"now_s,received_s,frame_id,source,target_id,category,level,height_ratio,bottom_ratio,relative_image_growth,assumed_region,held,still,reason\n";
        advice_records<<"now_s,severity,action,reason,scope,signal_direction,signal_permission,lane_direction,lane_requested,lane_rule,lane_allowed,vehicle_state,vehicle_arrow,vehicle_moving,vehicle_alarm\n";
        operation_records.open(output+"/user_operations.csv");if(!operation_records)throw std::runtime_error("Cannot open operation log");
        operation_records<<"now_s,command,origin,road_direction,lane_direction,lane_requested,rule,vehicle_state,action,reason\n";
        permission_records.open(output+"/signal_permissions.csv");
        if(!permission_records)throw std::runtime_error("Cannot open permission log");
        permission_records<<"now_s,direction,permission,color,age_ms,stop_held,still_image_test,frame_id,source,association_reason\n";
        observations.open(output+"/signal_observations.csv");commands.open(output+"/signal_commands.csv");
        if(!observations||!commands)throw std::runtime_error("Cannot open signal logs");
        if(config.automatic){automatic_records.open(output+"/signal_auto_association.csv");if(!automatic_records)throw std::runtime_error("Cannot open automatic association log");automatic_records<<"now_s,received_s,frame_id,source,direction,track_id,reason,color,stable_color,roi_x1,roi_y1,roi_x2,roi_y2,raw_reason,direction_basis,evidence_age_ms,association_id\n";for(auto& roi:config.rois)roi={};}
        layout_records.open(output+"/signal_layout_hints.csv");
        if(!layout_records)throw std::runtime_error("Cannot open layout hint log");
        layout_records<<"now_s,received_s,frame_id,source,direction,hint_color,reason,permission_changed\n";
        scene_records.open(output+"/signal_scene.csv");if(!scene_records)throw std::runtime_error("Cannot open signal scene log");
        scene_records<<"now_s,received_s,frame_id,source,raw_candidates,qualified_candidates,signal_recent,last_valid_age_ms,absence_ms,absence_samples,event\n";
        observations<<"now_s,received_s,frame_id,source,direction,roi_enabled,raw_color,stable_color,reason,red_area,yellow_area,green_area,analysis_ms\n";
        commands<<"now_s,event,direction,light,requested,allowed,alarm,state,age_ms,frame_id,source,roi_x1,roi_y1,roi_x2,roi_y2,stop_held,stop_age_ms,instruction\n";
        if(config.lane_rules){lane_records.open(output+"/lane_rules.csv");if(!lane_records)throw std::runtime_error("Cannot open lane rule log");
            lane_records<<"now_s,event,frame_id,source,received_s,age_ms,direction,motion,trend,rule,reason,offset,left_style,right_style,requested,marking_permits,side_verified,allowed,alarm,signal_stop,raw_valid,raw_motion,raw_offset,position_held,geometry_age_ms,unknown_run_ms,model_observations,unknown_observations,raw_reason,pair_reason,left_reason,right_reason,left_solid_votes,left_dashed_votes,left_raw_style,left_streak,left_direct,right_solid_votes,right_dashed_votes,right_raw_style,right_streak,right_direct,style_held\n";}
    }
    void reset_scene(Config c,double now){
        // Keep log streams open; forget all evidence, requests, held STOP and display filters.
        if(c.automatic!=config.automatic || c.lane_rules!=config.lane_rules)throw std::runtime_error("Scene mode must remain unchanged");
        config=effective_config(std::move(c));if(config.automatic)for(auto& roi:config.rois)roi={};
        state=State(config.settings);lane_state=lane_demo::State(config.lane_position_hold_ms);
        lane_left.reset();lane_right.reset();lane_left_visible=lane_right_visible=false;lane_blocked.clear();lane_input={};left_style_evidence={};right_style_evidence={};
        frame_id=source=0;previous_loop=0;have_loop=false;last_received=last_auto_received=-1;loop_floor=now;
        last_analysis_ms=0;automatic_ids={};associated_boxes={};automatic_candidates.clear();cached_signals.clear();layout_hints.reset();association.reset();
        association_reason={"NO_AUTO_ARROW","NO_AUTO_ARROW","NO_AUTO_ARROW"};
        last_state.clear();last_instruction.clear();last_lane_rule.clear();last_lane_notice.clear();last_permissions={};still_image=false;structure.reset();
        front_risk.reset();corridor={};object_received=-1;front_counts={};last_advice.clear();signal_presence.reset();restriction_pending={};product_views={};forced_alarm=false;manual_stop=false;
        log("SCENE_RESET_STOP",now);log_lane("SCENE_RESET_STOP",now);
    }
    void set_still_image(bool value,double now){auto cfg=config;reset_scene(cfg,now);still_image=value;lane_state.set_still_test(value);log(value?"STILL_IMAGE_BEGIN":"STILL_IMAGE_END",now);}
    const char* permission(Direction direction,double now)const{
        auto decision=state.signal_decision(direction,now);
        return decision.allowed?"ALLOW":std::string(decision.instruction)=="STOP"?"STOP":"WAIT";
    }
    std::string permissions_text(double now)const{return std::string("SIGNAL ONLY | LEFT ")+permission(Direction::Left,now)+" | STRAIGHT "+permission(Direction::Straight,now)+" | RIGHT "+permission(Direction::Right,now);}
    void set_surround(const std::array<product::ViewRisk,4>& views){product_views=views;}
    void observe_objects(const ai::Result& result,double now){
        if(!result.finished||result.received<=object_received||result.received<loop_floor)return;
        object_received=result.received;front_counts={};
        if(now>=result.received&&now-result.received<=.4)for(const auto& t:result.tracks)
            if(!t.predicted&&t.box.category>=0&&t.box.category<8)++front_counts[size_t(t.box.category)];
        if(!front_risk.observe(result.tracks,result.received,now,corridor,still_image||(local_input&&!config.loop_frames)))return;
        auto r=front_risk.decision(now);
        risk_records<<std::fixed<<std::setprecision(6)<<now<<','<<result.received<<','<<result.frame_id<<','<<result.source<<','<<r.target<<','<<r.category<<','<<r.level<<','<<r.height<<','<<r.bottom<<','<<r.growth<<','<<r.assumed_region<<','<<r.held<<','<<r.still<<','<<r.reason<<'\n';
    }
    driver_demo::Explanation explanation(double now){
        driver_demo::Explanation out;auto lane=lane_decision(now);auto risk=front_risk.decision(now);auto selected_signal=state.signal_decision(selected(),now);
        out.scope=std::string("意图 ")+(selected()==Direction::Left?"左转":selected()==Direction::Right?"右转":"直行")+" · 模拟辅助提示";
        out.still=still_image||(local_input&&!config.loop_frames);
        bool front_fresh=object_received>=0&&now>=object_received&&now-object_received<=.4;
        bool signal_recent=signal_presence.recent(now);
        for(unsigned i=0;i<3;++i){auto fresh_signal=state.signal_decision(Direction(i),now);
            if(fresh_signal.light==Color::Red||fresh_signal.light==Color::Yellow)restriction_pending[i]=true;
            else if(fresh_signal.light==Color::Green)restriction_pending[i]=false;
            std::string p=permission(Direction(i),now);const auto& reason=association_reason[i];
            out.signal[i]=p=="ALLOW"?"信号允许":p=="STOP"?"信号禁止":!front_fresh?"前视证据不足":restriction_pending[i]?"信号暂失":
                !signal_recent?"当前未见灯":reason=="NO_AUTO_ARROW"?"未关联此方向":
                reason=="CONFLICTING_SAME_SIGNAL"||reason=="MULTIPLE_ARROW_SIGNALS"?"信号冲突":"信号待核实";
            if(p=="ALLOW"||p=="STOP")out.signal_light[i]=name(fresh_signal.stop_held?fresh_signal.held_light:fresh_signal.light);
            const auto& evidence=state.get(Direction(i));
            if(fresh_signal.stop_held)out.signal_detail[i]="沿用最近确认的灯色";
            else if(p=="ALLOW"||p=="STOP")out.signal_detail[i]=reason.find("LAYOUT_")==0?"按灯组布局判断·仅信号条件":reason=="BRIEF_DETECTION_GAP"||reason=="BRIEF_COLOR_GAP"?"短时缺失，沿用新鲜结果":"灯色与方向已关联";
            else if(!front_fresh)out.signal_detail[i]="前视结果暂缺或过期";
            else if(restriction_pending[i])out.signal_detail[i]="原禁止信号暂失，等待新观测或有限无灯退出";
            else if(!signal_recent)out.signal_detail[i]="无有效灯信号，继续依据车道与前方目标观察";
            else if(reason=="CONFLICTING_SAME_SIGNAL")out.signal_detail[i]="同组灯色冲突";
            else if(reason=="MULTIPLE_ARROW_SIGNALS")out.signal_detail[i]="同向多灯待关联";
            else if(reason=="TRACK_NOT_CONFIRMED")out.signal_detail[i]="灯框跟踪确认中";
            else if(reason=="STALE_OR_FUTURE_DETECTION")out.signal_detail[i]="信号结果过期";
            else if(config.automatic&&reason=="NO_AUTO_ARROW")out.signal_detail[i]=signal_recent?"未关联此方向":"当前未见适用灯";
            else if(evidence.raw!=Color::Unknown&&fresh_signal.age_ms>=0&&fresh_signal.age_ms<=config.settings.max_age_ms)
                out.signal_detail[i]=std::string(evidence.raw==Color::Red?"红色":evidence.raw==Color::Green?"绿色":"黄色")+"连续确认中";
            else out.signal_detail[i]="灯色证据不足";
        }
        // Report unassigned colors separately: they never grant a direction permission.
        if(config.automatic&&signal_recent&&last_auto_received>=0&&now>=last_auto_received&&(now-last_auto_received)*1000<=config.settings.max_age_ms){
            unsigned colors=0;bool unassigned=false;
            for(const auto& candidate:automatic_candidates)if(candidate.direction<0&&qualified_signal(candidate)){unassigned=true;
                colors|=candidate.color=="RED"?1u:candidate.color=="YELLOW"?2u:candidate.color=="GREEN"?4u:0u;}
            const char* label=colors==1?"见红色·方向未明":colors==2?"见黄色·方向未明":colors==4?"见绿色·方向未明":colors?"多色灯·方向未明":"灯色/方向待识别";
            if(unassigned)for(unsigned i=0;i<3;++i)if(association_reason[i]=="NO_AUTO_ARROW"&&!restriction_pending[i])out.signal_detail[i]=label;
            for(unsigned i=0;i<3;++i)if(!layout_hints.colors[i].empty()&&association_reason[i]=="NO_AUTO_ARROW"&&!restriction_pending[i]){
                const auto& hint=layout_hints.colors[i];out.signal_hint[i]=hint=="GREEN"?"推测绿灯":hint=="RED"?"推测红灯":"推测黄灯";
                out.signal_detail[i]="按灯组顺序推测·非方向确认";
            }
        }
        std::string motion=lane.motion;
        out.lane_styles={lane.left_style,lane.right_style};out.lane_offset=lane.offset;
        out.lane_position_known=motion!="UNKNOWN";out.lane_position_held=lane.position_held;
        out.lane_requested=lane.requested;out.lane_intent=lane.direction;out.intent=int(selected());
        if(lane.age_ms>=0&&lane.age_ms<=750)out.lane_pixels=lane_input.solid_pixels&&lane_input.dashed_pixels?"已识别实线/虚线像素":lane_input.solid_pixels?"已识别实线像素":lane_input.dashed_pixels?"已识别虚线像素":"未见足量标线像素";
        if(lane.age_ms<0)out.lane_detail="等待车道结果";
        else if(lane.age_ms>750)out.lane_detail="车道结果过期";
        else if((lane.left_style!=0)!=(lane.right_style!=0)){
            bool left=lane.left_style!=0;int style=left?lane.left_style:lane.right_style;
            const auto& missing=left?lane_input.right_reason:lane_input.left_reason;
            out.lane_detail=std::string(left?"左":"右")+(style==1?"实线":"虚线")+"已确认，"+(left?"右":"左")+
                (missing=="VEHICLE_OCCLUSION"?"侧车辆遮挡":missing=="TEMPORAL_CONFIRMATION"?"侧类别确认中":"侧边界不足");
        }
        else if(lane_input.left_reason=="NO_CURVE"||lane_input.right_reason=="NO_CURVE")
            out.lane_detail=lane_input.pixels_seen?"有像素候选，边界未拟合":"未找到本车道边界";
        else if(lane_input.left_reason=="VEHICLE_OCCLUSION"||lane_input.right_reason=="VEHICLE_OCCLUSION")out.lane_detail="车辆遮挡，标线待确认";
        else if(!lane_input.valid)out.lane_detail="边界未配对，位置待确认";
        else if(!lane.left_style||!lane.right_style)out.lane_detail="位置可用，标线类别确认中";
        else out.lane_detail="边界已关联，侧后仍需检查";
        if(lane.style_held)out.lane_detail="标线短时保持，等待新帧更新";
        out.lane=motion=="CENTERED"?"当前位于车道中央":motion=="LEFT_OF_CENTER"?"当前偏左":motion=="RIGHT_OF_CENTER"?"当前偏右":motion=="NEAR_LEFT_BOUNDARY"?"靠近左侧边界":motion=="NEAR_RIGHT_BOUNDARY"?"靠近右侧边界":motion=="CROSS_LEFT_CANDIDATE"?"左跨线候选":motion=="CROSS_RIGHT_CANDIDATE"?"右跨线候选":"边界不足，暂不能判断偏移";
        auto style=[](int value){return value==1?"实线":value==2?"虚线":"未确认";};
        out.markings=std::string("左 ")+style(lane.left_style)+" / 右 "+style(lane.right_style)+(lane.position_held?" · 短时保持":"");
        if(lane.requested)out.lane_request=lane.alarm?"实线变道请求：禁止":lane.marking_permits?"虚线条件通过，侧后未验证":"变道请求：条件不足";
        if(risk.target){char text[128];std::snprintf(text,sizeof(text),"%s ID%llu · 高度占画面 %.0f%%",risk.category==0?"行人":"道路目标",static_cast<unsigned long long>(risk.target&0xffffffffu),risk.height*100);out.target=text;}
        else out.target=object_received>=0&&now-object_received<=.4?"当前未选中前方路径内目标":"前视证据暂缺或过期";
        out.risk=risk.assumed_region?"路径使用假设区域 · 非米制测距":"路径来自新鲜车道边界 · 非米制测距";
        out.source=std::string("人 ")+std::to_string(front_counts[0])+" 车 "+std::to_string(front_counts[2]+front_counts[3]+front_counts[4])+" 灯 "+std::to_string(front_counts[7]);
        if(risk.level==2){out.severity=3;out.action="停止 / STOP";out.reason=risk.held?"近目标短时保持，请继续观察":"前方目标画面很近，模拟停止";}
        else if(std::string(selected_signal.instruction)=="STOP"){out.severity=3;out.action="停止 / STOP";
            auto color=selected_signal.stop_held?selected_signal.held_light:selected_signal.light;
            out.reason=color==Color::Red?"当前意图方向红灯":color==Color::Yellow?"当前意图方向黄灯":"当前意图方向信号禁止";}
        else if(risk.level==1){out.severity=2;out.action="减速 / SLOW";out.reason=risk.category==0?"行人进入前方图像区域，模拟礼让":"前方目标较近或画面扩大，模拟减速";}
        else if(lane.alarm||std::string(lane.rule)=="SOLID_CROSS_CANDIDATE"){out.action="保持车道 / KEEP";out.reason="实线限制或跨线候选，请保持车道";}
        else if(std::string(selected_signal.instruction)=="ALLOW"){out.action="观察 / WATCH";out.reason="信号允许，仍需检查前方与侧后";}
        else if(!front_fresh){out.action="观察 / WATCH";out.reason="前视检测证据暂缺或过期";}
        else if(restriction_pending[unsigned(selected())]){out.action="观察 / WATCH";out.reason="原禁止信号暂失，等待新观测或有限无灯退出";}
        else if(front_fresh&&!signal_recent){out.severity=0;out.action="保持观察 / KEEP";out.reason="当前未见灯，未触发前方风险规则";}
        else {out.action="观察 / WATCH";out.reason=signal_recent?"信号存在或暂丢失，适用方向待核实":"前视检测证据暂缺或过期";}
        if(product_views[0].fresh&&product_views[0].person>0&&out.severity<(product_views[0].person==2?3:2)){
            out.severity=product_views[0].person==2?3:2;out.action=product_views[0].person==2?"停止 / STOP":"减速 / SLOW";out.reason="行人进入已标定前方近区";
        }
        if(product_views[0].image_fresh&&product_views[0].proximity>0){
            int level=product_views[0].proximity>=2?3:2;
            if(level>out.severity){out.severity=level;out.action=level==3?"报警 / ALERT":"减速 / SLOW";out.reason=level==3?"前方图像目标很近，请注意避让":"前方图像目标靠近，请减速观察";}
        }
        if(lane.requested&&(lane.side_verified||std::string(lane.rule)=="BLOCKED_SIDE_OCCUPIED"||std::string(lane.rule)=="BLOCKED_REAR_APPROACH")){
            out.lane_request=lane.allowed?"侧后连续未见目标 · 允许变道":std::string(lane.rule)=="BLOCKED_SIDE_OCCUPIED"?std::string(lane.direction==0?"左侧":"右侧")+"目标靠近 · 禁止变道":std::string(lane.rule)=="BLOCKED_REAR_APPROACH"?"后方过近或快速接近 · 禁止变道":out.lane_request;
            if(lane.alarm){out.severity=3;if(risk.level<2&&product_views[0].person<2&&std::string(selected_signal.instruction)!="STOP"){out.action="禁止变道 / KEEP";out.reason=out.lane_request;}}
        }
        if(product_views[3].fresh&&product_views[3].approaching&&out.severity<2){out.severity=2;out.action="后方告警 / WATCH";out.reason="后方目标持续接近，请注意";}
        out.near_fresh[0]=front_fresh;out.near_levels[0]=risk.level;
        for(unsigned ch:camera_roles::Order)if(ch){const auto& v=product_views[ch];
            out.near_fresh[ch]=v.image_fresh;out.near_levels[ch]=v.proximity;out.near_held[ch]=v.proximity_held;out.near_approaching[ch]=v.image_approaching;
            if(!v.image_fresh||!driver_demo::proximity_alert_active(out,ch))continue;
            int level=v.proximity>=2?3:v.proximity||v.image_approaching?2:0;
            if(level>out.severity){out.severity=level;out.action=level==3?"侧后危险 / WARN":"侧后关注 / CAUTION";
                out.reason=std::string(camera_roles::name(ch))+(v.image_approaching?"目标持续接近，请注意":level==3?"目标很近，请注意避让":"目标靠近，请保持间距");
                char text[96];std::snprintf(text,sizeof(text),"%s目标框面积 %.1f%% · 图像近远，非米制",camera_roles::name(ch),v.proximity_area*100);out.target=text;}
        }
        if(product_views[0].distance>=0&&out.action!="侧后危险 / WARN"&&out.action!="侧后关注 / CAUTION"&&out.action!="禁止变道 / KEEP"){char distance[80];std::snprintf(distance,sizeof(distance),"行人地面估距约 %.1f m · 已标定区域",product_views[0].distance);out.target=distance;}
        if(lane.requested){
            std::string rule=lane.rule,detail=lane.allowed?"虚线、信号及侧后图像条件通过":rule=="BLOCKED_SIGNAL_STOP"?"当前行驶方向红/黄灯限制":
                rule=="WAIT_SIGNAL_UNCONFIRMED"?"当前行驶方向信号待确认":rule=="WAIT_FRONT_EVIDENCE"?"前视结果待更新":rule=="WAIT_LANE_EVIDENCE"?"本车道边界待更新":rule=="BLOCKED_FRONT_RISK"?"前方目标较近":
                rule=="BLOCKED_SIDE_OCCUPIED"?"请求侧有近距目标":rule=="BLOCKED_REAR_APPROACH"?"后方过近或快速接近":
                rule=="ALARM_SOLID_REQUEST"||rule=="SOLID_CROSS_CANDIDATE"?"实线限制":!lane.marking_permits?"标线类别待确认":"侧后净空尚未确认";
            out.lane_request=std::string(lane.direction==0?"向左变道：":"向右变道：")+detail;
            if(out.severity<2){out.severity=lane.allowed?0:1;out.action=lane.allowed?"允许变道 / CHANGE":"保持车道 / KEEP";out.reason=out.lane_request;}
        }
        if(out.still)out.scope="图片检测中 · 不判断接近运动";
        driver_demo::VehicleInput input;input.manual_stop=manual_stop;input.forced_alarm=forced_alarm;input.still=out.still;input.front_fresh=front_fresh;
        input.signal_stop=std::string(selected_signal.instruction)=="STOP";input.road_ready=selected_signal.allowed||(!signal_recent&&!restriction_pending[unsigned(selected())]);
        input.front_stop=risk.level>=2||product_views[0].person>=2;
        input.front_stop|=product_views[0].image_fresh&&product_views[0].proximity>=2;
        input.slow=risk.level==1||(product_views[0].fresh&&product_views[0].person==1)||(product_views[0].image_fresh&&product_views[0].proximity==1);
        input.rear_alert=(product_views[3].image_fresh&&(product_views[3].proximity>=2||product_views[3].image_approaching))||(product_views[3].fresh&&product_views[3].approaching);
        input.lane_requested=lane.requested;input.lane_allowed=lane.allowed;input.lane_alarm=lane.alarm;
        input.road_direction=int(selected());input.lane_direction=lane.direction;
        auto vehicle=driver_demo::vehicle_decision(input);out.vehicle_state=vehicle.state;out.vehicle_label=vehicle.label;out.vehicle_reason=std::string(vehicle.state)=="SLOW"?out.reason:vehicle.reason;
        out.vehicle_arrow=vehicle.arrow;out.vehicle_moving=vehicle.moving;out.vehicle_alarm=vehicle.alarm;out.manual_stop=manual_stop;
        if(vehicle.alarm)out.severity=3;
        out.ui_alarm=forced_alarm||(config.lane_rules&&(lane.alarm||std::string(lane.rule)=="SOLID_CROSS_CANDIDATE"));
        out.ui_alarm|=input.rear_alert||(front_fresh&&input.front_stop);
        for(unsigned ch:{camera_roles::Front,camera_roles::Rear})out.ui_alarm|=product_views[ch].image_fresh&&product_views[ch].proximity>=2;
        out.audible_alarm=out.ui_alarm; // Ordinary red-light waiting and passive side observations remain silent.
        return out;
    }
    Direction selected() const{return state.selected();}
    bool automatic() const{return config.automatic;}
    bool has_lane_rules()const{return config.lane_rules;}
    bool toolbar_alarm(double now){return explanation(now).ui_alarm;}
    std::string lane_feedback(double now){
        auto lane=lane_decision(now);std::string rule=lane.rule;
        if(rule=="BLOCKED_SIDE_OCCUPIED")return "BLOCKED - SIDE OCCUPIED";
        if(rule=="BLOCKED_REAR_APPROACH")return "BLOCKED - REAR APPROACH";
        if(lane.alarm||rule=="SOLID_CROSS_CANDIDATE")return "BLOCKED - SOLID LINE";
        if(!lane.requested)return "SELECTED - PRESS REQUEST";
        if(rule=="BLOCKED_SIGNAL_STOP")return "BLOCKED - SIGNAL STOP";
        if(rule=="WAIT_SIGNAL_UNCONFIRMED")return "WAIT - ROAD SIGNAL UNCONFIRMED";
        if(rule=="WAIT_FRONT_EVIDENCE")return "WAIT - FRONT EVIDENCE";
        if(rule=="WAIT_LANE_EVIDENCE")return "WAIT - LANE GEOMETRY";
        if(rule=="BLOCKED_FRONT_RISK")return "BLOCKED - FRONT RISK";
        if(rule=="SELECT_LEFT_OR_RIGHT")return "SELECT LEFT OR RIGHT";
        if(lane.allowed)return "LANE CHANGE PERMITTED - ADVISORY";
        if(lane.marking_permits)return "DASHED OK - REAR UNVERIFIED";
        return "WAIT - LINE TYPE UNCONFIRMED";
    }
    lane_demo::Decision lane_decision(double now){
        auto signal=state.signal_decision(selected(),now);
        auto d=lane_state.decision(now,std::string(signal.instruction)=="STOP");
        unsigned side=d.direction==0?camera_roles::Left:camera_roles::Right;const auto& s=product_views[side];const auto& rear=product_views[camera_roles::Rear];
        d.side_verified=(s.calibrated&&s.fresh&&rear.calibrated&&rear.fresh)||(s.image_clear&&rear.image_clear);
        // A confirmed near vehicle prohibits a request even with unknown markings.
        // Keep an already established solid-line prohibition as the primary reason.
        if(d.requested&&!d.alarm){
            if(s.image_fresh&&(s.proximity>0||s.image_approaching)){d.rule="BLOCKED_SIDE_OCCUPIED";d.reason="IMAGE_SIDE_NEAR";d.alarm=true;d.allowed=false;}
            else if(rear.image_fresh&&(rear.proximity>=2||rear.image_approaching)){d.rule="BLOCKED_REAR_APPROACH";d.reason="IMAGE_REAR_NEAR_OR_GROWING";d.alarm=true;d.allowed=false;}
        }
        if(d.requested&&d.marking_permits&&d.side_verified&&!d.alarm){
            if(s.occupied){d.rule="BLOCKED_SIDE_OCCUPIED";d.reason="CALIBRATED_SIDE_OCCUPIED";d.alarm=true;}
            else if(s.approaching||rear.approaching){d.rule="BLOCKED_REAR_APPROACH";d.reason="REAR_IMAGE_EXPANSION";d.alarm=true;}
            else if(object_received>=0&&now-object_received<=.4&&front_risk.decision(now).level==0&&product_views[0].person==0){d.rule="SIMULATED_LANE_CHANGE_ALLOWED";d.reason=s.image_clear&&rear.image_clear?"IMAGE_SIDE_REAR_CLEAR_600MS":"CALIBRATED_SIDE_REAR_CLEAR";d.allowed=true;}
        }
        // Road-turn intent and lateral manoeuvre are independent. A green
        // turn signal is never read from the lane-change direction by accident.
        if(d.requested&&d.marking_permits&&!d.alarm){
            if(!signal.allowed&&(signal_presence.recent(now)||restriction_pending[unsigned(selected())])){
                d.allowed=false;d.rule="WAIT_SIGNAL_UNCONFIRMED";d.reason="ROAD_INTENT_SIGNAL_UNCONFIRMED";
            }else if(object_received<0||now<object_received||now-object_received>.4){
                d.allowed=false;d.rule="WAIT_FRONT_EVIDENCE";d.reason="FRONT_RESULT_EXPIRED";
            }else if(!d.raw_valid){
                d.allowed=false;d.rule="WAIT_LANE_EVIDENCE";d.reason="FRESH_LANE_PAIR_REQUIRED";
            }else if(front_risk.decision(now).level>0||product_views[0].person>0){
                d.allowed=false;d.rule="BLOCKED_FRONT_RISK";d.reason="FRONT_TARGET_NEAR";
            }
        }
        return d;
    }
    void log_lane(const char* event,double now){if(!config.lane_rules)return;auto d=lane_decision(now);
        lane_records<<std::fixed<<std::setprecision(6)<<now<<','<<event<<','<<lane_input.frame_id<<','<<lane_input.source<<','<<lane_input.received<<','<<d.age_ms<<','<<name(Direction(d.direction))<<','<<d.motion<<','<<d.trend<<','<<d.rule<<','<<d.reason<<','<<d.offset<<','<<d.left_style<<','<<d.right_style<<','<<d.requested<<','<<d.marking_permits<<','<<d.side_verified<<','<<d.allowed<<','<<d.alarm<<','<<(std::string(state.signal_decision(selected(),now).instruction)=="STOP")<<','<<d.raw_valid<<','<<d.raw_motion<<','<<d.raw_offset<<','<<d.position_held<<','<<d.geometry_age_ms<<','<<d.unknown_run_ms<<','<<d.observations<<','<<d.unknown_observations<<','<<d.raw_reason<<','<<lane_input.pair_reason<<','<<lane_input.left_reason<<','<<lane_input.right_reason<<','<<left_style_evidence.solid_votes<<','<<left_style_evidence.dashed_votes<<','<<left_style_evidence.raw_style<<','<<left_style_evidence.confirmation_streak<<','<<left_style_evidence.direct<<','<<right_style_evidence.solid_votes<<','<<right_style_evidence.dashed_votes<<','<<right_style_evidence.raw_style<<','<<right_style_evidence.confirmation_streak<<','<<right_style_evidence.direct<<','<<d.style_held<<'\n';
        lane_records.flush();if(!lane_records)throw std::runtime_error("Lane rule log failed");}
    std::string toolbar_status(double now){auto d=state.signal_decision(selected(),now);auto lane=lane_decision(now);
        if(config.lane_rules)return std::string("COMBINED | ROAD ")+name(selected())+" "+permission(selected(),now)+" | "+(lane.requested?std::string("LANE ")+name(Direction(lane.direction))+" "+lane_feedback(now):"LANE MONITORING");
        return config.automatic?permissions_text(now):std::string("MANUAL ROI ")+name(d.direction)+" CMD "+d.instruction;
    }
    std::string toolbar_detail(double now){auto d=state.signal_decision(selected(),now);char age[160];
        std::snprintf(age,sizeof(age),"SIGNAL %s AGE %.0FMS",name(d.stop_held?d.held_light:d.light),d.stop_held?d.stop_age_ms:d.age_ms);
        if(config.lane_rules){auto lane=lane_decision(now);auto marking=[](int style){return style==1?"SOLID":style==2?"DASHED":"UNKNOWN";};
            char detail[160];std::snprintf(detail,sizeof(detail)," | OFFSET %+.2F L %s R %s | J/M request lane; L cancels",lane.offset,marking(lane.left_style),marking(lane.right_style));return std::string(age)+detail;}
        return std::string(age)+" "+(config.automatic?association_reason[unsigned(d.direction)]:"manually bound signal");
    }
    void log(const std::string& event,double now){
        auto d=state.decision(now);const auto& r=config.rois[unsigned(d.direction)];
        commands<<std::fixed<<std::setprecision(6)<<now<<','<<event<<','<<name(d.direction)<<','<<name(d.light)<<','<<d.requested<<','<<d.allowed<<','<<d.alarm<<','<<d.state<<','<<d.age_ms<<','<<frame_id<<','<<source<<','<<r.box.x1<<','<<r.box.y1<<','<<r.box.x2<<','<<r.box.y2<<','<<d.stop_held<<','<<d.stop_age_ms<<','<<d.instruction<<'\n';
        commands.flush();if(!commands)throw std::runtime_error("Signal command log failed");
    }
    void record_operation(char command,double now,const char* origin="界面"){
        auto lane=lane_decision(now);auto detail=explanation(now);
        const char* label=command=='a'?"选择路口左转":command=='w'?"选择直行方向":command=='d'?"选择路口右转":command=='j'?"请求左变道":command=='m'?"请求右变道":command==' '?"请求恢复行驶":command=='s'?"停车提示":command=='l'?"取消变道":command=='c'?"重新核查":command=='p'?"暂停或继续素材":command=='n'?"切换素材":command=='v'?"恢复前视实时":command=='k'?"请求驻车模式":command=='u'?"请求全景模式":"其他操作";
        auto field=[&](const std::string& value){operation_records<<'"';for(char c:value){if(c=='"')operation_records<<'"';operation_records<<(c=='\n'||c=='\r'?' ':c);}operation_records<<'"';};
        operation_records<<std::fixed<<std::setprecision(6)<<now<<',';field(label);operation_records<<',';field(origin);operation_records<<',';field(name(selected()));operation_records<<',';field(name(Direction(lane.direction)));operation_records<<','<<lane.requested<<',';field(lane.rule);operation_records<<',';field(detail.vehicle_state);operation_records<<',';field(detail.action);operation_records<<',';field(detail.reason);operation_records<<'\n';
        operation_records.flush();if(!operation_records)throw std::runtime_error("Operation log failed");
    }
    void key(char key,double now,const char* origin="界面"){
        if(key=='c'){layout_hints.reset();association.reset();}
        if(key=='a'||key=='w'||key=='d'){forced_alarm=false;state.select(key=='a'?Direction::Left:key=='d'?Direction::Right:Direction::Straight);lane_state.stop();log("SELECT_ROAD_DIRECTION",now);log_lane("SELECT_ROAD_DIRECTION",now);}
        else if((key=='j'||key=='m')&&config.lane_rules){manual_stop=false;forced_alarm=false;state.stop();lane_state.select(key=='j'?0:2);lane_state.request();log("SIM_LANE_REQUEST",now);log_lane("SIM_LANE_REQUEST",now);}
        else if(key==' '){manual_stop=false;forced_alarm=std::string(state.signal_decision(selected(),now).instruction)=="STOP"||front_risk.decision(now).level>=2||product_views[0].person>=2||(product_views[0].image_fresh&&product_views[0].proximity>=2);lane_state.stop();state.go(now);log("SIM_GO_REQUEST",now);log_lane("SIM_GO_REQUEST",now);}
        else if(key=='s'){manual_stop=true;forced_alarm=false;state.stop();lane_state.stop();log("STOP_ACKNOWLEDGE",now);log_lane("STOP_ACKNOWLEDGE",now);}
        else if(key=='l'&&config.lane_rules){lane_state.stop();log("CANCEL_LANE_REQUEST",now);log_lane("CANCEL_LANE_REQUEST",now);}
        else if(key=='c'){
            forced_alarm=false;lane_state.reset();lane_left.reset();lane_right.reset();lane_left_visible=lane_right_visible=false;lane_blocked.clear();lane_input={};left_style_evidence={};right_style_evidence={};log_lane("RECHECK_LANE",now);
            structure.reset();config.rois[unsigned(selected())]={};automatic_ids[unsigned(selected())]=0;associated_boxes[unsigned(selected())]={};cached_signals.clear();automatic_candidates.erase(std::remove_if(automatic_candidates.begin(),automatic_candidates.end(),[&](const ai::SignalCandidate& c){return c.direction==int(selected());}),automatic_candidates.end());state.reset(selected());state.stop();log(config.automatic?"RECHECK_AUTO_SIGNAL":"CLEAR_MANUAL_ROI",now);
        }
        if(std::string("awdjm scl").find(key)!=std::string::npos)record_operation(key,now,origin);
    }

    void set_roi(float x1,float y1,float x2,float y2,double now){
        if(config.automatic){log("MANUAL_ROI_DISABLED_IN_AUTO_MODE",now);return;}
        auto a=std::min(x1,x2),b=std::min(y1,y2),c=std::max(x1,x2),d=std::max(y1,y2);
        if(a<0||b<0||c>1280||d>720||c-a<4||d-b<6||c-a>512||d-b>512)return;
        config.rois[unsigned(selected())]={true,{a,b,c,d,1,7}};state.reset(selected());state.stop();log("BIND_MANUAL_ROI",now);
    }
    void focus_lost(double now){log("FOCUS_LOST_UI_ONLY",now);log_lane("FOCUS_LOST_UI_ONLY",now);}
    void observe(const uint8_t* pixels,uint32_t id,uint32_t src,double received,double now,bool local){
        if(received==last_received)return;
        if(local && config.loop_frames && src){uint64_t loop=(uint64_t(src)-1)/config.loop_frames;
            if(have_loop&&loop!=previous_loop){layout_hints.reset();association.reset();}
            if(have_loop && loop!=previous_loop){auto prior_lane=lane_state.decision(now);state.reset_all();structure.reset();front_risk.reset();corridor={};object_received=-1;signal_presence.reset();restriction_pending={};lane_state.reset();if(prior_lane.requested){lane_state.select(prior_lane.direction);lane_state.request();}lane_left.reset();lane_right.reset();lane_left_visible=lane_right_visible=false;lane_blocked.clear();lane_input={};left_style_evidence={};right_style_evidence={};automatic_ids={};associated_boxes={};automatic_candidates.clear();cached_signals.clear();loop_floor=received;log("LOCAL_LOOP_RESET",now);log_lane("LOCAL_LOOP_RESET",now);}
            previous_loop=loop;have_loop=true;
        }
        last_received=received;local_input=local;
        if(config.automatic)return; // AUTO colors arrive with the matching detector frame, not a newer preview.
        frame_id=id;source=src;
        auto start=qv::now_seconds();
        for(unsigned i=0;i<3;++i){const auto& r=config.rois[i];ai::LightCandidate candidate;
            if(r.enabled){auto rgb=ai::light_roi_rgb565(pixels,r.box);if(!rgb.empty())candidate=ai::classify_light(rgb,r.box,config.min_s,config.min_v,config.red_high,config.min_ratio);}
            else candidate.reason="DIRECTION_NOT_BOUND";
            state.observe(Direction(i),color(candidate.color),received,src,now);
            observations<<std::fixed<<std::setprecision(6)<<now<<','<<received<<','<<id<<','<<src<<','<<name(Direction(i))<<','<<r.enabled<<','<<candidate.color<<','<<name(state.get(Direction(i)).stable)<<','<<candidate.reason<<','<<candidate.components[0]<<','<<candidate.components[1]<<','<<candidate.components[2]<<','<<(qv::now_seconds()-start)*1000<<'\n';
        }
        last_analysis_ms=(qv::now_seconds()-start)*1000;observations.flush();
        if(!observations)throw std::runtime_error("Signal observation log failed");
    }
    void observe_automatic(const ai::Result& result,double now){
        if(!config.automatic||!result.finished||result.received<=last_auto_received||result.received<loop_floor)return;
        last_auto_received=result.received;frame_id=result.frame_id;source=result.source;automatic_candidates=result.signal_candidates;
        bool fresh=result.received<=now&&(now-result.received)*1000<=config.settings.max_age_ms;
        if(fresh)layout_hints.update(automatic_candidates,result.received);else layout_hints.reset();
        for(unsigned i=0;i<3;++i)layout_records<<std::fixed<<std::setprecision(6)<<now<<','<<result.received<<','<<frame_id<<','<<source<<','<<name(Direction(i))<<','<<layout_hints.colors[i]<<','<<layout_hints.reason<<",0\n";
        if(fresh){structure.update(automatic_candidates,result.received);
            if(config.practical_association)association.update(automatic_candidates,result.received);}
        else association.reset();
        unsigned qualified=0;for(const auto& c:automatic_candidates)
            qualified+=qualified_signal(c);
        bool had_signal=signal_presence.recent(now);
        if(fresh)signal_presence.observe(result.received,qualified!=0);
        const char* scene_event=!fresh?"IGNORE_STALE":qualified&&!had_signal?"ENTER_VALID_SIGNAL":"OBSERVE";
        if(config.box_hold_ms){
            cached_signals.erase(std::remove_if(cached_signals.begin(),cached_signals.end(),[&](const CachedSignal& old){return (now-old.received)*1000>config.box_hold_ms;}),cached_signals.end());
            if(fresh)for(const auto& c:automatic_candidates)if(c.track_id){
                cached_signals.erase(std::remove_if(cached_signals.begin(),cached_signals.end(),[&](const CachedSignal& old){return c.direction>=0&&old.candidate.direction==c.direction&&old.candidate.track_id!=c.track_id&&old.received<result.received;}),cached_signals.end());
                auto found=std::find_if(cached_signals.begin(),cached_signals.end(),[&](const CachedSignal& old){return old.candidate.track_id==c.track_id;});
                if(found!=cached_signals.end())found->update(c,result.received);
                else if(cached_signals.size()<32){CachedSignal entry;entry.update(c,result.received);cached_signals.push_back(entry);}
            }
        }
        for(int d=0;d<3;++d){std::vector<const ai::SignalCandidate*> groups;bool conflict=false;
            for(const auto& c:automatic_candidates)if(c.direction==d){
                bool duplicate=false;for(auto*& old:groups)if((c.track_id&&c.track_id==old->track_id)||(config.practical_association&&c.association_id&&c.association_id==old->association_id)||ai::overlap(c.box,old->box)>.6f){
                    duplicate=true;
                    if(config.practical_association){
                        bool known=color(c.color)!=Color::Unknown,old_known=color(old->color)!=Color::Unknown;
                        if(known&&old_known&&c.color!=old->color)conflict=true;
                        else if(known&&!old_known)old=&c;
                    }else if(c.color!=old->color)conflict=true;
                    break;
                }
                if(!duplicate)groups.push_back(&c);
            }
            const ai::SignalCandidate* chosen=fresh&&!conflict&&groups.size()==1&&groups[0]->track_id?groups[0]:nullptr;
            auto& reason=association_reason[d];reason=!fresh?"STALE_OR_FUTURE_DETECTION":conflict?"CONFLICTING_SAME_SIGNAL":groups.size()>1?"MULTIPLE_ARROW_SIGNALS":groups.empty()?"NO_AUTO_ARROW":!groups[0]->track_id?"TRACK_NOT_CONFIRMED":"UNIQUE_VISIBLE_ARROW_NOT_EGO_LANE_PROOF";
            if(chosen&&chosen->reason=="TRACKED_ARROW_STRUCTURE_FRESH_COLOR")reason="TRACKED_ARROW_STRUCTURE_FRESH_COLOR_NOT_EGO_LANE_PROOF";
            if(chosen&&!chosen->direction_basis.empty())reason=chosen->direction_basis;
            if(conflict||groups.size()>1){state.reset(Direction(d));cached_signals.erase(std::remove_if(cached_signals.begin(),cached_signals.end(),[&](const CachedSignal& c){return c.candidate.direction==d;}),cached_signals.end());}
            Color observed=Color::Unknown;auto& roi=config.rois[d];roi={};
            if(chosen){
                uint64_t identity=chosen->association_id?chosen->association_id:chosen->track_id;
                if(automatic_ids[d]!=identity){state.reset(Direction(d));if(selected()==Direction(d))state.stop();automatic_ids[d]=identity;}
                observed=color(chosen->color);roi={true,chosen->box};associated_boxes[d]=chosen->box;
            }
            const auto& previous=state.get(Direction(d));
            // One missing detector frame may retain a still-fresh decision. Do not
            // renew its timestamp, and never bridge a reported color conflict.
            bool weak_color=chosen&&observed==Color::Unknown&&(chosen->reason=="INSUFFICIENT_COLOR_PIXELS"||chosen->reason=="NO_COLOR_COMPONENT");
            bool contrary=false;
            if(groups.empty()&&automatic_ids[d])for(const auto& c:automatic_candidates)
                if((c.track_id&&c.track_id==automatic_ids[d])||(c.association_id&&c.association_id==automatic_ids[d])||ai::overlap(c.box,associated_boxes[d])>.45f)
                    contrary|=(c.direction>=0&&c.direction!=d)||(color(c.color)!=Color::Unknown&&color(c.color)!=previous.raw)||c.shape=="DOWN"||
                        (color(c.color)==Color::Unknown&&c.reason!="INSUFFICIENT_COLOR_PIXELS"&&c.reason!="NO_COLOR_COMPONENT");
            bool brief_gap=config.practical_association&&fresh&&!conflict&&!contrary&&(groups.empty()||weak_color)&&previous.raw!=Color::Unknown&&
                result.received>previous.last&&(now-previous.last)*1000<=std::min(300.,config.settings.max_age_ms);
            if(brief_gap)reason=weak_color?"BRIEF_COLOR_GAP":"BRIEF_DETECTION_GAP";
            else state.observe(Direction(d),observed,result.received,source,now);
            const auto& b=roi.box;
            const auto& used=state.get(Direction(d));
            automatic_records<<std::fixed<<std::setprecision(9)<<now<<','<<result.received<<','<<frame_id<<','<<source<<','<<name(Direction(d))<<','<<(chosen?chosen->track_id:0)<<','<<reason<<','<<name(observed)<<','<<name(used.stable)<<','<<b.x1<<','<<b.y1<<','<<b.x2<<','<<b.y2<<','<<(chosen?chosen->reason:"NO_CANDIDATE")<<','<<(chosen?chosen->direction_basis:"")<<','<<(used.last<0?-1:(now-used.last)*1000)<<','<<(chosen?chosen->association_id:0)<<'\n';
        }
        // Continuous fresh absence ends an observation episode, not a road right of way.
        bool stop_grace=false;for(unsigned i=0;i<3;++i){auto d=state.signal_decision(Direction(i),now);stop_grace|=d.stop_held||d.light==Color::Red||d.light==Color::Yellow;}
        double absence=signal_presence.absence_ms(),valid_age=signal_presence.age_ms(now);unsigned absent=signal_presence.absent_samples();
        if(fresh&&signal_presence.may_exit(now)&&!stop_grace){
            state.reset_all();restriction_pending={};automatic_ids={};associated_boxes={};cached_signals.clear();structure.reset();association.reset();layout_hints.reset();
            config.rois={};association_reason={"NO_AUTO_ARROW","NO_AUTO_ARROW","NO_AUTO_ARROW"};signal_presence.reset();scene_event="EXIT_NO_VALID_SIGNAL";
            log(scene_event,now);
        }
        scene_records<<std::fixed<<std::setprecision(9)<<now<<','<<result.received<<','<<frame_id<<','<<source<<','<<automatic_candidates.size()<<','<<qualified<<','
            <<signal_presence.recent(now)<<','<<valid_age<<','<<absence<<','<<absent<<','<<scene_event<<'\n';
        automatic_records.flush();if(!automatic_records)throw std::runtime_error("Cannot write automatic association log");
    }
    void observe_lane(const ai::LaneMask& mask,double now){if(!config.lane_rules||mask.received<loop_floor)return;
        lane_demo::Input in;in.frame_id=mask.frame_id;in.source=mask.source;in.received=mask.model_received>0?mask.model_received:mask.received;
        in.valid=mask.four_class&&mask.geometry.valid&&mask.position_valid;in.propagated=mask.cpu_propagated;
        in.ego=0;in.left=-mask.left_margin;in.right=mask.right_margin;in.offset=mask.relative_offset;in.left_style=mask.left_style;in.right_style=mask.right_style;
        in.left_valid=mask.four_class&&mask.left_curve.valid&&std::string(mask.left_evidence.reason)=="CONFIRMED_CANDIDATE";
        in.right_valid=mask.four_class&&mask.right_curve.valid&&std::string(mask.right_evidence.reason)=="CONFIRMED_CANDIDATE";
        in.pair_reason=mask.pair_reason;in.left_reason=mask.left_evidence.reason;in.right_reason=mask.right_evidence.reason;
        if(lane_state.observe(in,now)){
            left_style_evidence=mask.left_evidence;right_style_evidence=mask.right_evidence;
            unsigned solid=0,dashed=0;for(uint8_t c:mask.data){solid+=c==1;dashed+=c==2;}
            in.pixels_seen=solid+dashed>0;
            in.solid_pixels=solid>=40;in.dashed_pixels=dashed>=40;
            lane_input=in;corridor={mask.left_curve,mask.right_curve,in.received,in.valid};
            lane_left_visible=mask.left_curve.valid;lane_right_visible=mask.right_curve.valid;lane_blocked=mask.blocked;
            lane_left.observe(mask.left_curve,in.received);lane_right.observe(mask.right_curve,in.received);
            log_lane("MODEL_OBSERVATION",now);}}
    Decision tick(double now,driver_demo::Explanation* published=nullptr){auto d=state.decision(now);
        if(d.alarm&&!forced_alarm){state.stop();d=state.decision(now);log("AUTO_STOP_CANCELS_OLD_GO_REQUEST",now);}
        for(unsigned i=0;i<3;++i){auto direction=Direction(i);std::string value=permission(direction,now);
            if(value!=last_permissions[i]){last_permissions[i]=value;auto decision=state.signal_decision(direction,now);
                permission_records<<std::fixed<<std::setprecision(6)<<now<<','<<name(direction)<<','<<value<<','<<name(decision.light)<<','<<decision.age_ms<<','<<decision.stop_held<<','<<still_image<<','<<frame_id<<','<<source<<','<<association_reason[i]<<'\n';}}
        permission_records.flush();if(!permission_records)throw std::runtime_error("Cannot write permission log");
        if(last_state!=d.state){last_state=d.state;log("STATE_CHANGE",now);}
        if(last_instruction!=d.instruction){last_instruction=d.instruction;log("INSTRUCTION_CHANGE",now);std::cout<<"SIGNAL SIM dir="<<name(d.direction)<<" instruction="<<d.instruction<<" light="<<name(d.light)<<" state="<<d.state<<"\n";}
        if(config.lane_rules){auto lane=lane_decision(now);std::string value=std::string(lane.motion)+":"+lane.trend+":"+lane.rule;if(value!=last_lane_rule){last_lane_rule=value;log_lane("LANE_STATE_CHANGE",now);}
            std::string notice=std::string(name(Direction(lane.direction)))+":"+lane.rule;
            if(notice!=last_lane_notice){last_lane_notice=notice;std::cout<<"LANE SIM dir="<<name(Direction(lane.direction))<<" feedback="<<lane_feedback(now)<<" rule="<<lane.rule<<" alarm="<<lane.alarm<<"\n";}}
        auto advice=explanation(now);auto joint_lane=lane_decision(now);std::string key=advice.vehicle_state+advice.vehicle_reason+advice.action+advice.reason+name(selected())+permission(selected(),now)+joint_lane.rule+std::to_string(joint_lane.direction)+std::to_string(joint_lane.requested);
        if(key!=last_advice){last_advice=key;advice_records<<std::fixed<<std::setprecision(6)<<now<<','<<advice.severity<<','<<advice.action<<','<<advice.reason<<','<<advice.scope<<','<<name(selected())<<','<<permission(selected(),now)<<','<<name(Direction(joint_lane.direction))<<','<<joint_lane.requested<<','<<joint_lane.rule<<','<<joint_lane.allowed<<','<<advice.vehicle_state<<','<<advice.vehicle_arrow<<','<<advice.vehicle_moving<<','<<advice.vehicle_alarm<<'\n';}
        if(now-last_risk_flush>=1){risk_records.flush();advice_records.flush();layout_records.flush();scene_records.flush();last_risk_flush=now;if(!risk_records||!advice_records||!layout_records||!scene_records)throw std::runtime_error("Driver explanation log failed");}
        if(published)*published=std::move(advice);
        return d;}
    // Localization only: all explanatory prose and alarms live outside the video.
    void draw(uint8_t* nv12,unsigned channel,double now){
        int ox=int(channel%2)*640,oy=int(channel/2)*360;
        if(config.lane_rules){
            bool raw=lane_input.received>=0&&(now-lane_input.received)*1000<=750;
            auto curve=[&](const lane_demo::CurveView& view,bool available){
                bool held=false;auto c=view.at(now,available,held);if(!c.valid)return;
                for(double y=c.ymin;y<=c.ymax;y+=.75){if(held&&int(y/8)%2)continue;
                    int x=int(std::lround(c.x(y)*.9)),yy=int(std::lround(y*.9));
                    if(x<0||x>=ui::ContentW-1||yy<0||yy>=ui::ContentH)continue;
                    if(lane_blocked.size()==size_t(ui::ContentW*ui::ContentH)&&lane_blocked[yy*ui::ContentW+x])continue;
                    auto ink=held?ui::Color{180,85,170}:ui::White;
                    ui::pixel(nv12,ox+ui::ContentX+x,oy+ui::ContentY+yy,ink);ui::pixel(nv12,ox+ui::ContentX+x+1,oy+ui::ContentY+yy,ink);
                }
            };
            curve(lane_left,raw&&lane_left_visible);curve(lane_right,raw&&lane_right_visible);
        }
        const ui::Color red{81,90,240},amber{210,16,146};
        auto box=[&](const ai::Box& b,ui::Color ink,bool held){
            int x1=ox+ui::ContentX+std::clamp(int(b.x1*.45f),0,ui::ContentW-1),x2=ox+ui::ContentX+std::clamp(int(b.x2*.45f),0,ui::ContentW-1);
            int y1=oy+ui::ContentY+std::clamp(int(b.y1*.45f),0,ui::ContentH-1),y2=oy+ui::ContentY+std::clamp(int(b.y2*.45f),0,ui::ContentH-1);
            if(!held){ui::rectangle(nv12,x1,y1,x2,y2,ink);return;}
            for(int x=x1;x<=x2;++x)if((x-x1)%8<4){ui::pixel(nv12,x,y1,ink);ui::pixel(nv12,x,y2,ink);}
            for(int y=y1;y<=y2;++y)if((y-y1)%8<4){ui::pixel(nv12,x1,y,ink);ui::pixel(nv12,x2,y,ink);}
        };
        if(config.automatic){
            if(last_auto_received>=0&&(now-last_auto_received)*1000<=config.settings.max_age_ms)for(const auto& c:automatic_candidates){
                auto found=std::find_if(cached_signals.begin(),cached_signals.end(),[&](const CachedSignal& old){return old.candidate.track_id==c.track_id;});
                box(found!=cached_signals.end()?found->box(now):c.box,c.color=="RED"?red:c.color=="GREEN"?ui::Green:amber,false);
            }
            for(const auto& old:cached_signals){double age=(now-old.received)*1000;if(age<0||age>config.box_hold_ms)continue;
                bool current=false;for(const auto& c:automatic_candidates)if(c.track_id==old.candidate.track_id&&now-last_auto_received<=.3){current=true;break;}
                if(!current)box(old.box(now),amber,true);
            }
        }else for(const auto& roi:config.rois)if(roi.enabled)box(roi.box,amber,false);
    }
};
}
