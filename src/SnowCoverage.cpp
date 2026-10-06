// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"
#include "BoxFilter.h"
#include "CoverageWorkQueue.h"
#include <chrono>

#include "Globals.h"
#include "Profiler.h"
#include "Settings.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "SurfaceTypes.h"
#include "SurfaceProfiles.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace SnowCoverage
{
	namespace
	{
		constexpr uint32_t kMask = kTexels - 1;
		static_assert((kTexels & kMask) == 0, "kTexels must be a power of two for the "
											  "toroidal index to be a bitmask");

		ID3D11Texture2D*          g_texture{ nullptr };
		ID3D11ShaderResourceView* g_srv{ nullptr };
		ID3D11SamplerState*       g_sampler{ nullptr };
		bool                      g_failed{ false };

		std::vector<uint8_t> g_coverage;

		std::vector<uint8_t>  g_smooth;
		std::vector<uint32_t> g_rowSums;

		std::vector<uint8_t> g_upload;

		uint32_t g_shelterRevision{ 0 };

		std::vector<int32_t> g_filledX;
		std::vector<int32_t> g_filledY;

		CoverageWorkQueue::Grid<kTexels> g_work;
		std::vector<double> g_retryAt;
		double g_retryTime{};

		int32_t g_centreCellX{ 0 };
		int32_t g_centreCellY{ 0 };

		bool    g_dirty{ false };
		bool    g_everFilled{ false };

		bool g_reportedComplete{ false };

		bool Fail(const char* a_why)
		{
			logger::error("SnowCoverage: {}", a_why);
			g_failed = true;
			Shutdown();
			return false;
		}
	}

	bool Ready()
	{
		return g_srv && g_sampler;
	}

	bool Initialize()
	{
		if (g_failed || Ready()) {
			return Ready();
		}

		auto* device = globals::d3d::device;
		if (!device) {
			return Fail("no device");
		}

		g_coverage.assign(static_cast<size_t>(kTexels) * kTexels, 0);
		g_work.Reset();
		g_retryAt.assign(g_coverage.size(), 0.0);
		g_retryTime = 0.0;
		g_smooth.assign(g_coverage.size(), 0);
		g_upload.assign(g_coverage.size(), 0);
		g_rowSums.assign(g_coverage.size(), 0);
		g_shelterRevision = 0;

		g_filledX.assign(g_coverage.size(), -0x40000000);
		g_filledY.assign(g_coverage.size(), -0x40000000);

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kTexels;
		desc.Height = kTexels;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

		D3D11_SUBRESOURCE_DATA initial{};
		initial.pSysMem = g_upload.data();
		initial.SysMemPitch = kTexels * sizeof(uint8_t);

		if (FAILED(device->CreateTexture2D(&desc, &initial, &g_texture))) {
			return Fail("CreateTexture2D failed");
		}
		if (FAILED(device->CreateShaderResourceView(g_texture, nullptr, &g_srv))) {
			return Fail("CreateShaderResourceView failed");
		}

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

		if (FAILED(device->CreateSamplerState(&samplerDesc, &g_sampler))) {
			return Fail("CreateSamplerState failed");
		}

		logger::info("SnowCoverage ready: {}x{} texels over {} world units ({} per texel); profile-aware raise",
			kTexels, kTexels, kWorldSize, kTexelSize);
		return true;
	}

	void Shutdown()
	{
		const auto drop = [](auto*& a_ptr) {
			if (a_ptr) {
				a_ptr->Release();
				a_ptr = nullptr;
			}
		};
		drop(g_sampler);
		drop(g_srv);
		drop(g_texture);
	}

	void Reset()
	{
		std::fill(g_filledX.begin(), g_filledX.end(), -0x40000000);
		std::fill(g_filledY.begin(), g_filledY.end(), -0x40000000);
		g_work.Reset();
		std::fill(g_retryAt.begin(), g_retryAt.end(), 0.0);
		g_retryTime = 0.0;
		g_everFilled = false;
		g_reportedComplete = false;
	}

	void ForgetWindow()
	{
		Reset();
		std::fill(g_coverage.begin(), g_coverage.end(), 0);
		std::fill(g_smooth.begin(), g_smooth.end(), 0);
		std::fill(g_upload.begin(), g_upload.end(), 0);
		g_dirty = true;
	}

	namespace
	{

		void Smooth()
		{
			const int n = static_cast<int>(kTexels);
			const int r = std::clamp(Settings::snowSeamTexels, 0, n / 4);
			if (r <= 0) {
				g_smooth = g_coverage;
				return;
			}

			const auto started = Profiler::Ticks();
			BoxFilter::Apply<kTexels>(g_coverage.data(), g_smooth.data(), g_rowSums.data(), r);
			Profiler::AddCpuTicks(Profiler::CpuScope::kCoverageFilter, Profiler::Ticks() - started);
		}
	}

	namespace
	{

		void Combine(int32_t a_baseX, int32_t a_baseY)
		{
			const auto start = Profiler::Ticks();
			const auto scaled = Shelter::CombineCoverage(g_smooth.data(), g_upload.data(), a_baseX, a_baseY);
			Profiler::AddCpuTicks(Profiler::CpuScope::kCoverageCombine, Profiler::Ticks() - start);
			Profiler::Tally(Profiler::Count::kCoverageScaled, scaled);
			Profiler::Tally(Profiler::Count::kCoverageCombineFull, kTexels * kTexels);
		}
	}

	void Update()
	{
		if (!Settings::enableSnowRaise) {
			return;
		}

		if (!Ready() && !Initialize()) {
			return;
		}

		auto* player = globals::game::player;
		auto* context = globals::d3d::context;
		if (!player || !context) {
			return;
		}

		const int64_t started = Profiler::Ticks();

		const RE::NiPoint3 position = player->GetPosition();

		g_centreCellX = static_cast<int32_t>(std::floor(position.x / kTexelSize));
		g_centreCellY = static_cast<int32_t>(std::floor(position.y / kTexelSize));

		const int32_t baseX = g_centreCellX - static_cast<int32_t>(kTexels / 2);
		const int32_t baseY = g_centreCellY - static_cast<int32_t>(kTexels / 2);

		const uint32_t shelterRevision = Shelter::Revision();
		const bool     shelterMoved = shelterRevision != g_shelterRevision;
		const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
		const float delta = !paused && globals::game::deltaTime ? *globals::game::deltaTime : 0.0f;
		if (std::isfinite(delta)) { g_retryTime += std::clamp(delta, 0.0f, 0.1f); }
		g_work.Move(g_centreCellX, g_centreCellY, [](uint32_t index) { g_retryAt[index] = 0.0; });
		const bool settledHere = g_work.Count() == 0;

		if (!g_dirty && !shelterMoved && settledHere) {
			Profiler::AddCpuTicks(Profiler::CpuScope::kSnowCoverage,
				Profiler::Ticks() - started);
			return;
		}

		const uint32_t budget = static_cast<uint32_t>(
			std::max(Settings::snowCoverageBudget, 1));

		uint32_t       queries = 0;
		uint32_t       scanned = 0;

		const uint32_t scanLimit = std::min(g_work.Count(), budget);
		auto* tes = RE::TES::GetSingleton();
		uint32_t misses = 0;
		while (scanned < scanLimit) {
			const uint32_t index = g_work.Pop();
			++scanned;

			const uint32_t tx = index & kMask;
			const uint32_t ty = index / kTexels;

			const int32_t cellX =
				baseX + static_cast<int32_t>((tx - static_cast<uint32_t>(baseX)) & kMask);
			const int32_t cellY =
				baseY + static_cast<int32_t>((ty - static_cast<uint32_t>(baseY)) & kMask);

			if (g_filledX[index] == cellX && g_filledY[index] == cellY) {
				continue;
			}

			if (g_retryTime < g_retryAt[index]) { g_work.Push(index); continue; }
			RE::NiPoint3 probe{
				(static_cast<float>(cellX) + 0.5f) * kTexelSize,
				(static_cast<float>(cellY) + 0.5f) * kTexelSize,
				position.z
			};

			++queries;
			float landZ = 0.0f;
			if (!tes || !tes->GetLandHeight(probe, landZ)) {
				++misses;
				g_retryAt[index] = g_retryTime + 0.5;
				g_work.Push(index);
				continue;
			}

			probe.z = landZ;
			const auto ground = Surfaces::GroundAt(probe);
			g_coverage[index] = ground.type == Surfaces::Type::kSnow ? 255 : 0;
			g_filledX[index] = cellX;
			g_filledY[index] = cellY;
			g_dirty = true;
		}

		Profiler::Tally(Profiler::Count::kCoverageLandQueries, queries);
		Profiler::Tally(Profiler::Count::kCoverageLandMisses, misses);
		Profiler::Tally(Profiler::Count::kCoveragePending, g_work.Count());
		const bool complete = g_work.Count() == 0;
		const bool firstComplete = complete && !g_everFilled;
		if (complete) { g_everFilled = true; }

		if (g_dirty || shelterMoved) {

			if (g_dirty) {
				Smooth();
			}

			Combine(baseX, baseY);

			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(g_texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {

				auto* dst = static_cast<uint8_t*>(mapped.pData);
				for (uint32_t y = 0; y < kTexels; ++y) {
					std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
						g_upload.data() + static_cast<size_t>(y) * kTexels, kTexels);
				}
				context->Unmap(g_texture, 0);
				g_dirty = false;

				g_shelterRevision = shelterRevision;
			}
		}

		Profiler::AddCpuTicks(Profiler::CpuScope::kSnowCoverage,
			Profiler::Ticks() - started);

		if (!Settings::logSnowCoverage) {
			return;
		}

		using Clock = std::chrono::steady_clock;
		static Clock::time_point backlogSince{};
		static bool warned{};
		if (complete) {
			if (firstComplete || warned) {
				logger::info("SnowCoverage: swept clean at {} units a texel", kTexelSize);
			}
			g_reportedComplete = true;
			g_everFilled = true;
			backlogSince = {};
			warned = false;
		} else {
			g_reportedComplete = false;
			const auto now = Clock::now();
			if (backlogSince == Clock::time_point{}) { backlogSince = now; }
			if (!warned && now - backlogSince > std::chrono::seconds(2)) {
				warned = true;
				logger::info("SnowCoverage: still catching up after 2 seconds (budget {})", budget);
			}
		}
	}

	float At(float a_worldX, float a_worldY)
	{

		if (!Settings::enableSnowRaise || !g_everFilled || g_upload.empty()) {
			return 1.0f;
		}

		const float u = (a_worldX / kWorldSize) * static_cast<float>(kTexels) - 0.5f;
		const float v = (a_worldY / kWorldSize) * static_cast<float>(kTexels) - 0.5f;

		const float u0f = std::floor(u);
		const float v0f = std::floor(v);

		const auto warp = [](float a_f) { return a_f * a_f * (3.0f - 2.0f * a_f); };

		const float fx = warp(u - u0f);
		const float fy = warp(v - v0f);

		const uint32_t x0 = static_cast<uint32_t>(static_cast<int32_t>(u0f)) & kMask;
		const uint32_t y0 = static_cast<uint32_t>(static_cast<int32_t>(v0f)) & kMask;
		const uint32_t x1 = (x0 + 1) & kMask;
		const uint32_t y1 = (y0 + 1) & kMask;

		const auto texel = [](uint32_t a_x, uint32_t a_y) {
			return static_cast<float>(
					   g_upload[(static_cast<size_t>(a_y) * kTexels) + a_x]) /
			       255.0f;
		};

		const float top = texel(x0, y0) + (texel(x1, y0) - texel(x0, y0)) * fx;
		const float bottom = texel(x0, y1) + (texel(x1, y1) - texel(x0, y1)) * fx;
		return top + (bottom - top) * fy;
	}

	ID3D11ShaderResourceView* View()
	{
		return Ready() ? g_srv : nullptr;
	}

	void BindDomain(ID3D11DeviceContext* a_context)
	{
		if (!a_context || !Ready()) {
			return;
		}
		a_context->DSSetShaderResources(kSlot, 1, &g_srv);
		a_context->DSSetSamplers(kSamplerSlot, 1, &g_sampler);
	}

	void UnbindDomain(ID3D11DeviceContext* a_context)
	{
		if (!a_context) {
			return;
		}
		ID3D11ShaderResourceView* nullSRV = nullptr;
		ID3D11SamplerState*       nullSampler = nullptr;
		a_context->DSSetShaderResources(kSlot, 1, &nullSRV);
		a_context->DSSetSamplers(kSamplerSlot, 1, &nullSampler);
	}
}
