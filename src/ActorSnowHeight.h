// SPDX-License-Identifier: GPL-3.0-only
#pragma once
namespace RE {
    class Actor;
}
namespace ActorSnowHeight {
    struct Surface {
        bool snow{};
        float z{}, offset{}, landZ{};
        const char* reason{"no-3d"};
    };
    Surface Resolve(RE::Actor* actor);
    bool Available(RE::Actor* actor);
    void BeginFrame();
    void Publish(RE::Actor* actor, const Surface& surface);
    void EndFrame();
    void Clear();
}
