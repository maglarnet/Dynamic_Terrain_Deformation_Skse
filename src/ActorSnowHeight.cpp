// SPDX-License-Identifier: GPL-3.0-only
#include "ActorSnowHeight.h"

#include <chrono>
#include <unordered_map>

#include "ActorSnowHeightMath.h"
#include "PCH.h"
#include "Settings.h"
#include "SnowSurface.h"
#include "SurfaceProfiles.h"

namespace ActorSnowHeight {
    namespace {
        struct Entry {
            RE::ActorHandle handle;
            bool seen{};
        };
        std::unordered_map<RE::FormID, Entry> tracked;
        const RE::BSFixedString& Variable() {
            static const RE::BSFixedString name("NMN_SnowHeight");
            return name;
        }
        bool reported{};
        void Zero(const Entry& entry) {
            if (auto actor = entry.handle.get(); actor && actor->Is3DLoaded()) {
                actor->SetGraphVariableFloat(Variable(), 0.0f);
            }
        }
    }
    Surface Resolve(RE::Actor* actor) {
        Surface result;
        auto* root = actor ? actor->Get3D(false) : nullptr;
        if (!root) return result;
        if (actor->IsOnMount()) {
            result.reason = "mounted";
            return result;
        }
        if (actor->IsInMidair()) {
            result.reason = "midair";
            return result;
        }
        if (actor->AsActorState()->IsSwimming()) {
            result.reason = "swimming";
            return result;
        }
        const auto pos = actor->GetPosition();
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
            result.reason = "invalid-position";
            return result;
        }
        if (Surfaces::GroundAt(pos).type != Surfaces::Type::kSnow) {
            result.reason = "non-snow-material";
            return result;
        }
        auto* tes = RE::TES::GetSingleton();
        float landZ{};
        if (!tes || !tes->GetLandHeight(pos, landZ)) {
            result.reason = "missing-land-height";
            return result;
        }
        result.landZ = landZ;
        const float lift = Settings::enableSnowRaise ? SnowSurface::LiftAt(pos.x, pos.y) : 0.0f;
        result.z = landZ + lift;
        if (pos.z - landZ > std::max(8.0f, lift + 4.0f) || pos.z < landZ - 32.0f) {
            result.reason = "outside-ground-contact";
            return result;
        }
        result.snow = lift > 0.1f;
        result.reason = result.snow ? "terrain-snow" : "snow-suppressed";
        result.offset = ActorSnowHeightMath::Offset(result.z, root->world.translate.z, result.snow);
        return result;
    }
    void Clear() {
        for (const auto& [id, entry] : tracked) Zero(entry);
        tracked.clear();
    }
    bool Available(RE::Actor* actor) {
        float previous{};
        return Settings::enableSnowHeightVariable && actor && actor->Is3DLoaded() &&
               actor->GetGraphVariableFloat(Variable(), previous);
    }
    void BeginFrame() {
        if (!Settings::enableSnowHeightVariable) {
            Clear();
            return;
        }
        for (auto& [id, entry] : tracked) entry.seen = false;
    }
    void Publish(RE::Actor* actor, const Surface& surface) {
        if (!Settings::enableSnowHeightVariable || !actor || !actor->Is3DLoaded()) return;
        const auto id = actor->GetFormID();
        if (!tracked.contains(id) && tracked.size() >= 128) return;
        if (actor->SetGraphVariableFloat(Variable(), surface.offset)) {
            tracked.insert_or_assign(id, Entry{actor->GetHandle(), true});
        }
        if (!reported) {
            reported = true;
            float readBack{};
            if (actor->GetGraphVariableFloat(Variable(), readBack)) {
                logger::info("Actor snow data: NMN_SnowHeight is in the actor's graph and being written");
            } else {
                logger::info("Actor snow data: NMN_SnowHeight is not in the actor's graph, so nothing can read it; is NMN_DeformableTerrain_BDI.json in SKSE/Plugins/BehaviorDataInjector?");
            }
        }
    }
    namespace {
        void Report() {
            if (!Settings::logSnowHeight) return;
            static std::chrono::steady_clock::time_point next{};
            const auto now = std::chrono::steady_clock::now();
            if (now < next) return;
            next = now + std::chrono::seconds(1);
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* root = player ? player->Get3D(false) : nullptr;
            if (!root) return;
            float value{};
            const bool declared = player->GetGraphVariableFloat(Variable(), value);
            const auto surface = Resolve(player);
            size_t readable{}, inSnow{};
            float deepest{};
            for (const auto& [id, entry] : tracked) {
                float other{};
                if (auto actor = entry.handle.get(); actor && actor->GetGraphVariableFloat(Variable(), other)) {
                    ++readable;
                    if (other > 0.01f) ++inSnow;
                    deepest = std::max(deepest, other);
                }
            }
            logger::info("Snow height: player declared={} value={:.2f} expected={:.2f} reason={} land={:.1f} surface={:.1f} root={:.1f} | actors written={} readable={} inSnow={} deepest={:.2f}",
                declared, value, surface.offset, surface.reason, surface.landZ, surface.z, root->world.translate.z,
                tracked.size(), readable, inSnow, deepest);
        }
    }
    void EndFrame() {
        for (auto it = tracked.begin(); it != tracked.end();) {
            if (!it->second.seen) {
                Zero(it->second);
                it = tracked.erase(it);
            } else
                ++it;
        }
        Report();
    }
}
