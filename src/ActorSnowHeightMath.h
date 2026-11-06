// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
namespace ActorSnowHeightMath {
    inline float Offset(float surfaceZ, float rootZ, bool snow) {
        return snow && std::isfinite(surfaceZ) && std::isfinite(rootZ) ? std::max(surfaceZ - rootZ, 0.0f) : 0.0f;
    }
}
