// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "ShaderReflection.h"

namespace Tessellation
{

	inline constexpr uint32_t kMaterialSlot = 11;


	enum class Mode
	{
		kLandscape,

		kStaticProbe,


		kBloodDecal,
	};

	bool WantsDisplacement(Mode a_mode);
	bool WantsSubdivision(Mode a_mode);

	struct DrawMaterial
	{

		int revealLayer{ -1 };


		int snowLayer{ -1 };

		float strengthScale{ 1.0f };

	};

	bool BeginDraw(uint64_t a_vertexDesc, const Reflection::Signature& a_signature,
		Mode a_mode, const DrawMaterial& a_material);

	void EndDraw();

	bool Active();

	void VertexShaderBound(ID3D11DeviceContext*, ID3D11VertexShader*);
	ID3D11RasterizerState* RasterizerFor(ID3D11DeviceContext*, ID3D11RasterizerState*);

	void Reset();
	void PrepareFrame();
}
