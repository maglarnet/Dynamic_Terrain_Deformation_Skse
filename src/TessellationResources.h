// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <d3d11.h>

namespace Tessellation
{

	struct SavedResources
	{
		ID3D11Buffer* domainBuffers[4]{};
		ID3D11Buffer* hullBuffers[4]{};
		ID3D11ShaderResourceView* domainViews[6]{};
		ID3D11ShaderResourceView* hullViews[2]{};
		ID3D11SamplerState* domainSamplers[3]{};

		SavedResources() = default;
		SavedResources(const SavedResources&) = delete;
		SavedResources& operator=(const SavedResources&) = delete;
		~SavedResources() { Release(); }

		template <class T, unsigned N>
		static void Drop(T* (&objects)[N])
		{
			for (auto*& object : objects) {
				if (object) { object->Release(); object = nullptr; }
			}
		}

		void Release()
		{
			Drop(domainBuffers); Drop(hullBuffers);
			Drop(domainViews); Drop(hullViews); Drop(domainSamplers);
		}

		void Capture(ID3D11DeviceContext* context)
		{
			Release();
			context->DSGetConstantBuffers(10, 4, domainBuffers);
			context->HSGetConstantBuffers(10, 4, hullBuffers);
			context->DSGetShaderResources(0, 6, domainViews);
			context->HSGetShaderResources(0, 2, hullViews);
			context->DSGetSamplers(0, 3, domainSamplers);
		}

		void Restore(ID3D11DeviceContext* context)
		{
			context->DSSetConstantBuffers(10, 4, domainBuffers);
			context->HSSetConstantBuffers(10, 4, hullBuffers);
			context->DSSetShaderResources(0, 6, domainViews);
			context->HSSetShaderResources(0, 2, hullViews);
			context->DSSetSamplers(0, 3, domainSamplers);
			Release();
		}
	};
}
