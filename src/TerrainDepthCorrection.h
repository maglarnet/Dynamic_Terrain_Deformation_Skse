// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace TerrainDepthCorrection
{
	inline constexpr auto source = R"hlsl(
float4 RemoveTerrainDepthOffset(float4 clipPosition, float4x4 projection, float2 offset)
{
    bool reverseProjection = offset.y != 0 && projection[2][2] * projection[3][2] < 0;
    clipPosition.z -= reverseProjection ? -offset.x : offset.x;
    return clipPosition;
}

// The offset moves the near plane too; clip where the uncorrected position would be clipped.
float TerrainNearClipDistance(float4 clipPosition, float4x4 projection)
{
    return projection[2][2] * projection[3][2] < 0 ? clipPosition.w - clipPosition.z : clipPosition.z;
}
)hlsl";
}
