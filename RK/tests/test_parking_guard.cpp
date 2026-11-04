#include "parking_guard.hpp"
#include <cassert>
#include <iostream>
ai::Box box(float area){return {.35f,.9f-area/.3f,.65f,.9f,.8f,0};}
int main(){
    // The decoder already maps 640x384 (12px pad) to a 1280x720 source plane.
    // A model-space box [160,102,480,282] is therefore [320,180,960,540].
    auto center=parking::normalized_detector_box({320,180,960,540,.8f,0});
    assert(center.x1==.25f&&center.y1==.25f&&center.x2==.75f&&center.y2==.75f);
    auto lower=parking::normalized_detector_box({960,540,1280,720,.8f,0});
    assert(lower.x1==.75f&&lower.y1==.75f&&lower.x2==1&&lower.y2==1);
    parking::Engine geometry;auto retained=geometry.update(0,{lower},1,true);
    assert(retained.size()==1&&retained[0].area>.06&&retained[0].area<.063);
    // Integrate real DFL decoding, padding inversion, normalization and tile position.
    std::array<std::vector<float>,9> values;std::array<ai::Tensor,9> tensors;
    for(int k=0;k<9;++k){int h=48>>(k/3),w=80>>(k/3),c=k%3==0?64:k%3==1?8:1;values[k].resize(c*h*w);tensors[k]={values[k].data(),c,h,w,false};}
    int x=60,y=36;values[1][y*80+x]=.9f;
    for(int side=0;side<4;++side)for(int k=0;k<16;++k)values[0][((side*16+k)*48+y)*80+x]=k==2?100.f:-100.f;
    auto decoded=ai::decode(tensors,.25f,.45f,640,384);assert(decoded.size()==1);
    auto normal=parking::normalized_detector_box(decoded[0]);
    assert(std::abs(normal.x1*640-468)<.001&&std::abs(normal.y1*360-264)<.001);
    assert(std::abs(normal.x2*640-500)<.001&&std::abs(normal.y2*360-296)<.001);
    parking::Rules r;assert(r.valid()&&r.sweep_hz==0);r.sweep_hz=-1;assert(!r.valid());r={};r.person_red=.01;assert(!r.valid());r={};
    parking::Engine e(r);auto a=e.update(0,{box(.01f)},1,true);assert(a.size()==1&&a[0].level==0);
    a=e.update(0,{box(.04f)},1.5,true);assert(a[0].level==1&&a[0].event==0);
    a=e.update(0,{box(.09f)},2,true);assert(a[0].level==1&&a[0].event==0);
    a=e.update(0,{box(.09f)},2.5,true);assert(a[0].level==2&&a[0].event>0);auto event=a[0].event;
    a=e.update(0,{box(.09f)},3,true);assert(a[0].event==event);
    a=e.update(0,{box(.04f)},3.5,true);assert(a[0].level==2);
    a=e.update(0,{box(.04f)},4.3,true);assert(a[0].level==1);
    a=e.update(0,{box(.09f)},4.8,true);assert(a[0].level==1); // expired hold cannot stick red
    a=e.update(0,{box(.09f)},5.3,true);assert(a[0].level==2&&a[0].event==event);
    a=e.update(0,{},5.8,true);assert(a.size()==1&&a[0].held);
    a=e.update(0,{},6.2,true);assert(a.empty());
    assert(e.update(1,{box(.2f)},7,false).empty());
    auto light=box(.2f);light.category=7;assert(e.update(2,{light},8,true).empty());
    auto invalid=box(.2f);invalid.score=std::nanf("");assert(e.update(2,{invalid},9,true).empty());
    auto outside=box(.09f);outside.y1=0;outside.y2=.3f;assert(e.update(3,{outside},10,true)[0].level==0);
    e.configure(r);assert(e.update(0,{box(.09f)},20,true)[0].level==1);
    std::cout<<"PASS parking: image area and foot thresholds, confirmation, hold expiry, missed detection, stale coverage, class filter and rule reset\n";
}
