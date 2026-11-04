#include "surround_view.hpp"
#include <cassert>
#include <vector>
int main(){
    using namespace surround_ui;
    for(unsigned front=0;front<4;++front)for(bool mode:{false,true}){
        Layout layout{front,mode};auto rects=layout.destinations();int area=0;
        for(unsigned i=0;i<4;++i){auto t=rects[i];area+=t.w*t.h;
            assert((t.x|t.y|t.w|t.h)%2==0&&t.w*9==t.h*16);
            assert(t.x>=0&&t.y>=16&&t.x+t.w<=1280&&t.y+t.h<=720);
            unsigned ch=99;int x=-1,y=-1;auto s=layout.source(i);
            assert(layout.canonical(t.x+t.w/2,t.y+t.h/2,ch,x,y));
            assert(ch==layout.channels()[i]&&x==s.x+s.w/2&&y==s.y+s.h/2);
            for(unsigned j=0;j<i;++j){auto a=rects[j];assert(t.x+t.w<=a.x||a.x+a.w<=t.x||t.y+t.h<=a.y||a.y+a.h<=t.y);}
        }
        assert(area<=438912);
        std::vector<unsigned char> data(1280*720*3/2+64,91);
        initialize(data.data()+32,front,mode);
        for(unsigned i=0;i<32;++i)assert(data[i]==91&&data[data.size()-1-i]==91);
        if(!mode){std::vector<unsigned char> old(1280*720*3/2);initialize_original(old.data(),front);
            assert(std::equal(old.begin(),old.end(),data.begin()+32));}
    }
}
