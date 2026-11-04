#include "surround_view.hpp"
#include <cassert>
#include <iostream>
#include <set>
#include <vector>
int main(){
    using namespace surround_ui;
    assert((Layout{0}.channels()==std::array<unsigned,4>{0,2,1,3}));
    for(unsigned front=0;front<4;++front){Layout layout{front};auto order=layout.channels();
        assert(order[0]==front&&std::set<unsigned>(order.begin(),order.end()).size()==4);
        for(unsigned role=0;role<4;++role){auto r=Layout::tiles[role];
            assert((r.x|r.y|r.w|r.h)%2==0&&r.w*9==r.h*16);
            assert(r.x>=2&&r.y>=16&&r.x+r.w<1280&&r.y+r.h<720);
            for(unsigned other=0;other<role;++other){auto a=Layout::tiles[other];
                assert(r.x+r.w<=a.x||a.x+a.w<=r.x||r.y+r.h<=a.y||a.y+a.h<=r.y);}
            unsigned ch=99;int x=0,y=0;auto s=layout.source(role);
            assert(layout.canonical(r.x+r.w/2,r.y+r.h/2,ch,x,y));
            assert(ch==order[role]&&x==s.x+s.w/2&&y==s.y+s.h/2);
            assert(layout.canonical(r.x,r.y,ch,x,y)&&x==s.x&&y==s.y);
            assert(layout.canonical(r.x+r.w-1,r.y+r.h-1,ch,x,y)&&x<s.x+s.w&&y<s.y+s.h);
        }
        // Matte, car sprite and video labels never bind a manual light ROI.
        for(const auto p:{std::array<int,2>{640,420},{0,0},{384,8},{1279,719}}){unsigned ch=0;int x=0,y=0;
            assert(!layout.canonical(p[0],p[1],ch,x,y));}
        for(int y=0;y<720;y+=3)for(int x=0;x<1280;x+=3){unsigned ch=99;int cx=-1,cy=-1;
            if(layout.canonical(x,y,ch,cx,cy)){
                assert(ch<4);auto sx=int(ch%2)*640+32,sy=int(ch/2)*360+32;
                assert(cx>=sx&&cx<sx+576&&cy>=sy&&cy<sy+324);}
        }
    }
    constexpr size_t bytes=1280*720*3/2;
    std::vector<uint8_t> guarded(bytes+64,77);initialize(guarded.data()+32);
    for(int i=0;i<32;++i)assert(guarded[i]==77&&guarded[guarded.size()-1-i]==77);
    // Decorative asset stays outside all live tiles and has visible tonal range.
    for(const auto& tile:Layout::tiles)assert(tile.x+tile.w<=600||tile.x>=680||tile.y+tile.h<=364||tile.y>=524);
    auto limits=std::minmax_element(EgoCarNv12.begin(),EgoCarNv12.begin()+96*192);
    assert(*limits.first<60&&*limits.second>180);
    std::cout<<"PASS surround: 16:9/even disjoint tiles, four front permutations, source corners/centres, matte rejection and NV12 asset bounds. Not RGA/board acceptance.\n";
}
