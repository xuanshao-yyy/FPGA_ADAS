#include "visual.hpp"
#include <iostream>
#include <limits>
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
    std::vector<float> scores(2*640*640,.1f);
    auto at=[&](unsigned c,unsigned y,unsigned x)->float&{return scores[(c*640+y)*640+x];};
    for(unsigned y=0;y<640;++y)for(unsigned x=0;x<640;++x){at(0,y,x)=.8f;
        if(y<140 || y>=500){at(0,y,x)=.01f;at(1,y,x)=.99f;}
        else if(x>=100 && x<112){at(0,y,x)=.1f;at(1,y,x)=.9f;}}
    auto mask=ai::decode_lane({scores.data(),2,640,640,false},.5f);
    require(mask.pixels>3000 && mask.pixels<4000,"Expected thin vertical stripe, excluding padding");
    require(mask.data[10*576+95] && !mask.data[10*576+10],"Letterbox crop or pixel mapping failed");
    std::vector<float> nhwc(scores.size());
    for(unsigned y=0;y<640;++y)for(unsigned x=0;x<640;++x)for(unsigned c=0;c<2;++c)nhwc[(y*640+x)*2+c]=at(c,y,x);
    require(ai::decode_lane({nhwc.data(),2,640,640,true},.5f).data==mask.data,"NCHW/NHWC mismatch");
    require(ai::decode_lane({scores.data(),2,640,640,false},.95f).pixels==0,"Confidence threshold ignored");
    at(1,150,105)=std::numeric_limits<float>::quiet_NaN();bool rejected=false;
    try{ai::decode_lane({scores.data(),2,640,640,false},.5f);}catch(const std::exception&){rejected=true;}
    require(rejected,"Non-finite score accepted");
    constexpr size_t bytes=1280*720*3/2,guard=64;
    std::vector<uint8_t> buffer(bytes+2*guard,0xA5);auto image=buffer.data()+guard;
    std::fill(image,image+bytes,16);mask.received=qv::now_seconds();mask.finished=mask.received;mask.frame_id=10;
    require(ui::result_in_frame_order(mask,12) && !ui::result_in_frame_order(mask,9),"Lane frame order check failed");
    for(unsigned channel=0;channel<4;++channel)ui::lane_overlay(image,channel,true,&mask,2,750);
    for(unsigned channel=0;channel<4;++channel){int x=channel%2*640+32+95,y=channel/2*360+32+100;
        require(image[y*1280+x]>16,"Fresh lane candidate was not tinted");}
    for(size_t k=0;k<guard;++k)require(buffer[k]==0xA5 && buffer[guard+bytes+k]==0xA5,"Lane overlay exceeded NV12 bounds");
    for(unsigned y=0;y<720;++y)require(image[y*1280+639]==16 && image[y*1280+640]==16,"Lane overlay overwrote gutter");
    std::fill(image,image+bytes,16);mask.received=qv::now_seconds()-2;
    ui::lane_overlay(image,0,true,&mask,2,750);require(image[132*1280+127]==16,"Stale lane mask was rendered");
    mask.received=qv::now_seconds();mask.geometry.valid=true;mask.geometry.confirmed=true;
    mask.geometry.left={true,-.7f,370,.8f};mask.geometry.right={true,.7f,206,.8f};
    for(unsigned channel=0;channel<4;++channel)ui::lane_overlay(image,channel,true,&mask,2,750);
    for(size_t k=0;k<guard;++k)require(buffer[k]==0xA5 && buffer[guard+bytes+k]==0xA5,"Lane boundary overlay exceeded bounds");
    for(unsigned y=0;y<720;++y)require(image[y*1280+639]==16 && image[y*1280+640]==16,"Lane boundary overlay overwrote gutter");
    { // Compare production dense and cached overlays over every mixed 2x2 class pattern.
        auto sample=std::make_shared<ai::LaneMask>();sample->received=qv::now_seconds();sample->finished=sample->received;
        sample->four_class=true;sample->data.resize(576*324);
        for(size_t p=0;p<256;++p)for(unsigned k=0;k<4;++k){size_t x=(p%64)*2,y=(p/64)*2;
            sample->data[(y+k/2)*576+x+k%2]=uint8_t((p>>(k*2))&3);}
        ui::LaneOverlayCache cache;cache.bind(sample);cache.bind(sample);
        require(cache.builds()==1,"Immutable mask was unnecessarily rebuilt");
        std::vector<uint8_t> dense(bytes+2*guard),sparse;uint32_t seed=17;
        for(auto& v:dense){seed=seed*1664525u+1013904223u;v=uint8_t(seed>>24);}sparse=dense;
        for(bool four:{true,false}){sample->four_class=four; // Test fixture only; published production masks are immutable.
            for(unsigned ch=0;ch<4;++ch){
                dense=sparse;
                ui::lane_overlay(dense.data()+guard,ch,true,sample.get(),1,100000,false,false);
                ui::lane_overlay(sparse.data()+guard,ch,true,sample.get(),1,100000,false,false,&cache);
                require(dense==sparse,"Sparse overlay differs from dense NV12 reference");
            }}
        auto replacement=std::make_shared<ai::LaneMask>(*sample);std::fill(replacement->data.begin(),replacement->data.end(),0);
        cache.bind(replacement);require(cache.builds()==2&&cache.active_blocks()==0,"Changed mask retained old sparse blocks");
        dense=sparse;ui::lane_overlay(dense.data()+guard,0,true,replacement.get(),1,100000,false,false);
        ui::lane_overlay(sparse.data()+guard,0,true,replacement.get(),1,100000,false,false,&cache);require(dense==sparse,"Empty mask retained tint");
        cache.bind(nullptr);require(!cache.matches(replacement.get()),"Null mask retained cache lease");
        replacement->data.assign(576*324,3);cache.bind(replacement);require(!cache.matches(replacement.get()),"Dense mask should use dense fallback");
        dense=sparse;ui::lane_overlay(dense.data()+guard,3,true,replacement.get(),1,100000,false,false);
        ui::lane_overlay(sparse.data()+guard,3,true,replacement.get(),1,100000,false,false,&cache);require(dense==sparse,"Dense fallback differs");
        replacement->received=qv::now_seconds()-2;dense=sparse;
        ui::lane_overlay(dense.data()+guard,0,true,replacement.get(),1,750,false,false);
        ui::lane_overlay(sparse.data()+guard,0,true,replacement.get(),1,750,false,false,&cache);require(dense==sparse,"Stale sparse mask was rendered");
    }
    std::cout<<"PASS: crop140, bilinear mapping, sigmoid threshold, tensor layouts, NaN rejection, NV12 bounds/gutters, stale/order checks, exact sparse overlay parity\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
