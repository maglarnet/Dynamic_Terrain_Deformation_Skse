// SPDX-License-Identifier: GPL-3.0-only
#pragma once

namespace TerrainDepthCorrection
{
	inline constexpr auto source = R"hlsl(
// Depth offsets (Terrain Blending, Reverse Z) change only clip z, so solve the camera-relative
// position from clip x, y and w. An orthographic camera has a constant w and keeps z.
float3 CameraRelativeFromClip(float4 clipPosition, float4x4 viewProj)
{
    const bool   perspective = dot(viewProj[3].xyz, viewProj[3].xyz) > 0.0f;
    const float4 depthRow    = perspective ? viewProj[3] : viewProj[2];
    const float3 rhs = float3(clipPosition.xy, perspective ? clipPosition.w : clipPosition.z) -
        float3(viewProj[0].w, viewProj[1].w, depthRow.w);
    const float3 yd = cross(viewProj[1].xyz, depthRow.xyz);
    return (rhs.x * yd + rhs.y * cross(depthRow.xyz, viewProj[0].xyz) +
        rhs.z * cross(viewProj[0].xyz, viewProj[1].xyz)) / dot(viewProj[0].xyz, yd);
}

// An offset moves the near plane too; clip where the camera's own depth would be clipped.
float TerrainNearClipDistance(float4 clipPosition, float3 cameraRelative, float4x4 viewProj, float4x4 projection)
{
    const float z = dot(viewProj[2], float4(cameraRelative, 1.0f));
    return projection[2][2] * projection[3][2] < 0 ? clipPosition.w - z : z;
}
)hlsl";
}
