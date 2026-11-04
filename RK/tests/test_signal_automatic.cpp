#include "signal_control.hpp"
#include "signal_shape.hpp"
#include <filesystem>
#include <cassert>
#include <unistd.h>
int main(){
    namespace fs=std::filesystem;using namespace signal_demo;
    { // A UI tick, duplicate, stale gap, or one isolated empty frame cannot end a scene.
        SignalPresence p;p.observe(1,false);assert(!p.recent(1)&&!p.may_exit(1));
        p.observe(2,true);p.observe(2.1,false);
        for(int i=0;i<20;++i)p.observe(2.1,false);
        assert(p.absent_samples()==1&&!p.may_exit(3.5));
        p.observe(5,false);assert(p.absent_samples()==1&&!p.may_exit(5));
        for(int i=1;i<=11;++i)p.observe(5+i*.1,false);assert(!p.may_exit(6.1));
        p.observe(6.2,false);assert(p.may_exit(6.2));
        p.observe(6.3,true);assert(!p.may_exit(6.3)&&p.absent_samples()==0);
        p.observe(std::nan(""),false);assert(p.recent(6.31));p.reset();assert(!p.recent(6.31));
    }
    auto root=fs::temp_directory_path()/("signal_auto_"+std::to_string(getpid()));assert(fs::create_directory(root));
    auto cfg=load("configs/auto_red.json");cfg.automatic=true;cfg.loop_frames=1000;cfg.practical_association=false;
    Control control(cfg,root.string());
    ai::Result result;result.received=100;result.finished=100.05;
    ai::SignalCandidate straight;straight.box={500,100,580,180,.2f,7};straight.track_id=10;straight.direction=1;straight.color="RED";straight.shape="STRAIGHT";
    ai::SignalCandidate left=straight;left.track_id=11;left.direction=0;left.color="GREEN";left.shape="LEFT";left.box={300,100,380,180,.99f,7};
    for(int i=0;i<4;++i){result.received=100+i*.1;result.finished=result.received+.05;result.frame_id=result.source=i+1;straight.box.x1+=2;straight.box.x2+=2;result.signal_candidates={left,straight};control.observe(nullptr,result.frame_id,result.source,result.received,result.finished,true);control.observe_automatic(result,result.finished);}
    auto chosen=control.tick(result.finished);assert(chosen.light==Color::Red);control.key(' ',result.finished);assert(control.tick(result.finished).alarm);
    control.key('s',result.finished);control.key('a',result.finished);assert(control.tick(result.finished).light==Color::Green);control.key(' ',result.finished);assert(control.tick(result.finished).allowed);
    // Missing detection invalidates color; a predicted/held image is not fresh evidence.
    result.received+=.1;result.finished=result.received+.05;result.frame_id++;result.source++;result.signal_candidates.clear();control.observe_automatic(result,result.finished);assert(!control.tick(result.finished).allowed);
    // One unique green circle is detected but never silently assigned to straight.
    control.key('w',result.finished);auto circle=straight;circle.direction=-1;circle.shape="CIRCLE";circle.color="GREEN";
    for(int i=0;i<4;++i){result.received+=.1;result.finished=result.received+.05;result.signal_candidates={circle};control.observe_automatic(result,result.finished);}
    control.key(' ',result.finished);assert(!control.tick(result.finished).allowed&&control.tick(result.finished).light==Color::Unknown);
    // Conflicting arrows are unknown even when one has a higher score.
    auto competitor=straight;competitor.track_id=99;competitor.color="GREEN";competitor.box={750,100,830,180,.99f,7};
    for(int i=0;i<4;++i){result.received+=.1;result.finished=result.received+.05;result.signal_candidates={straight,competitor};control.observe_automatic(result,result.finished);}
    assert(control.tick(result.finished).light==Color::Unknown&&!control.tick(result.finished).allowed);
    // Manual box assignment cannot override automatic mode.
    control.set_roi(5,5,50,50,result.finished);assert(!control.tick(result.finished).allowed);
    // Repeating the same detector result cannot meet the minimum sample count.
    control.key('c',result.finished);result.received+=.1;result.finished=result.received+.05;result.signal_candidates={straight};
    for(int i=0;i<10;++i)control.observe_automatic(result,result.finished);
    assert(control.tick(result.finished).light==Color::Unknown);
    // A fresh raw-input loop reset rejects late results from the preceding clip.
    control.observe(nullptr,1001,1001,result.received+.1,result.finished+.1,true);
    control.observe_automatic(result,result.finished+.1);assert(!control.tick(result.finished+.1).allowed);
    // Arrow structure survives brief shape dropout, but fresh color is mandatory.
    ai::SignalStructure structure;ai::SignalCandidate arrow=left;std::vector<ai::SignalCandidate> items;
    for(int i=0;i<3;++i){items={arrow};structure.update(items,200+i*.1);}
    auto missing=arrow;missing.direction=-1;missing.shape="UNKNOWN";missing.reason="PICTOGRAM_AMBIGUOUS";
    items={missing};structure.update(items,200.3);assert(items[0].direction==0&&items[0].reason=="TRACKED_ARROW_STRUCTURE_FRESH_COLOR");
    missing.color="UNKNOWN";items={missing};structure.update(items,200.4);assert(items[0].direction==-1);
    missing.color="GREEN";missing.track_id=1234;items={missing};structure.update(items,200.5);assert(items[0].direction==-1);
    missing.track_id=arrow.track_id;missing.shape="CIRCLE";items={missing};structure.update(items,200.5);assert(items[0].direction==-1);
    structure.reset();items={missing};structure.update(items,200.6);assert(items[0].direction==-1);
    { // Position ordering is a labelled prior, never a replacement for arrow permission.
        ai::SignalLayoutHints hints;auto a=circle,b=circle;a.track_id=40;b.track_id=41;
        a.box={300,100,330,180,.9f,7};b.box={500,100,530,180,.9f,7};a.color="RED";b.color="GREEN";
        for(int i=0;i<3;++i)hints.update({b,a},300+i*.1);
        assert(hints.colors[0]=="RED"&&hints.colors[1]=="GREEN"&&hints.colors[2].empty());
        b.color="UNKNOWN";hints.update({a,b},300.3);assert(hints.colors[1].empty()); // never hold green color
        hints.update({a},300.4);assert(hints.colors[0].empty()); // no rank reassignment after a missing head
        b.color="GREEN";hints.update({a,b},300.5);hints.update({a,b},300.5);assert(hints.colors[1].empty());
        for(int i=0;i<3;++i)hints.update({a,b},301+i*.1);assert(hints.colors[1]=="GREEN");
        b.direction=0;hints.update({a,b},301.3);assert(hints.colors[1].empty()); // arrow contradiction wins
        b.direction=-1;auto c=b;c.track_id=42;c.box={700,100,730,180,.9f,7};
        for(int i=0;i<3;++i)hints.update({a,b,c},302+i*.1);assert(hints.colors[2]=="GREEN");
        c.box.y1=std::numeric_limits<float>::quiet_NaN();hints.update({a,b,c},302.3);assert(hints.colors[2].empty());
        control.reset_scene(cfg,303);
        for(int i=0;i<3;++i){result.received=304+i*.1;result.finished=result.received+.02;result.frame_id=result.source=i+1;result.signal_candidates={a,b};
            control.observe_automatic(result,result.finished);control.observe_objects(result,result.finished);}
        auto e=control.explanation(304.23);assert(e.signal_hint[1]=="推测绿灯"&&e.signal[1]!="信号允许");
        assert(std::string(control.permission(Direction::Straight,304.23))=="WAIT");
        assert(control.explanation(305).signal_hint[1].empty());
        control.key('c',304.24);assert(control.explanation(304.24).signal_hint[1].empty());
    }
    { // Practical fallback grants a labelled signal advisory, not vehicle control.
        { // Main-row grouping excludes smaller nearby heads; horizontal housings are supported.
            ai::SignalAssociation roles;auto a=circle,b=circle,c=circle,small=circle;
            a.track_id=201;a.box={300,100,330,180,.9f,7};a.color="RED";
            b.track_id=202;b.box={500,100,530,180,.9f,7};b.color="GREEN";
            c.track_id=203;c.box={700,100,730,180,.9f,7};c.color="RED";
            small.track_id=204;small.box={850,110,870,140,.9f,7};
            std::vector<ai::SignalCandidate> heads;
            for(int i=0;i<3;++i){heads={small,c,b,a};roles.update(heads,390+i*.1);}
            assert(heads[1].direction==2&&heads[2].direction==1&&heads[3].direction==0&&heads[0].direction==-1);
            heads={c};roles.update(heads,390.3);assert(heads[0].direction==2);
            roles.reset();a.box={500,100,620,140,.9f,7};
            for(int i=0;i<3;++i){heads={a};roles.update(heads,391+i*.1);}assert(heads[0].direction==1);
            roles.reset();a.box.x1=std::numeric_limits<float>::quiet_NaN();
            for(int i=0;i<3;++i){heads={a};roles.update(heads,392+i*.1);}assert(heads[0].direction==-1);
        }
        auto directory=root/"practical";assert(fs::create_directory(directory));
        Config practical;practical.automatic=true;practical.settings.stop_hold_ms=1500;
        Control app(practical,directory.string());ai::Result frame;frame.finished=1;
        auto feed=[&](double stamp,std::vector<ai::SignalCandidate> lamps){
            frame.received=stamp;frame.finished=stamp+.01;frame.frame_id++;frame.source++;
            frame.signal_candidates=std::move(lamps);app.observe_automatic(frame,frame.finished);app.observe_objects(frame,frame.finished);app.tick(frame.finished);
        };
        auto one=circle;one.track_id=301;one.box={600,100,630,180,.9f,7};one.color="GREEN";
        for(int i=0;i<5;++i)feed(400+i*.1,{one});
        assert(std::string(app.permission(Direction::Straight,400.42))=="ALLOW");
        assert(std::string(app.permission(Direction::Left,400.42))=="WAIT"&&std::string(app.permission(Direction::Right,400.42))=="WAIT");
        assert(app.explanation(400.42).signal_detail[1]=="按灯组布局判断·仅信号条件");
        one.track_id=302;one.box.x1+=1;one.box.x2+=1;feed(400.45,{one});
        assert(std::string(app.permission(Direction::Straight,400.47))=="ALLOW"); // unique physical housing survives tracker-ID handover
        feed(400.5,{});assert(std::string(app.permission(Direction::Straight,400.52))=="ALLOW");
        assert(std::string(app.permission(Direction::Straight,400.91))=="WAIT"); // original evidence expires; gaps cannot renew it
        one.color="RED";for(int i=0;i<4;++i)feed(401+i*.1,{one});
        assert(std::string(app.permission(Direction::Straight,401.32))=="STOP");
        feed(401.4,{});assert(std::string(app.permission(Direction::Straight,402))=="STOP");
        assert(std::string(app.permission(Direction::Straight,403))=="WAIT"); // finite stop hold
        app.reset_scene(practical,404);auto a=one,b=one,c=one;
        a.track_id=401;a.box={300,100,330,180,.9f,7};a.color="RED";
        b.track_id=402;b.box={500,100,530,180,.9f,7};b.color="GREEN";
        c.track_id=403;c.box={700,100,730,180,.9f,7};c.color="RED";
        for(int i=0;i<5;++i)feed(405+i*.1,{c,a,b});
        assert(std::string(app.permission(Direction::Left,405.42))=="STOP"&&std::string(app.permission(Direction::Straight,405.42))=="ALLOW");
        // After neighbours disappear the original left head must not become straight.
        a.color="GREEN";for(int i=0;i<4;++i)feed(405.5+i*.1,{a});
        assert(std::string(app.permission(Direction::Left,405.82))=="ALLOW");
        assert(std::string(app.permission(Direction::Straight,405.82))=="WAIT");
        app.reset_scene(practical,407);
        for(int i=0;i<5;++i)feed(408+i*.1,{a,b});
        assert(std::string(app.permission(Direction::Left,408.42))=="ALLOW"&&std::string(app.permission(Direction::Straight,408.42))=="ALLOW");
        assert(std::string(app.permission(Direction::Right,408.42))=="WAIT");
        auto uncertain=b;uncertain.color="UNKNOWN";c.color="UNKNOWN";
        feed(408.45,{a,uncertain,b,c}); // new neighbour + duplicate unknown must not erase the two known green roles
        assert(std::string(app.permission(Direction::Left,408.47))=="ALLOW"&&std::string(app.permission(Direction::Straight,408.47))=="ALLOW");
        // Fresh arrow contradicting a stored layout takes precedence immediately.
        b.direction=2;b.shape="RIGHT";b.color="RED";
        for(int i=0;i<4;++i)feed(408.5+i*.1,{a,b});
        assert(std::string(app.permission(Direction::Right,408.82))=="STOP");
        assert(std::string(app.permission(Direction::Straight,408.82))!="ALLOW");
        app.key('c',408.83);app.reset_scene(practical,409);
        // An edge lamp/down signal, duplicate ID, stale frame, or color conflict cannot grant a fallback.
        one.color="GREEN";one.box={10,100,40,180,.9f,7};
        for(int i=0;i<5;++i)feed(410+i*.1,{one});assert(std::string(app.permission(Direction::Straight,410.42))=="WAIT");
        one.box={600,100,630,180,.9f,7};one.shape="DOWN";
        for(int i=0;i<5;++i)feed(411+i*.1,{one});assert(std::string(app.permission(Direction::Straight,411.42))=="WAIT");
        one.shape="CIRCLE";auto duplicate=one;duplicate.color="RED";
        for(int i=0;i<5;++i)feed(412+i*.1,{one,duplicate});assert(std::string(app.permission(Direction::Straight,412.42))=="WAIT");
        frame.received=413;frame.finished=413.7;frame.signal_candidates={one};app.observe_automatic(frame,413.7);
        assert(std::string(app.permission(Direction::Straight,413.7))=="WAIT");
        app.reset_scene(practical,414);
        for(int i=0;i<5;++i)feed(415+i*.1,{one});
        assert(std::string(app.permission(Direction::Straight,415.42))=="ALLOW");
        one.color="UNKNOWN";one.reason="INSUFFICIENT_COLOR_PIXELS";feed(415.5,{one});
        assert(std::string(app.permission(Direction::Straight,415.52))=="ALLOW");
        one.reason="MULTIPLE_COLORS";feed(415.55,{one});assert(std::string(app.permission(Direction::Straight,415.57))=="WAIT");
        // Unlike UNKNOWN + GREEN duplicates, two explicit contradictory colors remain blocked.
        one.color="GREEN";for(int i=0;i<4;++i)feed(415.6+i*.1,{one});
        auto opposed=one;opposed.color="RED";feed(415.95,{one,opposed});
        assert(std::string(app.permission(Direction::Straight,415.97))!="ALLOW");
        app.reset_scene(practical,416);assert(std::string(app.permission(Direction::Straight,416))=="WAIT");
        // Partial dropout has the same bounded grace as a whole-group dropout.
        a.direction=-1;a.shape="CIRCLE";a.color="RED";b.direction=-1;b.shape="CIRCLE";b.color="GREEN";
        for(int i=0;i<5;++i)feed(500+i*.1,{a,b});
        feed(500.5,{a});assert(std::string(app.permission(Direction::Straight,500.52))=="ALLOW");
        feed(500.65,{a});assert(std::string(app.permission(Direction::Straight,500.67))=="ALLOW");
        feed(500.8,{a});assert(std::string(app.permission(Direction::Straight,500.82))=="WAIT");
        // A fresh red/direction contradiction at the same housing cannot inherit old GREEN.
        app.reset_scene(practical,501);for(int i=0;i<5;++i)feed(502+i*.1,{a,b});
        b.direction=0;b.shape="LEFT";b.color="RED";feed(502.5,{a,b});
        assert(std::string(app.permission(Direction::Straight,502.52))!="ALLOW");
        app.reset_scene(practical,503);
        // Weak colorless or untracked boxes cannot keep the UI in a signal episode forever.
        one.track_id=701;one.direction=1;one.shape="STRAIGHT";one.color="RED";one.reason="COLOR_COMPONENT_CANDIDATE";
        for(int i=0;i<5;++i)feed(504+i*.1,{one});
        auto weak=one;weak.color="UNKNOWN";weak.shape="UNKNOWN";weak.direction=-1;weak.reason="INSUFFICIENT_COLOR_PIXELS";
        for(int i=0;i<13;++i)feed(504.5+i*.1,{weak});
        assert(std::string(app.permission(Direction::Straight,505.72))=="STOP"); // Existing 1.5s STOP grace survives.
        for(int i=0;i<6;++i)feed(505.8+i*.1,{weak});
        auto explanation=app.explanation(506.32);
        assert(explanation.signal[1]=="当前未见灯"&&explanation.action=="保持观察 / KEEP");
        assert(std::string(app.permission(Direction::Straight,506.32))=="WAIT");
        one.color="GREEN";one.track_id=0;
        for(int i=0;i<20;++i)feed(506.4+i*.1,{one});
        assert(app.explanation(508.32).signal[1]=="当前未见灯");
        one.track_id=702;for(int i=0;i<4;++i)feed(509+i*.1,{one});
        assert(std::string(app.permission(Direction::Straight,509.32))=="ALLOW");
        assert(app.explanation(509.32).signal[0]=="未关联此方向");
        // Re-reading a frozen result or having no completed inference cannot count as fresh absence.
        frame.signal_candidates.clear();frame.received=frame.finished=509.4;app.observe_automatic(frame,509.42);
        for(int i=0;i<50;++i)app.observe_automatic(frame,509.42);
        assert(app.explanation(512).signal[1]=="前视证据不足");
        app.reset_scene(practical,513);one.direction=1;one.color="GREEN";
        for(int i=0;i<5;++i)feed(514+i*.1,{one});
        feed(514.5,{});assert(std::string(app.permission(Direction::Straight,514.52))=="ALLOW");
        auto contradiction=one;contradiction.direction=0;contradiction.shape="LEFT";contradiction.color="RED";
        feed(514.55,{contradiction});assert(std::string(app.permission(Direction::Straight,514.57))!="ALLOW");
        app.reset_scene(practical,515);one.color="RED";one.direction=1;one.shape="STRAIGHT";one.box={500,500,580,580,.9f,7};
        for(int i=0;i<4;++i)feed(516+i*.1,{one});
        assert(std::string(app.permission(Direction::Straight,516.32))=="STOP");
        for(int i=0;i<20;++i)feed(516.4+i*.1,{});
        assert(app.explanation(518.32).signal[1]=="当前未见灯");
        std::ifstream scenes(directory/"signal_scene.csv");std::string history((std::istreambuf_iterator<char>(scenes)),{});
        assert(history.find("EXIT_NO_VALID_SIGNAL")!=std::string::npos);
    }
    std::vector<uint8_t> guarded(1280*720*3/2+64,77);control.draw(guarded.data()+32,0,result.finished+.1);
    for(int i=0;i<32;++i)assert(guarded[i]==77&&guarded[guarded.size()-1-i]==77);
    std::cout<<"PASS dynamic detector ROI, direction separation, missing/circle/conflict/duplicate/manual override/loop rejection and overlay bounds\n";
}
