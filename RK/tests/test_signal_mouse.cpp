#include "signal_mouse.hpp"
#include "signal_state.hpp"
#include <cassert>
#include <iostream>
int main(){
    using namespace signal_demo;
    MouseLayout layout;MouseGesture gesture;
    for(const auto& size:{std::array<int,2>{1280,804},{640,444},{1100,900},{1600,600}}){
        layout.resize(size[0],size[1]);
        assert(layout.vx>=0&&layout.vy>=0&&layout.vx+layout.vw<=layout.width);
        assert(layout.vy+layout.vh<=layout.height-MouseLayout::bar_height);
        assert(layout.px>=0&&layout.py>=0&&layout.px+layout.pw<=layout.width&&layout.py+layout.ph<=layout.height-MouseLayout::bar_height);
        assert(layout.py==0&&layout.pw==layout.width&&layout.py+layout.ph<=layout.vy);
        assert(layout.vx+layout.vw<=layout.px||layout.vy+layout.vh<=layout.py||layout.py+layout.ph<=layout.vy);
        for(int i=0;i<int(MouseLayout::keys.size());++i){int x=(layout.left(i)+layout.right(i))/2,y=layout.height-60;
            assert(layout.hit(x,y)==i);gesture.press(layout.hit(x,y));
            assert(gesture.release(layout.hit(x,y))==MouseLayout::keys[i]);
            assert(gesture.release(layout.hit(x,y))==0); // no double action or held-button repeat
            gesture.press(i);assert(gesture.release((i+1)%MouseLayout::keys.size())==0); // moved into another button
            gesture.press(i);assert(gesture.release(-1)==0); // release outside
            gesture.press(i);gesture.cancel();assert(gesture.release(i)==0); // lost focus
            assert(layout.hit(layout.right(i),y)==-1); // gutter cannot click
        }
        assert(layout.hit(0,0)==-1);
        assert(layout.hit(layout.width-1,layout.height-1)==-1);
        assert(layout.hit(100,layout.height-MouseLayout::bar_height+7)==-1);
    }
    gesture.press(3);assert(gesture.release(3)=='j');gesture.press(4);assert(gesture.release(4)=='m');
    std::cout<<"PASS mouse hitboxes/aspect layout, release-only actions, gutters, drag-off, focus cancel and explicit independent lane requests\n";
}
