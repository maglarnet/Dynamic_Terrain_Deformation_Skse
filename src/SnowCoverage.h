// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace SnowCoverage
{

	inline constexpr uint32_t kTexels = 256;
	inline constexpr float    kWorldSize = 16384.0f;
	inline constexpr float    kTexelSize = kWorldSize / static_cast<float>(kTexels);

	inline constexpr uint32_t kSlot = 1;
	inline constexpr uint32_t kSamplerSlot = 1;

	bool Initialize();
	void Shutdown();
	bool Ready();

	void Update();

	void Reset();
	void ForgetWindow();

	float At(float a_worldX, float a_worldY);

	void BindDomain(ID3D11DeviceContext* a_context);
	void UnbindDomain(ID3D11DeviceContext* a_context);

	ID3D11ShaderResourceView* View();
}
