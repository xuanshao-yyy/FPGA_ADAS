#pragma once
#include <array>
namespace panorama_display {
// FPGA RC2 already feathered 16px seams. Divide at seam midpoints so the
// five display slots are disjoint; only the two equal-width side slots move.
struct Copy {int source_x,destination_x,width;};
constexpr std::array<Copy,2> SideCopies{{{1592,500,468},{500,1592,468}}};
constexpr int source_x(int output_x){
    return output_x>=500&&output_x<968?output_x+1092:
           output_x>=1592&&output_x<2060?output_x-1092:output_x;
}
}
