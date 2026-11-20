// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
namespace SnowMarkDepth {
inline float Resolve(float ordinary,float snowThickness,float trenchScale,float sink=1.0f) {
 return std::max(ordinary,std::max(snowThickness,0.0f)*std::clamp(trenchScale,0.0f,4.0f)*std::clamp(sink,0.0f,1.0f));
}
}
