// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ActorCollisionCachePolicy
{
    inline constexpr double AuditSeconds = 1.0;
    inline double Advance(double clock, float dt)
    {
        return clock + (std::isfinite(dt) ? std::clamp(dt, 0.0f, 0.1f) : 0.0f);
    }
    inline bool Rebuild(bool present, bool sameHandle, bool sameRoot,
        bool sameRagdoll, bool attached, double clock, double nextAudit)
    {
        return !present || !sameHandle || !sameRoot || !sameRagdoll ||
            !attached || clock >= nextAudit;
    }
    template <class Object>
    bool Attached(Object* owner, Object* root)
    {
        for (unsigned i = 0; owner && i < 256; ++i, owner = owner->parent) {
            if (owner == root) { return true; }
        }
        return false;
    }
}
