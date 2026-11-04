// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

namespace PatchArea
{
	inline constexpr const char* source = R"(
bool PatchHasNoArea(float3 a, float3 b, float3 c)
{
    const float longest = max(length(b - a), max(length(c - b), length(a - c)));
    return length(cross(b - a, c - a)) <= 0.05f * longest;
}
)";

	inline constexpr const char* cull =
		"\tif (PatchHasNoArea(p0, p1, p2)) {\n"
		"\t\to.edges[0] = 0.0f;\n"
		"\t\to.edges[1] = 0.0f;\n"
		"\t\to.edges[2] = 0.0f;\n"
		"\t\to.inside   = 0.0f;\n"
		"\t\treturn o;\n"
		"\t}\n\n";
}
