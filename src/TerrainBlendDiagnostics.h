// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once

#include "Settings.h"
#include "ShaderRegistry.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <chrono>
#include <format>
#include <set>
#include <string>
#include <string_view>

namespace TerrainBlendDiagnostics
{

	struct Snapshot
	{
		uintptr_t vs{}, hs{}, ds{}, target{}, vsFrame{}, dsFrame{};
		unsigned topology{}, depthFunc{}, depthWrite{}, format{};
		bool depthEnabled{};
		uintptr_t ps{}, blendDepthView{};
		unsigned cull{}, fill{}, blendSrc{}, blendDst{}, blendOp{}, writeMask{};
		bool frontCCW{}, depthClip{}, blendEnabled{};
		int rasterBias{};
		float slopeBias{};
		float correctionBias{};
	};
	inline Snapshot before{};
	inline bool armed{}, started{};
	inline unsigned samples{};
	inline std::chrono::steady_clock::time_point deadline{};
	inline std::set<std::string> reported;
	inline std::set<uintptr_t> shaders;

	template<class T> inline uintptr_t Identity(T* object)
	{
		const auto id = reinterpret_cast<uintptr_t>(object);
		if (object) { object->Release(); }
		return id;
	}

	inline void DescribeShader(ID3D11VertexShader* shader)
	{
		if (!shader || shaders.size() >= 8 || !shaders.insert(reinterpret_cast<uintptr_t>(shader)).second) { return; }
		const auto bytes = ShaderRegistry::For(shader);
		if (!bytes) {
			logger::info("TB diagnostic VS={:X}: bytecode unknown", reinterpret_cast<uintptr_t>(shader));
			return;
		}
		ID3DBlob* assembly{};
		if (SUCCEEDED(D3DDisassemble(bytes.data, bytes.size, 0, nullptr, &assembly)) && assembly) {
			logger::info("TB diagnostic VS={:X}, bytes={}:\n{}", reinterpret_cast<uintptr_t>(shader), bytes.size,
				std::string_view(static_cast<const char*>(assembly->GetBufferPointer()),
					std::min<size_t>(assembly->GetBufferSize() ? assembly->GetBufferSize() - 1 : 0, 65536)));
			assembly->Release();
		}
	}

	inline Snapshot Capture(ID3D11DeviceContext* context, float correctionBias)
	{
		Snapshot result;
		result.correctionBias = correctionBias;
		ID3D11VertexShader* vs{}; ID3D11HullShader* hs{}; ID3D11DomainShader* ds{};
		ID3D11DepthStencilView* target{}; ID3D11DepthStencilState* depth{};
		ID3D11Buffer* vsFrame{}; ID3D11Buffer* dsFrame{};
		ID3D11PixelShader* ps{};
		ID3D11ShaderResourceView* blendDepthView{};
		ID3D11RasterizerState* raster{};
		ID3D11BlendState* blend{};
		D3D11_PRIMITIVE_TOPOLOGY topology{};
		context->VSGetShader(&vs, nullptr, nullptr);
		context->HSGetShader(&hs, nullptr, nullptr);
		context->DSGetShader(&ds, nullptr, nullptr);
		context->OMGetRenderTargets(0, nullptr, &target);
		context->OMGetDepthStencilState(&depth, nullptr);
		context->VSGetConstantBuffers(12, 1, &vsFrame);
		context->DSGetConstantBuffers(12, 1, &dsFrame);
		context->IAGetPrimitiveTopology(&topology);
		context->PSGetShader(&ps, nullptr, nullptr);
		context->PSGetShaderResources(55, 1, &blendDepthView);
		context->RSGetState(&raster);
		context->OMGetBlendState(&blend, nullptr, nullptr);
		D3D11_RASTERIZER_DESC rasterDesc{};
		if (raster) { raster->GetDesc(&rasterDesc); }
		else { rasterDesc.CullMode = D3D11_CULL_BACK; rasterDesc.FillMode = D3D11_FILL_SOLID; rasterDesc.DepthClipEnable = TRUE; }
		result.cull = rasterDesc.CullMode; result.fill = rasterDesc.FillMode;
		result.frontCCW = rasterDesc.FrontCounterClockwise != FALSE;
		result.depthClip = rasterDesc.DepthClipEnable != FALSE;
		result.rasterBias = rasterDesc.DepthBias; result.slopeBias = rasterDesc.SlopeScaledDepthBias;
		D3D11_BLEND_DESC blendDesc{};
		if (blend) { blend->GetDesc(&blendDesc); }
		else {
			blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
			blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
			blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		}
		const auto& rt = blendDesc.RenderTarget[0];
		result.blendEnabled = rt.BlendEnable != FALSE;
		result.blendSrc = rt.SrcBlend; result.blendDst = rt.DestBlend;
		result.blendOp = rt.BlendOp; result.writeMask = rt.RenderTargetWriteMask;
		result.ps = Identity(ps); result.blendDepthView = Identity(blendDepthView);
		Identity(raster); Identity(blend);
		D3D11_DEPTH_STENCIL_DESC desc{};
		if (depth) { depth->GetDesc(&desc); }
		else { desc.DepthEnable = TRUE; desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; desc.DepthFunc = D3D11_COMPARISON_LESS; }
		result.depthEnabled = desc.DepthEnable != FALSE;
		result.depthFunc = desc.DepthFunc; result.depthWrite = desc.DepthWriteMask;
		if (target) { D3D11_DEPTH_STENCIL_VIEW_DESC view{}; target->GetDesc(&view); result.format = view.Format; }
		DescribeShader(vs);
		result.vs = Identity(vs); result.hs = Identity(hs); result.ds = Identity(ds);
		result.target = Identity(target); result.vsFrame = Identity(vsFrame); result.dsFrame = Identity(dsFrame);
		Identity(depth); result.topology = topology;
		return result;
	}

	inline std::string Describe(const Snapshot& s)
	{
		return std::format("VS={:X} HS={:X} DS={:X} DSV={:X} format={} topology={} depth={}/{}/{} frameVS={:X} frameDS={:X}",
			s.vs, s.hs, s.ds, s.target, s.format, s.topology, s.depthEnabled, s.depthFunc, s.depthWrite, s.vsFrame, s.dsFrame) +
			std::format(" correctionBias={}", s.correctionBias) +
			std::format(" PS={:X} TB55={:X} cull={} frontCCW={} fill={} depthClip={} rasterBias={}/{} blend0={}/{}/{}/{} write0={}",
				s.ps, s.blendDepthView, s.cull, s.frontCCW, s.fill, s.depthClip, s.rasterBias, s.slopeBias,
				s.blendEnabled, s.blendSrc, s.blendDst, s.blendOp, s.writeMask);
	}
	inline void Begin(ID3D11DeviceContext* context, float correctionBias)
	{
		armed = false;
		if (!Settings::logTerrainBlendDraws || !Settings::enableLogging) { return; }
		const auto now = std::chrono::steady_clock::now();
		if (!started) {
			started = true; deadline = now + std::chrono::seconds(60);
			logger::info("TB diagnostic v3 started: correction bias/raster/blend/PS capture; 60 seconds / 65536 terrain draws, 32 state pairs, 8 VS listings; observation only");
		}
		if (now >= deadline || samples >= 65536 || reported.size() >= 32) { return; }
		++samples; before = Capture(context, correctionBias); armed = true;
	}
	inline void End(ID3D11DeviceContext* context, float correctionBias)
	{
		if (!armed) { return; }
		armed = false;
		const auto after = Capture(context, correctionBias);
		const auto pair = Describe(before) + " -> " + Describe(after);
		if (reported.insert(pair).second) { logger::info("TB diagnostic pair {}: {}", reported.size(), pair); }
	}
	inline void Reset()
	{
		armed = started = false; samples = 0; reported.clear(); shaders.clear();
	}
}
