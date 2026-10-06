// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace GatherSnapshotPolicy
{
    struct Identity
    {
        std::uint64_t generation{};
        std::uint32_t world{}, cell{};
        std::uintptr_t root{};
        bool operator==(const Identity&) const = default;
    };
    struct State
    {
        Identity identity{};
        bool ready{};
        void Publish(Identity value) { identity = value; ready = true; }
        void Reset() { ready = false; }
        bool Consume(Identity current)
        {
            const bool valid = ready && current == identity;
            ready = false;
            return valid;
        }
    };
}
