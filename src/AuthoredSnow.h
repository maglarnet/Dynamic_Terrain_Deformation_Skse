// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Clipmap.h"
namespace RE {
    class BSGeometry;
}
namespace AuthoredSnow {
    struct Contact {
        uint64_t mesh{};
        float z{}, gx{}, gy{};
    };
    bool Matches(RE::BSGeometry* geometry);
    void BeginGather(float dt);
    bool FindContact(float x, float y, float bottom, float clearance, Contact& result);
    void NotePress(const Clipmap::Stamp& stamp);
    void NewFrame();
    bool NeedsDraw(RE::BSGeometry* geometry);
    void Reset();
    void Release();
}
