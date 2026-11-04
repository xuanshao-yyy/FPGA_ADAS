#pragma once
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <chrono>

namespace signal_demo {
// Separate X11 toolbar: its pixels never cover or enter the camera image.
struct MouseLayout {
    static constexpr int bar_height=84;
    static constexpr std::array<char,12> keys{{'a','w','d','j','m','l','c',' ','p','n','v','q'}};
    static constexpr std::array<const char*,12> labels{{"路口左转","直行方向","路口右转","左变道","右变道","取消变道","重新核查","恢复通行","暂停画面","下一素材","前视实时","退出"}};
    int width=1280,height=804,vx=0,vy=0,vw=1280,vh=720,px=0,py=0,pw=0,ph=0;
    void resize(int w,int h){
        width=std::max(1,w);height=std::max(bar_height+1,h);
        int available=height-bar_height;
        // Three native status cards above the video; never draw prose over CH0.
        px=py=0;pw=width;ph=std::clamp(available/4,160,208);
        ph=std::min(ph,std::max(1,available-32));
        int video_height=std::max(1,available-ph-8);
        vw=std::min(width,std::max(1,video_height*16/9));vh=std::max(1,vw*9/16);
        vx=(width-vw)/2;vy=ph+8+(video_height-vh)/2;
    }
    int left(int i) const{return 8+i*(width-10)/int(keys.size());}
    int right(int i) const{return left(i+1)-6;}
    int hit(int x,int y) const{
        if(y<height-bar_height+8||y>=height-bar_height+44)return -1;
        for(int i=0;i<int(keys.size());++i)if(x>=left(i)&&x<right(i))return i;
        return -1;
    }
};
struct MouseGesture {
    int down=-1;
    void cancel(){down=-1;}
    void press(int hit){down=hit;}
    char release(int hit){int old=down;cancel();return old>=0&&old==hit?MouseLayout::keys[old]:0;}
};
}
