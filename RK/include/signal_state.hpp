#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace signal_demo {
enum class Color { Unknown, Red, Yellow, Green };
enum class Direction { Left=0, Straight=1, Right=2 };
inline const char* name(Color c){switch(c){case Color::Red:return "RED";case Color::Yellow:return "YELLOW";case Color::Green:return "GREEN";default:return "UNKNOWN";}}
inline const char* name(Direction d){return d==Direction::Left?"LEFT":d==Direction::Right?"RIGHT":"STRAIGHT";}
inline Color color(const std::string& s){return s=="RED"?Color::Red:s=="GREEN"?Color::Green:s=="YELLOW"?Color::Yellow:Color::Unknown;}
struct Settings {
    double confirm_red_ms=100,confirm_green_ms=200,confirm_yellow_ms=100;
    double max_gap_ms=200,max_age_ms=300;
    unsigned minimum_samples=3;
    double stop_hold_ms=0; // Opt-in: hold only a confirmed restrictive decision, never GREEN permission.
};
struct Evidence {
    Color raw=Color::Unknown,stable=Color::Unknown;
    double first=-1,last=-1;
    uint32_t source=0;
    unsigned samples=0;
};
struct Decision {
    Direction direction=Direction::Straight;
    Color light=Color::Unknown;
    bool requested=false,allowed=false,alarm=false;
    double age_ms=-1;
    const char* state="UNKNOWN";
    bool stop_held=false;
    Color held_light=Color::Unknown; // Last confirmed restrictive color; presentation only.
    double stop_age_ms=-1;
    const char* instruction="WAIT"; // Simulation instruction, separate from raw light/diagnostic state.
};
// Simulation only. No speed, stop-line crossing, actuator or actual violation claim.
class State {
    Settings settings;
    std::array<Evidence,3> evidence{};
    struct StopMemory {double received=-1;Color light=Color::Unknown;};
    std::array<StopMemory,3> stops{};
    Direction direction=Direction::Straight;
    bool requested=false,alarm=false;
public:
    explicit State(Settings s={}):settings(s){
        if(s.minimum_samples<2 || s.confirm_red_ms<0 || s.confirm_green_ms<0 || s.confirm_yellow_ms<0 ||
           s.max_age_ms<=0 || s.max_gap_ms<=0 || s.max_gap_ms>s.max_age_ms ||
           !std::isfinite(s.stop_hold_ms)||s.stop_hold_ms<0||s.stop_hold_ms>2000)
            throw std::runtime_error("Invalid signal confirmation/age settings");
    }
    Direction selected() const{return direction;}
    const Evidence& get(Direction d) const{return evidence.at(unsigned(d));}
    void reset(Direction d){evidence.at(unsigned(d))={};stops.at(unsigned(d))={};}
    void reset_all(){for(auto& e:evidence)e={};for(auto& m:stops)m={};requested=false;alarm=false;}
    void select(Direction d){if(d!=direction){direction=d;requested=false;alarm=false;}}
    void stop(){requested=false;alarm=false;}
    void observe(Direction d,Color c,double received,uint32_t source,double now){
        auto& e=evidence.at(unsigned(d));
        if(!std::isfinite(received)||!std::isfinite(now)||received>now){reset(d);return;}
        if((now-received)*1000>settings.max_age_ms){e={};return;} // A late result cannot renew the old bounded stop grace.
        // Re-reading a held frame cannot satisfy temporal confirmation.
        if(e.last>=0 && received<=e.last)return;
        const bool gap=e.last<0 || (received-e.last)*1000>settings.max_gap_ms;
        if(gap || c!=e.raw){e={};e.raw=c;e.first=received;}
        e.last=received;e.source=source;
        if(c==Color::Unknown){e.samples=0;e.stable=Color::Unknown;return;}
        ++e.samples;
        double delay=c==Color::Green?settings.confirm_green_ms:c==Color::Red?settings.confirm_red_ms:settings.confirm_yellow_ms;
        if(e.samples>=settings.minimum_samples && (received-e.first)*1000+1e-7>=delay){
            e.stable=c;
            auto& memory=stops.at(unsigned(d));
            if(c==Color::Red||c==Color::Yellow){memory.received=received;memory.light=c;}
            else if(c==Color::Green)memory={};
        }
    }
    Decision decision(double now){
        const auto& e=evidence.at(unsigned(direction));
        double age=e.last<0?-1:(now-e.last)*1000;
        Color c=age>=0 && age<=settings.max_age_ms?e.stable:Color::Unknown;
        const auto& memory=stops.at(unsigned(direction));
        double stop_age=memory.received<0?-1:(now-memory.received)*1000;
        bool hold=c==Color::Unknown&&settings.stop_hold_ms>0&&stop_age>=0&&stop_age<=settings.stop_hold_ms;
        if(requested && c==Color::Red)alarm=true;
        Decision result{direction,c,requested,requested && c==Color::Green && !alarm,alarm,age,"UNKNOWN"};
        result.stop_held=hold;result.stop_age_ms=stop_age;
        if(hold)result.held_light=memory.light;
        if(alarm)result.state="ALARM_RED_REQUEST";
        else if(hold)result.state="STOP_HOLD";
        else if(c==Color::Unknown)result.state=requested?"BLOCKED_UNKNOWN":"UNKNOWN";
        else if(c==Color::Red)result.state="RED_WAIT";
        else if(c==Color::Yellow)result.state=requested?"BLOCKED_YELLOW":"YELLOW_WAIT";
        else result.state=requested?"SIM_GO_ALLOWED":"GREEN_READY";
        result.instruction=result.alarm||hold||c==Color::Red||c==Color::Yellow?"STOP":result.allowed?"SIM_GO":"WAIT";
        return result;
    }
    Decision signal_decision(Direction direction,double now)const{
        State copy=*this;copy.direction=direction;copy.requested=false;copy.alarm=false;
        auto result=copy.decision(now);
        result.allowed=result.light==Color::Green&&!result.stop_held;
        result.instruction=result.allowed?"ALLOW":result.instruction;
        return result; // Signal advisory only; does not request vehicle motion.
    }
    Decision go(double now){requested=true;return decision(now);}
};
}
