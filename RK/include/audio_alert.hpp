#pragma once
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <spawn.h>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
extern char **environ;
namespace audio_alert {
struct Gate {
    bool previous=false;double last=-100;
    bool update(bool active,double now){bool edge=active&&!previous;previous=active;
        if(!edge||now-last<2.)return false;
        last=now;return true;}
};
// 80ms + 80ms gap + 80ms, mono PCM16. Generated once, not in the frame loop.
inline void write_wav(const std::string& path){
    constexpr int rate=16000,count=3840;std::ofstream f(path,std::ios::binary);
    auto le=[&](uint32_t value,int bytes){for(int i=0;i<bytes;++i)f.put(char((value>>(8*i))&255));};
    f.write("RIFF",4);le(36+count*2,4);f.write("WAVEfmt ",8);le(16,4);le(1,2);le(1,2);le(rate,4);le(rate*2,4);le(2,2);le(16,2);f.write("data",4);le(count*2,4);
    for(int i=0;i<count;++i){int offset=i<1280?i:i>=2560?i-2560:-1;double envelope=offset<0?0:std::min(1.,std::min(offset/80.,(1279-offset)/80.));
        int16_t sample=int16_t(6000*envelope*std::sin(2*3.141592653589793*1200*i/rate));le(uint16_t(sample),2);}
    f.close();if(!f)throw std::runtime_error("Cannot write alert WAV");
}
class Cue {
    std::string wav,log,backend,backend_override;bool enabled=false,stopping=false,pending=false;Gate gate;
    std::mutex mutex;std::condition_variable wake;std::thread worker;std::atomic<int> state{0};
    void record(const char* event,int code=0){std::ofstream f(log,std::ios::app);f<<std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count()<<','<<event<<','<<code<<','<<backend<<'\n';}
    void run(){
        for(;;){
            {std::unique_lock<std::mutex> lock(mutex);wake.wait(lock,[&]{return stopping||pending;});if(stopping)return;pending=false;}
            std::vector<std::string> words;
            if(backend.find("gst-launch")!=std::string::npos)words={backend,"-q","filesrc","location="+wav,"!","wavparse","!","audioconvert","!","audioresample","!","autoaudiosink"};
            else words={backend,"-q",wav};
            std::vector<char*> argv;for(auto& word:words)argv.push_back(word.data());argv.push_back(nullptr);
            posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
            posix_spawn_file_actions_addopen(&actions,STDOUT_FILENO,"/dev/null",O_WRONLY,0);
            posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,(log+".backend").c_str(),O_WRONLY|O_CREAT|O_APPEND,0600);
            pid_t child=-1;int error=posix_spawn(&child,backend.c_str(),&actions,nullptr,argv.data(),environ);posix_spawn_file_actions_destroy(&actions);
            if(error){state=3;record("SPAWN_FAILED",error);continue;}
            auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);int status=0;bool reaped=false;
            for(;;){if(waitpid(child,&status,WNOHANG)==child){reaped=true;break;}
                std::unique_lock<std::mutex> lock(mutex);
                if(stopping||std::chrono::steady_clock::now()>=until)break;
                wake.wait_for(lock,std::chrono::milliseconds(25));}
            if(!reaped){kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}state=3;record("PLAYER_TIMEOUT_OR_STOP");}
            else {bool ok=WIFEXITED(status)&&WEXITSTATUS(status)==0;state=ok?1:3;record(ok?"PLAYER_EXIT_OK_NOT_AUDIBILITY_PROOF":"PLAYER_FAILED",status);}
        }
    }
    void initialize(){
        try{write_wav(wav);
            if(!backend_override.empty())backend=backend_override;
            else for(const char* p:{"/usr/bin/aplay","/bin/aplay","/usr/bin/gst-launch-1.0"})if(access(p,X_OK)==0){backend=p;break;}
            if(backend.empty()){state=3;record("NO_PLAYER");return;}
            state=1;worker=std::thread([this]{run();});
        }catch(const std::exception&){state=3;record("AUDIO_INIT_FAILED");}
    }
public:
    Cue(const std::string& output,bool on,const std::string& override_backend=""):wav(output+"/violation_beep.wav"),log(output+"/audio_alert.csv"),backend_override(override_backend),enabled(on){
        if(enabled)initialize();
    }
    ~Cue(){{std::lock_guard<std::mutex> lock(mutex);stopping=true;pending=false;}wake.notify_all();if(worker.joinable())worker.join();}
    Cue(const Cue&)=delete;Cue& operator=(const Cue&)=delete;
    void observe(bool active,double now){if(!enabled)return;std::lock_guard<std::mutex> lock(mutex);
        if(!active)pending=false;
        if(gate.update(active,now)&&worker.joinable()){pending=true;wake.notify_one();}}
    void toggle(){
        {std::lock_guard<std::mutex> lock(mutex);enabled=!enabled;pending=false;gate.previous=false;}
        if(enabled&&!worker.joinable())initialize();
    }
    int ui_state()const{return !enabled?0:state.load()==3?2:1;} // off, ready, unavailable
};
}
