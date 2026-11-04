#pragma once
#include <array>
namespace cockpit {
// Solid native surfaces only: no animated/alpha-composited UI layers.
inline constexpr std::array<const char*,14> Palette{{
    "#101b26","#172632","#e9f0f3","#8da2af","#65d5b5","#eebd72","#fa8793",
    "#30232d","#2b4353","#173a34","#362f25","#8ac8ee","#452632","#493a26"}};
}
