#pragma once
#include <array>
namespace camera_roles {
constexpr unsigned Front=0,Left=2,Right=1,Rear=3;
constexpr std::array<unsigned,4> Order{{Front,Left,Right,Rear}};
constexpr const char* name(unsigned channel){return channel==Front?"前方":channel==Left?"左侧":channel==Right?"右侧":"后方";}
}
