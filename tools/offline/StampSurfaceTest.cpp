// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <string>
#include <string_view>
#include "ClipmapUpdateCS.h"
#include "ClipmapMetadata.h"
#include "ImpactPatterns.h"
#include "ShelterTransition.h"
#include "ShelterFadeCells.h"
#include "BoxFilter.h"
#include "CoverageShelterCombine.h"
#include <chrono>
#include <random>
#include "ShelterScan.h"
#include "ShelterProbeCache.h"
#include "BlanketTessellation.h"
#include "TessellationResources.h"
#include "BloodDecalFilter.h"
#include "ObjectStampFilter.h"
#include "TerrainDepthBias.h"
#include "TerrainCulling.h"
#include "TerrainActivityPolicy.h"
#include <limits>
#include <fstream>
#include <iterator>

using Microsoft::WRL::ComPtr;
constexpr UINT N = 64;
struct Params
{
	float window[4]{ 0, 0, 1, N };
	float control[4]{ 0, 1, N / 2 - 2, 0.55f };
	float weather[4]{ 0, 0, 0, 0.4f };
	float stamps[Clipmap::kMaxStamps][4]{};
	float stampParams[Clipmap::kMaxStamps][4]{};
	float stampShape[Clipmap::kMaxStamps][4]{};
	float stampMotion[Clipmap::kMaxStamps][4]{};
	float coarse[4]{ 0, N, 0, N / 2 - 2 };
	float rimShape[4]{ 0.5f, 0.45f, 0, 0 };
	float snowRim[4]{ 0.55f, 0.4f, 0.5f, 0.45f };

	float raise[4]{ 0, 1, 0, 0 };
	float raiseWindow[4]{ 0, 0, 1.0e6f, 2.0e6f };
	float stampBounds[Clipmap::kMaxStamps][4]{};
	Params()
	{
		stamps[0][2] = 5;
		stamps[0][3] = 2;
		stampParams[0][0] = 0.3f;
		stampParams[0][1] = 0.92f;
		stampParams[0][3] = 1;
	}
};
void Require(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
void Check(HRESULT hr, const char* message) { Require(SUCCEEDED(hr), message); }

class Field
{
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<ID3D11ComputeShader> shader;
	ComPtr<ID3D11Texture2D> height, metadata, activity, readHeight, readMeta, nextHeight;
	ComPtr<ID3D11ShaderResourceView> heightSRV, nextHeightSRV;
	ComPtr<ID3D11UnorderedAccessView> nextHeightUAV;
	ComPtr<ID3D11UnorderedAccessView> heightUAV, metaUAV, activityUAV;
	ComPtr<ID3D11ShaderResourceView> coarseHeight, coarseMeta;
	ComPtr<ID3D11Texture2D> coverage, meshCap;
	ComPtr<ID3D11ShaderResourceView> coverageSRV, meshCapSRV;
	ComPtr<ID3D11Buffer> cb;
	ComPtr<ID3D11SamplerState> sampler;
	DXGI_FORMAT metadataFormat;
public:
	explicit Field(DXGI_FORMAT format = Clipmap::kMetadataFormat) : metadataFormat(format)
	{
		const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
		Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
			D3D11_SDK_VERSION, &device, nullptr, &context), "Create WARP device");
		ComPtr<ID3DBlob> code, errors;
		const auto source = Clipmap::UpdateShaderSource();
		const auto count = std::to_string(Clipmap::kMaxStamps);
		const D3D_SHADER_MACRO defines[]{ { "MAX_STAMPS", count.c_str() }, {} };
		const auto hr = D3DCompile(source.data(), source.size(), "StampSurfaceTest", defines,
			nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr) && errors) { std::fprintf(stderr, "%s\n", static_cast<char*>(errors->GetBufferPointer())); }
		Check(hr, "Compile shipping stamp shader");
		Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader), "Create shader");
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = desc.Height = N;
		desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		Check(device->CreateTexture2D(&desc, nullptr, &height), "Create height");
		Check(device->CreateUnorderedAccessView(height.Get(), nullptr, &heightUAV), "Height UAV");
		Check(device->CreateShaderResourceView(height.Get(), nullptr, &heightSRV), "Height SRV");
		Check(device->CreateTexture2D(&desc, nullptr, &nextHeight), "Spare height");
		Check(device->CreateUnorderedAccessView(nextHeight.Get(), nullptr, &nextHeightUAV), "Spare UAV");
		Check(device->CreateShaderResourceView(nextHeight.Get(), nullptr, &nextHeightSRV), "Spare SRV");
		desc.Format = metadataFormat;
		Check(device->CreateTexture2D(&desc, nullptr, &metadata), "Create metadata");
		Check(device->CreateUnorderedAccessView(metadata.Get(), nullptr, &metaUAV), "Metadata UAV");
		desc.Format = DXGI_FORMAT_R32_UINT;
		desc.Width = desc.Height = Clipmap::kActivityTexels;
		Check(device->CreateTexture2D(&desc, nullptr, &activity), "Create activity");
		Check(device->CreateUnorderedAccessView(activity.Get(), nullptr, &activityUAV), "Activity UAV");
		desc.Width = desc.Height = N;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		Check(device->CreateTexture2D(&desc, nullptr, &readHeight), "Height readback");
		desc.Format = metadataFormat;
		Check(device->CreateTexture2D(&desc, nullptr, &readMeta), "Metadata readback");
		D3D11_BUFFER_DESC buffer{};
		buffer.ByteWidth = sizeof(Params);
		buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		Check(device->CreateBuffer(&buffer, nullptr, &cb), "Params buffer");
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		Check(device->CreateSamplerState(&sd, &sampler), "Sampler");

		D3D11_TEXTURE2D_DESC map{};
		map.Width = map.Height = 4;
		map.MipLevels = map.ArraySize = map.SampleDesc.Count = 1;
		map.Format = DXGI_FORMAT_R32_FLOAT;
		map.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		Check(device->CreateTexture2D(&map, nullptr, &coverage), "Create coverage");
		Check(device->CreateTexture2D(&map, nullptr, &meshCap), "Create mesh cap");
		Check(device->CreateShaderResourceView(coverage.Get(), nullptr, &coverageSRV), "Coverage SRV");
		Check(device->CreateShaderResourceView(meshCap.Get(), nullptr, &meshCapSRV), "Mesh cap SRV");
		SetFloorMaps(1.0f, 1.0f);
		Clear();
	}
	void CheckTerrainCulling()
	{
		TerrainCulling::Bracket bracket;
		D3D11_RASTERIZER_DESC desc{};
		desc.FillMode = D3D11_FILL_WIREFRAME; desc.CullMode = D3D11_CULL_NONE;
		desc.FrontCounterClockwise = TRUE; desc.DepthBias = -9;
		desc.DepthBiasClamp = 0.5f; desc.SlopeScaledDepthBias = -1.2f;
		desc.DepthClipEnable = TRUE; desc.ScissorEnable = TRUE;
		desc.MultisampleEnable = TRUE;
		ComPtr<ID3D11RasterizerState> none, front;
		Check(device->CreateRasterizerState(&desc, &none), "Create no-cull test state");
		Require(bracket.Select(device.Get(), none.Get()) == none.Get(), "Inactive bracket preserves non-terrain state");
		context->RSSetState(nullptr);
		bracket.Begin(context.Get());
		auto* selected = bracket.Select(device.Get(), none.Get());
		Require(selected && selected != none.Get(), "No-cull terrain gets replacement");
		D3D11_RASTERIZER_DESC actual{}; selected->GetDesc(&actual);
		Require(actual.CullMode == D3D11_CULL_BACK && actual.FrontCounterClockwise == desc.FrontCounterClockwise &&
			actual.FillMode == desc.FillMode && actual.DepthBias == desc.DepthBias && actual.DepthBiasClamp == desc.DepthBiasClamp &&
			actual.SlopeScaledDepthBias == desc.SlopeScaledDepthBias && actual.DepthClipEnable == desc.DepthClipEnable &&
			actual.ScissorEnable == desc.ScissorEnable && actual.MultisampleEnable == desc.MultisampleEnable &&
			actual.AntialiasedLineEnable == desc.AntialiasedLineEnable, "Only cull mode changes");
		Require(bracket.Select(device.Get(), none.Get()) == selected, "Equivalent request reuses replacement");
		context->RSSetState(selected);
		bracket.End(context.Get());
		ComPtr<ID3D11RasterizerState> restored; context->RSGetState(&restored);
		Require(restored.Get() == none.Get(), "Restore latest renderer request, not initial null");
		desc.CullMode = D3D11_CULL_FRONT;
		Check(device->CreateRasterizerState(&desc, &front), "Create front-cull test state");
		bracket.Begin(context.Get());
		Require(bracket.Select(device.Get(), front.Get()) == front.Get(), "Intentional front-face culling preserved");
		context->RSSetState(bracket.Select(device.Get(), nullptr));
		bracket.End(context.Get());
		restored.Reset(); context->RSGetState(&restored);
		Require(!restored, "Restore late null/default request exactly");
		Require(bracket.Select(device.Get(), none.Get()) == none.Get(), "Subsequent object draw remains unmodified");
		bracket.Reset();
		std::puts("PASS terrain culling scope, descriptor preservation, cache and latest-state restoration");
	}

	void CheckTessellationResources()
	{
		ComPtr<ID3D11ShaderResourceView> view;
		Check(device->CreateShaderResourceView(height.Get(), nullptr, &view), "State test SRV");
		for (unsigned phase = 0; phase < 2; ++phase) {

			for (UINT slot = 0; slot < 14; ++slot) {
				auto* value = (slot + phase) % 2 ? cb.Get() : nullptr;
				context->DSSetConstantBuffers(slot, 1, &value);
				context->HSSetConstantBuffers(slot, 1, &value);
			}
			for (UINT slot = 0; slot < 8; ++slot) {
				auto* srv = (slot + phase) % 2 ? view.Get() : nullptr;
				auto* state = (slot + phase) % 2 ? sampler.Get() : nullptr;
				context->DSSetShaderResources(slot, 1, &srv);
				context->HSSetShaderResources(slot, 1, &srv);
				context->DSSetSamplers(slot, 1, &state);
			}
			Tessellation::SavedResources saved;
			saved.Capture(context.Get());
			for (UINT slot = 10; slot < 14; ++slot) {
				auto* value = (slot + phase) % 2 ? nullptr : cb.Get();
				context->DSSetConstantBuffers(slot, 1, &value);
				if (slot >= 12) { context->HSSetConstantBuffers(slot, 1, &value); }
			}
			for (UINT slot = 0; slot < 4; ++slot) {
				auto* srv = (slot + phase) % 2 ? nullptr : view.Get();
				auto* state = (slot + phase) % 2 ? nullptr : sampler.Get();
				context->DSSetShaderResources(slot, 1, &srv);
				if (slot == 0) { context->HSSetShaderResources(slot, 1, &srv); }
				if (slot < 3) { context->DSSetSamplers(slot, 1, &state); }
			}
			saved.Restore(context.Get());
			for (UINT slot = 0; slot < 14; ++slot) {
				ComPtr<ID3D11Buffer> domain, hull;
				context->DSGetConstantBuffers(slot, 1, &domain);
				context->HSGetConstantBuffers(slot, 1, &hull);
				auto* expected = (slot + phase) % 2 ? cb.Get() : nullptr;
				Require(domain.Get() == expected && hull.Get() == expected, "Restore HS/DS constant buffers");
			}
			for (UINT slot = 0; slot < 8; ++slot) {
				ComPtr<ID3D11ShaderResourceView> domain, hull;
				ComPtr<ID3D11SamplerState> state;
				context->DSGetShaderResources(slot, 1, &domain);
				context->HSGetShaderResources(slot, 1, &hull);
				context->DSGetSamplers(slot, 1, &state);
				auto* expected = (slot + phase) % 2 ? view.Get() : nullptr;
				Require(domain.Get() == expected && hull.Get() == expected, "Restore HS/DS resources");
				Require(state.Get() == ((slot + phase) % 2 ? sampler.Get() : nullptr), "Restore DS samplers");
			}
		}
		context->ClearState();
		std::puts("PASS tessellation resource restoration: occupied/null slots and untouched neighbours");
	}
	void CheckBlanketBudget()
	{
		const std::string source = std::string(BlanketTessellation::source) + R"(
RWTexture2D<float> Result : register(u0);
[numthreads(8,1,1)] void main(uint3 id : SV_DispatchThreadID) {
    float values[8] = {
        BlanketDetail(128, 2, 64, 8, 64, 0),
        BlanketDetail(128, 2, 64, 8, 64, 1),
        BlanketDetail(128, 2, 64, 8, 64, 0.5),
        BlanketDetail(128, 2, 64, 0, 64, 0),
        BlanketDetail(128, 2, 8, 8, 8, 0),
        BlanketDetail(128, 2, 4, 8, 64, 0),
        BlanketDetail(128, 32, 64, 8, 64, 0),
        BlanketDetail(128, 2, 64, 8, 64, -1)
    };
    Result[id.xy] = values[id.x];
})";
		ComPtr<ID3DBlob> code, errors;
		Check(D3DCompile(source.data(), source.size(), "BlanketBudgetTest", nullptr,
			nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors), "Compile blanket budget");
		ComPtr<ID3D11ComputeShader> budget;
		Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &budget), "Create blanket budget shader");
		context->CSSetShader(budget.Get(), nullptr, 0);
		context->CSSetUnorderedAccessViews(0, 1, heightUAV.GetAddressOf(), nullptr);
		context->Dispatch(1, 1, 1);
		ID3D11UnorderedAccessView* empty = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &empty, nullptr);
		const auto result = Read();
		const float expected[]{16, 64, 40, 2, 8, 4, 32, 16};
		for (size_t i = 0; i < 8; ++i) {
			Require(std::abs(result[i] - expected[i]) < 0.001f, "Blanket floor, detail limits and activity regression");
		}
		Clear();
		std::puts("PASS GPU blanket budget: empty snow, active marks, interpolation, off, caps and finer user baseline");
	}

	void SetFloorMaps(float a_cover, float a_cap)
	{
		const float cover[16]{ a_cover, a_cover, a_cover, a_cover, a_cover, a_cover,
			a_cover, a_cover, a_cover, a_cover, a_cover, a_cover, a_cover, a_cover,
			a_cover, a_cover };
		const float cap[16]{ a_cap, a_cap, a_cap, a_cap, a_cap, a_cap, a_cap, a_cap,
			a_cap, a_cap, a_cap, a_cap, a_cap, a_cap, a_cap, a_cap };
		context->UpdateSubresource(coverage.Get(), 0, nullptr, cover, 4 * sizeof(float), 0);
		context->UpdateSubresource(meshCap.Get(), 0, nullptr, cap, 4 * sizeof(float), 0);
	}
	void Clear()
	{
		const float zero[4]{};
		context->ClearUnorderedAccessViewFloat(heightUAV.Get(), zero);
		context->ClearUnorderedAccessViewFloat(metaUAV.Get(), zero);
	}
	void Step(Params params, bool seed = false)
	{
		Clipmap::FillStampBounds(params, static_cast<uint32_t>(params.control[1]));
		context->UpdateSubresource(cb.Get(), 0, nullptr, &params, 0, 0);
		ID3D11UnorderedAccessView* uavs[]{ nextHeightUAV.Get(), metaUAV.Get(), activityUAV.Get() };
		ID3D11ShaderResourceView* srvs[]{ nullptr, seed ? coarseHeight.Get() : nullptr,
			seed ? coarseMeta.Get() : nullptr, coverageSRV.Get(), meshCapSRV.Get(), heightSRV.Get() };
		context->CSSetShader(shader.Get(), nullptr, 0);
		context->CSSetShaderResources(0, 6, srvs);
		context->CSSetSamplers(0, 1, sampler.GetAddressOf());
		context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
		context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
		context->Dispatch(N / 8, N / 8, 1);
		ID3D11UnorderedAccessView* nullUAVs[3]{};
		ID3D11ShaderResourceView* nullSRVs[6]{};
		context->CSSetUnorderedAccessViews(0, 3, nullUAVs, nullptr);
		context->CSSetShaderResources(0, 6, nullSRVs);
		height.Swap(nextHeight);
		heightSRV.Swap(nextHeightSRV);
		heightUAV.Swap(nextHeightUAV);
	}
	std::vector<float> Read()
	{
		context->CopyResource(readHeight.Get(), height.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(readHeight.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read height");
		std::vector<float> result(N * N);
		for (UINT y = 0; y < N; ++y) {
			const auto row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
			std::copy_n(row, N, result.begin() + y * N);
		}
		context->Unmap(readHeight.Get(), 0);
		return result;
	}
	float MetadataAtOrigin(unsigned channel)
	{
		context->CopyResource(readMeta.Get(), metadata.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(readMeta.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Read metadata");
		const float value = metadataFormat == DXGI_FORMAT_R8G8_UNORM ?
			static_cast<const uint8_t*>(mapped.pData)[channel] / 255.0f :
			static_cast<const Clipmap::MetadataChannel*>(mapped.pData)[channel] / 65535.0f;
		context->Unmap(readMeta.Get(), 0);
		return value;
	}
	float SnowAtOrigin() { return MetadataAtOrigin(1); }
	void SaveCoarse()
	{
		D3D11_TEXTURE2D_DESC desc{};
		for (int i = 0; i < 2; ++i) {
			auto* source = i ? metadata.Get() : height.Get();
			source->GetDesc(&desc);
			ComPtr<ID3D11Texture2D> copy;
			Check(device->CreateTexture2D(&desc, nullptr, &copy), "Coarse copy");
			context->CopyResource(copy.Get(), source);
			Check(device->CreateShaderResourceView(copy.Get(), nullptr,
				i ? &coarseMeta : &coarseHeight), "Coarse SRV");
		}
	}
};

void CheckGroundFloor(Field& field)
{
	const auto deepest = [](const std::vector<float>& a_field) {
		return *std::min_element(a_field.begin(), a_field.end());
	};

	Params p;
	p.stampMotion[0][2] = 1;
	p.stamps[0][3] = 40.0f;
	field.SetFloorMaps(1.0f, 1.0f);

	field.Clear(); field.Step(p);
	const auto unfloored = field.Read();
	Require(deepest(unfloored) < -20.0f, "The test stamp must overshoot every blanket below");

	p.raise[0] = 10.0f;
	field.Clear(); field.Step(p);
	const float floored = deepest(field.Read());
	Require(floored > -10.001f, "A snow mark cut below the blanket standing over it");
	Require(floored < -9.999f, "The floor is not the blanket - the mark stopped short of the land");

	p.raise[2] = 3.0f;
	field.Clear(); field.Step(p);
	const float bitten = deepest(field.Read());
	Require(bitten > -13.001f && bitten < -12.999f, "SnowGroundBite is not the floor's slack");
	p.raise[2] = 0.0f;

	p.raise[3] = 1.0f;
	field.SetFloorMaps(1.0f, 0.5f);
	field.Clear(); field.Step(p);
	const float capped = deepest(field.Read());
	Require(capped > -5.001f && capped < -4.999f, "The mesh cap did not carry into the floor");
	p.raise[3] = 0.0f;

	field.SetFloorMaps(0.0f, 1.0f);
	field.Clear(); field.Step(p);
	Require(deepest(field.Read()) > -0.001f, "Ground with no blanket still took a snow cut");
	field.SetFloorMaps(1.0f, 1.0f);

	p.stampMotion[0][2] = 0;
	p.raise[0] = 0.0f;
	field.Clear(); field.Step(p);
	const auto mud = field.Read();
	p.raise[0] = 10.0f;
	field.Clear(); field.Step(p);
	Require(field.Read() == mud, "The floor reached ground that no snow stamp claimed");

	field.Clear();
	std::puts("PASS snow ground floor: blanket, bite, mesh cap, bare ground and other surfaces");
}

void CheckMagicPatterns(Field& field)
{
	const auto draw = [&](const ImpactPatterns::Pattern& pattern, bool snow, float span, float lean) {
		Params p;
		p.control[1] = static_cast<float>(pattern.count);
		p.control[3] = span; p.rimShape[0] = lean;
		p.snowRim[0] = span; p.snowRim[2] = lean;
		for (size_t i = 0; i < pattern.count; ++i) {
			const auto& s = pattern.strokes[i];
			p.stamps[i][0] = s.x; p.stamps[i][1] = s.y;
			p.stamps[i][2] = ImpactPatterns::StampRadius(s.radius, 1.0f, span, lean);
			p.stamps[i][3] = s.strength * 3.0f;
			p.stampParams[i][0] = 0.25f; p.stampParams[i][1] = 0.92f;
			p.stampParams[i][3] = s.strength * 0.8f;
			p.stampMotion[i][0] = s.motionX; p.stampMotion[i][1] = s.motionY;
			p.stampMotion[i][2] = snow ? 1.0f : 0.0f;
		}
		field.Clear(); field.Step(p);
		const auto first = field.Read();
		field.Step(p);
		Require(field.Read() == first, "Repeated impact must not deepen or accumulate rims");
		return first;
	};
	Require(ImpactPatterns::BoundedRadius(100000, 2, 256) == 256, "Explosion radius cap failed");
	Require(ImpactPatterns::BoundedRadius(std::numeric_limits<float>::infinity(), 1, 256) == 0,
		"Non-finite explosion radius must be rejected");
	Require(ImpactPatterns::Directional(20, 8, 0, 0, true).count == 0 &&
		ImpactPatterns::Directional(0, 8, 0, 1, true).count == 0, "Invalid direction/range must not stamp");
	for (bool snow : { false, true }) {
		for (float span : { 0.0f, 0.8f, 4.0f }) {
			const auto explosion = draw(ImpactPatterns::Explosion(20), snow, span, 1);
			Require(*std::min_element(explosion.begin(), explosion.end()) < -0.1f,
				"Explosion must deform both land and snow");
			for (int y = 0; y < N; ++y) {
				for (int x = 0; x < N; ++x) {
					const float value = explosion[y * N + x];
					Require(std::isfinite(value), "Impact generated non-finite displacement");
					const int wx = x < N/2 ? x : x - N, wy = y < N/2 ? y : y - N;
					if (std::hypot(static_cast<float>(wx), static_cast<float>(wy)) > 21) {
						Require(value == 0, "Explosion affected ground beyond radius cap");
					}
				}
			}
		}
		const auto forward = draw(ImpactPatterns::Directional(20, 10, 0, 1, true), snow, 0.8f, 1);
		Require(forward[14 * N] < -0.1f && forward[(N - 10) * N] == 0,
			"Shout must disturb ground ahead, not behind");
		const auto sideways = draw(ImpactPatterns::Directional(20, 10, 1, 0, true), snow, 0.8f, 1);
		Require(sideways[14] < -0.1f && sideways[14 * N] == 0,
			"Shout pattern did not turn with casting direction");
	}
	std::puts("PASS magic impacts: land/snow, directional shouts, radius caps and idempotence");
}

void TestShelterScan()
{
	using Result = ShelterScan::Result;
	constexpr uint32_t total = 128 * 128;
	uint32_t cursor = total - 7;
	std::vector<unsigned> visits(total);
	for (uint32_t frame = 0; frame < (total + 47) / 48; ++frame) {
		const auto counts = ShelterScan::Run(cursor, total, 48, 2, [&](uint32_t index) {
			++visits[index];
			return Result::kNoLand;
		});
		Require(counts.attempts == 48 && counts.misses == 48 && counts.rays == 0,
			"Missing land must consume a hard attempt budget without firing rays");
	}
	Require(std::all_of(visits.begin(), visits.end(), [](unsigned n) { return n >= 1 && n <= 2; }),
		"Budgeted retries must reach every cell across ring wrap without starvation");

	for (uint32_t rayCost : { 1u, 2u }) {
		for (uint32_t budget : { 1u, 2u, 3u, 48u }) {
			std::vector<uint32_t> expected, actual;
			uint32_t oldCursor = 93, oldRays = 0, scanned = 0;
			while (oldRays + rayCost <= std::max(budget, rayCost) && scanned < 97) {
				const uint32_t index = oldCursor;
				oldCursor = (oldCursor + 1) % 97;
				++scanned;
				if (index % 3 == 0) { continue; }
				expected.push_back(index);
				oldRays += rayCost;
			}
			cursor = 93;
			const auto counts = ShelterScan::Run(cursor, 97, budget, rayCost, [&](uint32_t index) {
				if (index % 3 == 0) { return Result::kCached; }
				actual.push_back(index);
				return Result::kUpdated;
			});
			Require(actual == expected && cursor == oldCursor && counts.rays == oldRays,
				"Loaded-land sampling order and ray budget must match the previous scheduler");
		}
	}

	std::vector<bool> filled(257);
	bool streamed = false;
	cursor = 250;
	const auto probe = [&](uint32_t index) {
		if (filled[index]) { return Result::kCached; }
		if (!streamed && index % 3 != 0) { return Result::kNoLand; }
		filled[index] = true;
		return Result::kUpdated;
	};
	for (unsigned frame = 0; frame < 20; ++frame) {
		const auto counts = ShelterScan::Run(cursor, 257, 48, 2, probe);
		Require(counts.attempts <= 48 && counts.rays <= 48, "Mixed land must obey both budgets");
	}
	for (uint32_t i = 0; i < filled.size(); ++i) {
		Require(filled[i] == (i % 3 == 0), "Failed land must remain invalid for later retry");
	}
	streamed = true;
	for (unsigned frame = 0; frame < 12; ++frame) {
		ShelterScan::Run(cursor, 257, 48, 2, probe);
	}
	Require(std::all_of(filled.begin(), filled.end(), [](bool value) { return value; }),
		"Land arriving under a stationary window must converge without a reset");
	const auto cached = ShelterScan::Run(cursor, 257, 48, 2, probe);
	Require(cached.scanned == 257 && cached.attempts == 0 && cached.rays == 0,
		"Cached cells must consume neither query nor ray budget");
	std::fill(filled.begin(), filled.end(), false);
	for (unsigned frame = 0; frame < 12; ++frame) {
		ShelterScan::Run(cursor, 257, 48, 2, probe);
	}
	Require(std::all_of(filled.begin(), filled.end(), [](bool value) { return value; }),
		"Invalidation after a window move must repopulate the ring");
	const auto empty = ShelterScan::Run(cursor, 0, 48, 2, [](uint32_t) {
		throw std::runtime_error("Empty ring must never be probed");
		return Result::kUpdated;
	});
	Require(empty.scanned == 0, "Empty ring must finish without scanning");
	std::puts("PASS shelter budgets: missing/mixed/loaded land, wrap, streaming retries and invalidation");
}

void CheckDecayPrecision(Field& field)
{
	Field legacy(DXGI_FORMAT_R8G8_UNORM);
	for (const float rate : {0.9999f, 0.999f, 0.99f}) {
		for (const float snow : {0.0f, 1.0f}) {
			Params p;
			p.stampParams[0][1] = rate;
			p.stampMotion[0][2] = snow;
			field.Clear(); field.Step(p);
			legacy.Clear(); legacy.Step(p);
			const float initial = field.Read()[0];
			const float oldInitial = legacy.Read()[0];
			const float stored = field.MetadataAtOrigin(0);
			Require(initial < -0.1f && stored < 1.0f, "Slow decay must be representable below one");
			Require(std::abs(stored - rate) <= 0.5f / 65535.0f + 1e-7f, "Decay rate quantization error");
			p.control[1] = 0; p.control[0] = 0.25f;
			for (int i = 0; i < 2400; ++i) { field.Step(p); legacy.Step(p); }
			const float recovered = field.Read()[0];
			const float expected = initial * std::pow(stored, 600.0f);
			Require(std::abs(recovered - expected) <= std::abs(expected) * 0.001f + 1e-6f,
				"Ten-minute recovery differs from stored-rate reference");
			Require(std::abs(recovered) < std::abs(initial) * 0.95f, "Mark did not recover");
			if (rate >= 0.999f) {
				Require(legacy.MetadataAtOrigin(0) == 1.0f && legacy.Read()[0] == oldInitial,
					"Legacy precision regression must reproduce a permanent mark");
			}
			Require(field.SnowAtOrigin() == snow, "Recovery changed snow classification");
			field.SaveCoarse(); field.Clear();
			p.control[0] = 0; p.coarse[0] = 1; p.coarse[3] = 0;
			field.Step(p, true);
			Require(std::abs(field.MetadataAtOrigin(0) - stored) < 1e-7f && field.SnowAtOrigin() == snow,
				"Coarse seeding changed recovery or snow classification");
			std::printf("PASS decay %.7f -> %.7f, snow %.0f, 600 s height %.7f (reference %.7f)\n",
				rate, stored, snow, recovered, expected);
		}
	}
	field.Clear();
}

void TestActiveShelterFades()
{
	for (const uint32_t size : {0u, 65u, 16384u}) {
		ShelterTransition::FadeCells active;
		active.Reset(size);
		std::vector<uint8_t> roof(size, 0), cap(size, 255);
		std::vector<float> oldRoof(size, 0), oldCap(size, 255), newRoof = oldRoof, newCap = oldCap;
		std::vector<uint8_t> oldRoofBytes = roof, oldCapBytes = cap, newRoofBytes = roof, newCapBytes = cap;
		std::mt19937 random(20260925);
		bool oldPending = false;
		uint64_t fullVisits = 0, activeVisits = 0;
		for (unsigned frame = 0; frame < 1200; ++frame) {
			if (frame == 600) {
				active.Reset(size);
				std::fill(roof.begin(), roof.end(), 0); std::fill(cap.begin(), cap.end(), 255);
				oldRoof.assign(size, 0); oldCap.assign(size, 255); newRoof = oldRoof; newCap = oldCap;
				oldRoofBytes = newRoofBytes = roof; oldCapBytes = newCapBytes = cap; oldPending = false;
			}
			if (frame == 400) {
				std::fill(roof.begin(), roof.end(), 0); std::fill(cap.begin(), cap.end(), 255);
				active.ActivateAll(); oldPending = true;
			}
			if (size && (frame < 20 || frame == 200 || frame == 401 || frame == 700)) {
				const auto changes = frame == 200 ? size : 24u;
				for (uint32_t j = 0; j < changes; ++j) {
					const auto i = frame == 200 ? j : random() % size;
					roof[i] = (random() & 1) ? 255 : 0; cap[i] = static_cast<uint8_t>(random());
					active.Activate(i); active.Activate(i); oldPending = true;
				}
			}
			const float dt = frame % 47 == 0 ? 0.0f : frame % 59 == 0 ?
				std::numeric_limits<float>::quiet_NaN() : frame % 3 == 0 ? 0.2f : 1.0f / 144;
			const float alpha = ShelterTransition::Alpha(dt);
			unsigned oldDirty = 0, newDirty = 0;
			const auto step = [&](uint32_t i, auto& r, auto& c, auto& rb, auto& cb, unsigned& dirty) {
				bool pending = ShelterTransition::Advance(r[i], roof[i], alpha);
				pending |= ShelterTransition::Advance(c[i], cap[i], alpha);
				const auto rByte = static_cast<uint8_t>(r[i] + 0.5f), cByte = static_cast<uint8_t>(c[i] + 0.5f);
				dirty |= (rByte != rb[i] ? 1u : 0u) | (cByte != cb[i] ? 2u : 0u);
				rb[i] = rByte; cb[i] = cByte;
				return pending;
			};
			if (oldPending) {
				oldPending = false; fullVisits += size;
				for (uint32_t i = 0; i < size; ++i) { oldPending |= step(i, oldRoof, oldCap, oldRoofBytes, oldCapBytes, oldDirty); }
			}
			bool newPending = false;
			activeVisits += active.Advance([&](uint32_t i) {
				Require(i < size, "Fade mask tail escaped cell array");
				const bool pending = step(i, newRoof, newCap, newRoofBytes, newCapBytes, newDirty);
				newPending |= pending; return pending;
			});
			Require(oldPending == newPending && oldDirty == newDirty, "Active fades changed pending/dirty state");
			Require(oldRoof == newRoof && oldCap == newCap && oldRoofBytes == newRoofBytes && oldCapBytes == newCapBytes,
				"Active fades differ from full sweep");
		}
		Require(!oldPending, "Fade test should finish fully settled");
		Require(activeVisits <= fullVisits, "Active fades increased cell visits");
		std::printf("PASS active shelter fades: %u cells, %llu visits vs %llu full, exact floats/bytes/dirty; sparse, dense, reset, reversal, pause\n",
			size, static_cast<unsigned long long>(activeVisits), static_cast<unsigned long long>(fullVisits));
	}
}

template <uint32_t Size>
__declspec(noinline) void FilterBefore(const uint8_t* src, uint8_t* dst, uint32_t* scratch, int radius)
{

	BoxFilter::detail::Sliding<Size>(src, dst, scratch, radius);
}
template <uint32_t Size>
__declspec(noinline) void FilterAfter(const uint8_t* src, uint8_t* dst, uint32_t* scratch, int radius)
{
	BoxFilter::Apply<Size>(src, dst, scratch, radius);
}
template <uint32_t Size>
void CheckBoxFilter(bool timing)
{
	std::vector<uint8_t> source(Size * Size), before(source.size()), after(source.size());
	std::vector<uint32_t> scratch(source.size());
	std::mt19937 rng(20260927);
	for (unsigned pattern = 0; pattern < 5; ++pattern) {
		for (size_t i = 0; i < source.size(); ++i) {
			source[i] = pattern == 0 ? 0 : pattern == 1 ? 255 : pattern == 2 ?
				static_cast<uint8_t>(rng()) : pattern == 3 ? ((i % Size + i / Size) % 2 ? 255 : 0) : 0;
		}
		if (pattern == 4) { source[0] = 255; source.back() = 131; }
		for (int radius = 0; radius <= static_cast<int>(Size / 4); ++radius) {
			FilterBefore<Size>(source.data(), before.data(), scratch.data(), radius);
			FilterAfter<Size>(source.data(), after.data(), scratch.data(), radius);
			Require(before == after, "Fixed filter differs from original sliding filter");

			for (uint32_t i = 0; i < source.size(); ++i) {
				if constexpr (Size > 16) {
					if (i != 0 && i != Size - 1 && i != Size && i != Size * Size - 1 && i != Size * Size / 2 + Size / 2) { continue; }
				}
				uint32_t sum = 0;
				for (int dy = -radius; dy <= radius; ++dy) {
					for (int dx = -radius; dx <= radius; ++dx) {
						const auto x = (i % Size + dx) & (Size - 1), y = (i / Size + dy) & (Size - 1);
						sum += source[y * Size + x];
					}
				}
				Require(after[i] == sum / ((2 * radius + 1) * (2 * radius + 1)), "Direct convolution disagrees at seam/interior");
			}
		}
	}
	std::printf("PASS box filter %ux%u: every radius 0..%u, five patterns, exact bytes and wrap checks\n", Size, Size, Size / 4);
	if (!timing) { return; }
	for (auto& value : source) { value = static_cast<uint8_t>(rng()); }
	for (int radius : {1, 2, 3}) {
		std::vector<double> times[2]; uint64_t checksum[2]{};
		for (unsigned round = 0; round < 11; ++round) {
			for (unsigned order = 0; order < 2; ++order) {
				const unsigned which = (round + order) % 2;
				const auto start = std::chrono::steady_clock::now();
				for (unsigned repeat = 0; repeat < 80; ++repeat) {
					if (which) { FilterAfter<Size>(source.data(), after.data(), scratch.data(), radius); }
					else { FilterBefore<Size>(source.data(), after.data(), scratch.data(), radius); }
					checksum[which] += after[(repeat * 7919u) % after.size()];
				}
				const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 80;
				if (round) { times[which].push_back(us); }
			}
		}
		Require(checksum[0] == checksum[1], "Filter benchmark output mismatch");
		for (auto& values : times) { std::sort(values.begin(), values.end()); }
		std::printf("FILTER CPU %u radius %d: sliding %.3f us -> fixed/fallback %.3f us (median, 10 rounds x 80, checksum %llu)\n",
			Size, radius, times[0][5], times[1][5], static_cast<unsigned long long>(checksum[0]));
	}
}

__declspec(noinline) float OldShelterLookup(const uint8_t* roof, int32_t cx, int32_t cy, int32_t x, int32_t y)
{
	if (!roof || std::abs(x - cx) >= 64 || std::abs(y - cy) >= 64) { return 0.0f; }
	return static_cast<float>(roof[(static_cast<uint32_t>(y) & 127) * 128 + (static_cast<uint32_t>(x) & 127)]) * (1.0f / 255.0f);
}
__declspec(noinline) void OldCoverageCombine(const uint8_t* src, uint8_t* dst, int32_t bx, int32_t by,
	const uint8_t* roof, int32_t cx, int32_t cy)
{
	for (uint32_t i = 0; i < 256 * 256; ++i) {
		const int32_t x = bx + static_cast<int32_t>(((i & 255) - static_cast<uint32_t>(bx)) & 255);
		const int32_t y = by + static_cast<int32_t>(((i / 256) - static_cast<uint32_t>(by)) & 255);
		const float open = 1.0f - OldShelterLookup(roof, cx, cy, x, y);
		dst[i] = static_cast<uint8_t>(static_cast<float>(src[i]) * open);
	}
}
void CheckCoverageCombine(bool timing)
{
	std::vector<uint8_t> src(256 * 256), roof(128 * 128), old(src.size()), result(src.size(), 173);
	std::mt19937 rng(20260927);
	for (auto& v : src) { v = static_cast<uint8_t>(rng()); }
	for (auto& v : roof) { v = static_cast<uint8_t>(rng()); }
	const int offsets[]{-512, -256, -192, -191, -128, -64, -1, 0, 1, 63, 64, 127, 128, 191, 192, 256, 512};
	unsigned cases = 0;
	for (int centre : {-1000000, -257, -128, -1, 0, 127, 256, 1000000}) {
		for (int dx : offsets) {
			for (int dy : offsets) {
				const int cx = centre + dx, cy = -centre + dy;
				const uint8_t* view = cases % 7 == 0 ? nullptr : roof.data();
				OldCoverageCombine(src.data(), old.data(), centre - 128, -centre - 128, view, cx, cy);
				const auto count = CoverageShelter::Combine<256,128>(src.data(), result.data(), centre - 128, -centre - 128, view, cx, cy);
				Require(old == result, "Coverage overlap changed map bytes");
				Require(count <= 127 * 127 && (view || count == 0), "Coverage overlap exceeded valid interior");
				++cases;
			}
		}
	}

	for (unsigned value = 0; value < 256; ++value) {
		std::fill(roof.begin(), roof.end(), static_cast<uint8_t>(value));
		for (size_t i = 0; i < src.size(); ++i) { src[i] = static_cast<uint8_t>((i % 256) + (i / 256) * 17); }
		if (value == 0) {
			std::array<bool, 256> seen{};
			for (int y = -63; y <= 63; ++y) {
				for (int x = -63; x <= 63; ++x) { seen[src[(static_cast<uint32_t>(y) & 255) * 256 + (static_cast<uint32_t>(x) & 255)]] = true; }
			}
			Require(std::all_of(seen.begin(), seen.end(), [](bool v) { return v; }), "Coverage test missed a byte value");
		}
		OldCoverageCombine(src.data(), old.data(), -128, -128, roof.data(), 0, 0);
		CoverageShelter::Combine<256,128>(src.data(), result.data(), -128, -128, roof.data(), 0, 0);
		Require(old == result, "Coverage scaling rounding differs");
	}
	std::printf("PASS coverage combine: %u moving/wrapped/negative/disabled windows, byte-pair rounding and stale-output replacement\n", cases);
	if (!timing) { return; }
	for (auto& v : src) { v = static_cast<uint8_t>(rng()); }
	for (auto& v : roof) { v = static_cast<uint8_t>(rng()); }
	for (int mode = 0; mode < 3; ++mode) {
		const auto* view = mode == 2 ? nullptr : roof.data();
		const int cx = mode == 1 ? 190 : 0;
		std::vector<double> times[2]; uint64_t sums[2]{};
		for (unsigned round = 0; round < 11; ++round) {
			for (unsigned order = 0; order < 2; ++order) {
				const unsigned which = (round + order) % 2;
				const auto start = std::chrono::steady_clock::now();
				for (unsigned k = 0; k < 80; ++k) {
					if (which) { CoverageShelter::Combine<256,128>(src.data(), result.data(), -128, -128, view, cx, 0); }
					else { OldCoverageCombine(src.data(), result.data(), -128, -128, view, cx, 0); }
					sums[which] += result[k * 7919 % result.size()];
				}
				const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 80;
				if (round) { times[which].push_back(us); }
			}
		}
		Require(sums[0] == sums[1], "Combine benchmark checksum mismatch");
		for (auto& v : times) { std::sort(v.begin(), v.end()); }
		std::printf("COMBINE CPU mode %d (full/edge/disabled): %.3f -> %.3f us, checksum %llu\n", mode,
			times[0][5], times[1][5], static_cast<unsigned long long>(sums[0]));
	}
}

int main(int argc, char** argv)
{
	try {
		const bool combineTiming = argc > 1 && std::string_view(argv[1]) == "--combine-bench";
		CheckCoverageCombine(combineTiming);
		if (combineTiming) { return 0; }
		const bool filterTiming = argc > 1 && std::string_view(argv[1]) == "--filter-bench";
		CheckBoxFilter<16>(false);
		CheckBoxFilter<128>(filterTiming);
		CheckBoxFilter<256>(filterTiming);
		if (filterTiming) { return 0; }
		float at30 = 255.0f, at144 = 255.0f;
		for (int i = 0; i < 30; ++i) { ShelterTransition::Advance(at30, 0, ShelterTransition::Alpha(1.0f / 30)); }
		for (int i = 0; i < 144; ++i) { ShelterTransition::Advance(at144, 0, ShelterTransition::Alpha(1.0f / 144)); }
		Require(std::abs(at30 - at144) < 0.001f, "Shelter transition must be frame-rate independent");
		Require(at30 > 0 && at30 < 20, "Shelter must approach the target without snapping");
		const float shelterBefore = at30;
		ShelterTransition::Advance(at30, 255, ShelterTransition::Alpha(0));
		Require(at30 == shelterBefore, "Paused shelter must not move");
		ShelterTransition::Advance(at30, 255, ShelterTransition::Alpha(1.0f / 60));
		Require(at30 > shelterBefore && at30 < 255, "Reversing a ray answer must not snap or overshoot");
		Require(ShelterTransition::Alpha(std::numeric_limits<float>::quiet_NaN()) == 0, "Invalid delta must be ignored");
		std::puts("PASS shelter transitions: frame rate, pause, reversal and invalid time");
		TestShelterScan();
		{
			for (const int fps : { 30, 60, 144, 300 }) {
				ShelterProbe::Clock clock;
				uint32_t refreshed = 0, polls = 0;
				for (int frame = 0; frame < fps * 10; ++frame) {
					const auto tick = clock.Advance(1.0f / fps, 16, 16384);
					refreshed += tick.cells; polls += tick.due;
				}
				Require(refreshed >= 9599 && refreshed <= 9601 && polls == 40, "Refresh rate must be 960 cells/s at every tested FPS");
				const auto before = clock.now;
				Require(!clock.Advance(0, 16, 16384).due && clock.now == before, "Pause does not age shelter");
				Require(!clock.Advance(-1, 16, 16384).due && !clock.Advance(std::numeric_limits<float>::quiet_NaN(), 16, 16384).due && clock.now == before, "Invalid time cannot age shelter");
			}
			ShelterProbe::Clock clock;
			Require(!clock.Advance(20, 16, 16384).due && std::abs(clock.now - 0.1) < 1e-6, "Long stalls are clamped instead of producing a refresh spike");
			ShelterProbe::Clock disabled;
			for (int i = 0; i < 15; ++i) {
				const auto tick = disabled.Advance(1.0f / 60, 0, 16384);
				Require(tick.cells == 0, "Zero refresh never invalidates successful roof results");
			}
			ShelterProbe::LandCell cell;
			float height = 123;
			Require(!cell.Get(0, 0, height), "Empty height cache cannot impersonate zero-coordinate terrain");
			cell.Store(-65, 129, 0);
			Require(cell.Get(-65, 129, height) && height == 0, "Zero-height terrain is a valid cached sample");
			Require(!cell.Get(63, 129, height), "Toroidal index reuse must not return a different coordinate's height");
			cell.Miss(63, 129, 1);
			Require(!cell.Get(63, 129, height) && !cell.CanRetry(63, 129, 1.49) && cell.CanRetry(63, 129, 1.5), "Failed land retries after 0.5s, not every frame or never");
			Require(cell.CanRetry(-65, 129, 1.1), "Movement to different coordinates bypasses old retry delay");
			cell.Store(63, 129, -20);
			Require(cell.Get(63, 129, height) && height == -20, "Late-loaded land replaces a failed cache entry");
			cell = {};
			Require(!cell.Get(63, 129, height), "World/reset clears heights");
			std::puts("PASS shelter probe cache: 30/60/144/300 FPS cadence, pauses, stalls, coordinate wrap, misses and reset");
		}
		{
			struct Location {
				bool city;
				Location* parentLoc;
				bool HasKeywordString(std::string_view key) const { return city && key == "LocTypeCity"; }
			};
			Location city{ true, nullptr }, shop{ false, &city }, wild{ false, nullptr };
			Location cycle{ false, nullptr }; cycle.parentLoc = &cycle;
			Require(TerrainActivity::IsCity(&city) && TerrainActivity::IsCity(&shop), "City and nested city location excluded");
			Require(!TerrainActivity::IsCity(&wild) && !TerrainActivity::IsCity(&cycle) && !TerrainActivity::IsCity<Location>(nullptr), "Wilderness, missing and cyclic location handling");
			struct World { World* parentWorld; Location* location; };
			World tamriel{ nullptr, &wild }, cityWorld{ &tamriel, &city }, exterior{ &tamriel, &wild };
			World sharedCityTagged{ nullptr, &city }, unknownWorld{ &tamriel, nullptr };
			Require(TerrainActivity::IsCityWorld(&cityWorld), "Dedicated city world idles");
			Require(!TerrainActivity::IsCityWorld(&tamriel) && !TerrainActivity::IsCityWorld(&sharedCityTagged), "Shared exterior remains active even with a city tag");
			Require(!TerrainActivity::IsCityWorld(&exterior) && !TerrainActivity::IsCityWorld(&unknownWorld) && !TerrainActivity::IsCityWorld<World>(nullptr), "Exterior and unknown world locations are not city exclusions");
			TerrainActivity::Policy policy;
			Require(!policy.Update(false, false, false, 0) && !policy.active, "Startup without loaded world idles");
			Require(policy.Update(true, false, false, 1) && policy.active, "First exterior frame clears stale state");
			Require(!policy.Update(true, false, false, 1) && policy.active, "Stable exterior retains trails");
			Require(!policy.Update(true, false, TerrainActivity::IsCityWorld(&tamriel), 1) && policy.active, "Windhelm exterior city ancestry does not disable shared landscape");
			Require(policy.Update(true, false, true, 1) && !policy.active, "Dedicated city world idles");
			Require(!policy.Update(true, false, true, 1), "Stable city does not repeat resets");
			Require(policy.Update(true, false, false, 1) && policy.active, "Leaving city resumes");
			Require(policy.Update(true, true, false, 0) && !policy.active, "Interior transition idles");
			Require(!policy.Update(true, true, false, 0), "Stable interior does not repeat resets");
			Require(policy.Update(true, false, false, 2) && policy.active, "New exterior world resumes");
			Require(policy.Update(true, false, false, 3) && policy.active, "Direct exterior world change clears old state");
			Require(policy.Update(false, false, false, 3) && !policy.active, "Unloaded 3D idles even with old world pointer");
			std::puts("PASS terrain activity: city ancestry, malformed parents, interiors, unloads, resume and world changes");
		}
		TestActiveShelterFades();
		Require(BloodDecalFilter::Matches("textures/effects/BloodSplatter01.DDS", " blood "), "Blood basename matching");
		Require(BloodDecalFilter::Matches("custom/EBT_pool.dds", "blood,ebt_"), "Custom blood prefix");
		Require(!BloodDecalFilter::Matches("blood/scorch.dds", "blood"), "Blood directory is not identification");
		Require(!BloodDecalFilter::Matches("blood.dds", " , "), "Empty prefixes match nothing");
		Require(!BloodDecalFilter::Matches("arrow.dds", "blood"), "Non-blood decal isolation");
		const auto groundBloodPrefixes = "blood,decalsblood,bigspatter";
		Require(BloodDecalFilter::Matches("SanguineSymphony/Effects/Default/BloodSprayGroundImpactRegular.dds", groundBloodPrefixes), "Sanguine ground spray");
		Require(BloodDecalFilter::Matches("Blood/DecalsBloodSplatterBlend01.dds", groundBloodPrefixes), "Native ground splatter");
		Require(BloodDecalFilter::Matches("blood/bigspatter01.dds", groundBloodPrefixes), "Rip n Tear splatter");
		Require(!BloodDecalFilter::Matches("ImpactDecals/DecalFlameBurn01.dds", groundBloodPrefixes), "Observed scorch must not become blood");
		Require(!BloodDecalFilter::Matches("SanguineSymphony/Blood/Default/BladeCutLarge.dds", groundBloodPrefixes), "Wound texture is not a ground spray");
		std::puts("PASS blood texture identification and non-blood isolation");
		Require(BloodDecalFilter::Matches("Effects/BLOODSpray.DDS", " , scorch,\tBLOOD \t,"), "Mixed case and trimmed blood prefixes");
		Require(!BloodDecalFilter::Matches("blood/stone.dds", "blood"), "Blood directory alone does not match");
		Require(!BloodDecalFilter::Matches("blood.dds.tmp", "blood"), "Blood extension must end the filename");
		Require(!BloodDecalFilter::Matches("blood.dds", " , \t,"), "Empty prefixes match nothing");
		Require(TerrainDepthBias::Fingerprint("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 standard vector");
        Require(TerrainDepthBias::Instructions("// header\r\nvs_5_0  \r\nret\r\n// footer\n") == "vs_5_0\nret\n",
            "Shader normalization excludes comments and normalizes whitespace");
        Require(TerrainDepthBias::Recognize("vs_5_0\nret\n") == 0, "Unknown shaders remain uncorrected");
        Require(TerrainDepthBias::Recognize("") == 0, "Missing shader remains uncorrected");
		{
			std::ifstream file(std::string(TERRAIN_TEST_FIXTURE_DIR) + "/cs191_offset_vs.asm", std::ios::binary);
			Require(file.is_open(), "CS 1.9.1 shader fixture opens");
			const std::string captured{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
			Require(TerrainDepthBias::Recognize(captured) == 5.0f, "CS 1.9.1 offset shader recognized");
			auto changed = captured;
			const auto offset = changed.find("5.000000");
			Require(offset != std::string::npos, "CS 1.9.1 fixture has offset");
			changed.replace(offset, 8, "6.000000");
			Require(TerrainDepthBias::Recognize(changed) == 0, "Unverified offset remains uncorrected");
			changed = captured;
			const auto matrix = changed.find("cb12[10]");
			Require(matrix != std::string::npos, "CS 1.9.1 fixture has projection row");
			changed.replace(matrix, 8, "cb12[11]");
			Require(TerrainDepthBias::Recognize(changed) == 0, "Unverified projection remains uncorrected");
			Require(TerrainDepthBias::Recognize("// header\n" + captured + "// footer\n") == 5.0f,
				"CS 1.9.1 comments do not change recognition");
		}

        if (argc > 1) {
            std::ifstream file(argv[1], std::ios::binary);
            Require(file.is_open(), "Local shader capture opens");
            const std::string captured{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            Require(TerrainDepthBias::Recognize(captured) == 10.0f, "Captured offset shader fingerprint matches");
            auto changed = captured;
            const auto offset = changed.find("10.000000");
            Require(offset != std::string::npos, "Local capture has expected offset");
            changed.replace(offset, 9, "5.000000");
            Require(TerrainDepthBias::Recognize(changed) == 5.0f, "Validated CS 1.9.1 offset also recognized");
            changed = captured;
            const auto matrix = changed.find("cb12[10]");
            Require(matrix != std::string::npos, "Local capture has expected matrix");
            changed.replace(matrix, 8, "cb12[20]");
            Require(TerrainDepthBias::Recognize(changed) == 0, "Different matrix remains uncorrected");
            Require(TerrainDepthBias::Recognize("// reflection\n" + captured + "// footer\n") == 10.0f,
                "Reflection comments do not change recognition");
            std::puts("PASS local terrain shader fingerprint and variant isolation");
        }

		for (double nearPlane : { 5.0, 10.0, 15.0 }) {
			const double farPlane = 10000.0, A = farPlane / (farPlane - nearPlane), B = -nearPlane * A;
			for (double z : { 30.0, 300.0, 3000.0 }) {
				const double clipZ = A * z + B, biasedZ = clipZ + 10.0;
				const double inverseW = ((biasedZ - 10.0) - A * z) / B;
				Require(std::abs(z / inverseW - z) < 0.00001, "Corrected world depth agrees with unbiased pass");
				Require(std::abs(123.0 / inverseW - 123.0) < 0.00001, "Corrected world XY agrees with unbiased pass");
				Require(std::abs(((biasedZ + A * 35.0) - (clipZ + A * 35.0)) - 10.0) < 0.00001, "Raster depth bias survives displacement");
			}
		}
		std::puts("PASS captured terrain depth shader isolation and biased projection reconstruction");
		Require(ObjectStampFilter::IsVisualBloodProjectile("SanguineSymphony\\Effects\\Default\\BloodSprayProjectileRegular.nif"), "Blood spray must not stamp snow");
		Require(ObjectStampFilter::IsVisualBloodProjectile("SanguineSymphony/Effects/Insect/BLOODSPRAYPROJECTILESUBTLE.NIF"), "Insect spray and case handling");
		Require(!ObjectStampFilter::IsVisualBloodProjectile("weapons/iron/ironarrow.nif"), "Arrow stamps preserved");
		Require(!ObjectStampFilter::IsVisualBloodProjectile("BloodSprayProjectileRegular/ironarrow.nif"), "Spray directory must not exclude solid projectile");
		Require(!ObjectStampFilter::IsVisualBloodProjectile("magic/fireball.nif"), "Unrelated projectiles preserved");
		Require(!ObjectStampFilter::IsVisualBloodProjectile(""), "Missing model keeps existing classification");
		std::puts("PASS visual blood projectile exclusion and ordinary projectile preservation");
		Field field;
		CheckDecayPrecision(field);
		field.CheckTessellationResources();
		field.CheckTerrainCulling();
		field.CheckBlanketBudget();
		CheckMagicPatterns(field);
		CheckGroundFloor(field);
		field.Clear();
		Params p;
		field.Step(p);
		const auto ordinary = field.Read();
		field.Clear(); p.stampMotion[0][2] = 1; field.Step(p);
		Require(field.Read() == ordinary, "Inherited snow values must match global geometry");
		Require(field.SnowAtOrigin() == 1.0f, "Snow classification must be written");
		std::puts("PASS inherited snow settings preserve global geometry");
		p.control[3] = 0; p.weather[3] = 0; p.rimShape[1] = 0;
		p.snowRim[0] = 1;
		field.Clear(); p.stampMotion[0][2] = 0; field.Step(p);
		const auto noRim = field.Read();
		Require(*std::max_element(noRim.begin(), noRim.end()) == 0, "Global zero span must suppress other-ground rim");
		field.Clear(); p.stampMotion[0][2] = 1; field.Step(p);
		const auto snowRim = field.Read();
		Require(*std::max_element(snowRim.begin(), snowRim.end()) > 0.1f, "Snow span must work with global span zero");
		std::puts("PASS independent snow rim span, including global span zero");
		field.Clear(); p.stampMotion[0][2] = 0; field.Step(p);
		p.snowRim[1] = 1; p.snowRim[2] = 1; p.snowRim[3] = 1;
		field.Clear(); field.Step(p);
		Require(field.Read() == noRim, "Snow noise/lean/churn leaked into non-snow stamp");
		std::puts("PASS snow noise/lean/churn do not alter other ground");
		for (int parameter = 1; parameter <= 3; ++parameter) {
			p = Params{};
			field.Clear(); field.Step(p);
			const auto baseline = field.Read();
			p.snowRim[parameter] = parameter == 2 ? 1.0f : 0.0f;
			field.Clear(); field.Step(p);
			Require(field.Read() == baseline, "Snow override changed a non-snow rim");
			p.stampMotion[0][2] = 1;
			field.Clear(); field.Step(p);
			Require(field.Read() != baseline, "Snow override has no effect on snow");
		}
		std::puts("PASS each noise/lean/churn override changes only snow with both rims active");
		p = Params{}; p.stampMotion[0][2] = 1;
		p.snowRim[0] = 1; p.snowRim[1] = 0; p.snowRim[2] = 1;
		field.Clear(); field.Step(p);
		Require(field.Read()[12] > 0.1f, "Outward lean rim was clipped by early bounds");
		std::puts("PASS wide outward rims survive the stamp bounds check");
		p = Params{}; p.stampMotion[0][2] = 1;
		field.Clear(); field.Step(p);
		const auto before = field.Read();
		p.control[1] = 0; p.weather[1] = 0.1f; p.weather[2] = 0; p.rimShape[2] = 1;
		field.Step(p);
		Require(field.Read() != before && field.SnowAtOrigin() == 1.0f,
			"Snow must keep its own repose rate after the stamp leaves");
		p = Params{}; field.Clear(); field.Step(p);
		const auto nonSnow = field.Read();
		p.control[1] = 0; p.weather[1] = 0.1f; p.weather[2] = 0; p.rimShape[2] = 1;
		field.Step(p);
		Require(field.Read() == nonSnow, "Snow repose rate affected old non-snow marks");
		std::puts("PASS persistent snow/non-snow repose isolation");
		p = Params{}; p.stampMotion[0][2] = 1;
		field.Clear(); field.Step(p); field.SaveCoarse(); field.Clear();
		p.control[1] = 0; p.coarse[0] = 1; p.coarse[3] = 0;
		field.Step(p, true);
		Require(field.SnowAtOrigin() == 1.0f && field.Read()[0] < 0,
			"Coarse seeding lost mark or snow classification");
		std::puts("PASS coarse-to-fine seeding retains snow classification");
		std::puts("ALL STAMP SURFACE TESTS PASS");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
