// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"
#include "SnowSparkle.h"

#include "Globals.h"
#include "Settings.h"

#include <chrono>
#include <cmath>
#include <cstring>

namespace SnowSparkle
{
	namespace
	{

		constexpr uint32_t kMaxFlakes = 4096;

		constexpr size_t kMaxContacts = 8;

		struct Flake
		{
			float x{}, y{}, z{};
			float vx{}, vy{}, vz{};
			float age{}, life{ 1.0f };
			float size{ 1.0f };
			float seed{};

			float floorZ{};
		};

		struct Contact
		{
			float x{}, y{}, z{};

			float forwardX{}, forwardY{ 1.0f };
			float velX{}, velY{};
			float radius{ 16.0f };
			float speed{};
		};

		std::vector<Flake> g_flakes;
		Contact            g_contacts[kMaxContacts]{};
		size_t             g_contactCount{ 0 };

		// A blast's contacts, kept apart from the footprint ones.
		//
		// g_contacts holds up to 8 points and is flushed every frame, which
		// is the right shape for a walking actor: two feet, a few samples.
		// A crater wants snow thrown from all the way round its lip at once,
		// which is more than 8 points and arrives in one frame, so folding
		// it into the same pool would mean the blast was silently truncated
		// to whatever the feet had not already used.  These live for one
		// frame too, but they are the ring, not the feet.
		constexpr size_t kMaxBursts = 4;
		constexpr size_t kMaxBurstPoints = 48;

		struct Burst
		{
			Contact points[kMaxBurstPoints]{};
			size_t  count{ 0 };

			float rate{ 0.0f };
		};

		Burst g_bursts[kMaxBursts]{};
		size_t g_burstCount{ 0 };

		float g_budget{ 0.0f };

		bool g_hookInstalled{ false };
		bool g_reportedCount{ false };
		bool g_reportedFull{ false };

		// Diagnostics for the spray gates.  Per-branch caps keep a running
		// player from filling the log with the same rejection.
		constexpr uint32_t kTapLimit = 20;
		uint32_t g_tapDenied{};
		uint32_t g_tapSlow{};
		uint32_t g_tapOk{};

		// Render-side accounting.  Update() proves flakes are born; these prove
		// whether they ever reach the draw call.
		uint64_t g_renderCalls{};
		uint64_t g_renderEmpty{};
		uint64_t g_renderDrawn{};
		uint64_t g_renderDrawnFlakes{};
		size_t   g_renderPeakFlakes{};
		uint64_t g_renderReported{};
		std::chrono::steady_clock::time_point g_renderLast{};

		uint32_t g_rng{ 0x9E3779B9u };

		float Random01()
		{
			g_rng ^= g_rng << 13;
			g_rng ^= g_rng >> 17;
			g_rng ^= g_rng << 5;
			return static_cast<float>(g_rng & 0xFFFFFFu) / static_cast<float>(0x1000000u);
		}

		float RandomSigned() { return Random01() * 2.0f - 1.0f; }

		ID3D11VertexShader*       g_vs{ nullptr };
		ID3D11PixelShader*        g_ps{ nullptr };
		ID3D11Buffer*             g_instanceBuffer{ nullptr };
		ID3D11ShaderResourceView* g_instanceSRV{ nullptr };
		ID3D11Buffer*             g_cb{ nullptr };
		ID3D11BlendState*         g_blend{ nullptr };
		ID3D11RasterizerState*    g_raster{ nullptr };
		ID3D11DepthStencilState*  g_depthLess{ nullptr };
		ID3D11DepthStencilState*  g_depthGreater{ nullptr };
		bool                      g_failed{ false };

		struct GpuFlake
		{
			float x{}, y{}, z{}, size{};
			float alpha{}, spin{}, sparkle{}, seed{};
		};
		static_assert(sizeof(GpuFlake) == 32);

		struct SparkleCB
		{
			float view[16]{};
			float proj[16]{};
			float posAdjust[4]{};

			float params[4]{};
		};

		const char* const kSparkleVS = R"(
cbuffer Sparkle : register(b0)
{
	row_major float4x4 View;
	row_major float4x4 Proj;
	float4 PosAdjust;
	float4 Params;
};

struct Flake
{
	float3 pos;
	float  size;
	float  alpha;
	float  spin;
	float  sparkle;
	float  seed;
};

StructuredBuffer<Flake> Flakes : register(t0);

struct VSOut
{
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
	nointerpolation float alpha   : TEXCOORD1;
	nointerpolation float spin    : TEXCOORD2;
	nointerpolation float sparkle : TEXCOORD3;
	nointerpolation float seed    : TEXCOORD5;

	nointerpolation float px : TEXCOORD4;
};

VSOut main(uint id : SV_VertexID)
{
	const uint index  = id / 6u;
	const uint corner = id % 6u;

	const uint2 lut[6] = { uint2(0,0), uint2(1,0), uint2(0,1),
	                       uint2(0,1), uint2(1,0), uint2(1,1) };
	const float2 c = float2(lut[corner]) * 2.0f - 1.0f;

	const Flake f = Flakes[index];

	VSOut o;
	o.uv = float2(lut[corner]);

	const float3 rel       = f.pos - PosAdjust.xyz;
	const float3 eyeCentre = mul(float4(rel, 1.0f), View).xyz;

	const float3 eyePos = eyeCentre + float3(c * f.size, 0.0f);
	o.pos = mul(float4(eyePos, 1.0f), Proj);

	o.alpha   = f.alpha;
	o.spin    = f.spin;
	o.sparkle = f.sparkle;
	o.seed    = f.seed;
	o.px      = o.pos.w > 0.001f ? (f.size * 2.0f * Proj._11 / o.pos.w) * Params.z * 0.5f : 0.0f;
	return o;
}
)";

		const char* const kSparklePS = R"(
cbuffer Sparkle : register(b0)
{
	row_major float4x4 View;
	row_major float4x4 Proj;
	float4 PosAdjust;
	float4 Params;
};

struct VSOut
{
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
	nointerpolation float alpha   : TEXCOORD1;
	nointerpolation float spin    : TEXCOORD2;
	nointerpolation float sparkle : TEXCOORD3;
	nointerpolation float seed    : TEXCOORD5;
	nointerpolation float px      : TEXCOORD4;
};

float4 main(VSOut i) : SV_TARGET
{
	const float2 d = i.uv * 2.0f - 1.0f;
	const float  r = length(d);
	if (r > 1.0f) {
		discard;
	}

	const float show = saturate((i.px - 6.0f) / 14.0f) * saturate(Params.x);
	const float ang  = atan2(d.y, d.x) + i.spin;

	float wob = 0.45f * sin(3.0f * ang + i.seed);
	wob += 0.32f * sin(5.0f * ang + i.seed * 2.7f);
	wob += 0.23f * sin(8.0f * ang + i.seed * 5.3f);

	const float lobes = 1.0f - show * 0.42f * (1.0f - wob) * 0.5f;
	const float mask  = saturate((lobes - r) * 3.0f);

	const float core = saturate(1.0f - r * r);
	const float a    = saturate(i.alpha * mask * (0.25f + 0.75f * core));

	const float3 tint = float3(0.94f, 0.97f, 1.0f) * Params.y * i.sparkle;
	return float4(tint, a);
}
)";

		bool Compile(const char* a_source, const char* a_target, ID3DBlob** a_out)
		{
			ID3DBlob*  errors = nullptr;
			const auto hr = D3DCompile(a_source, std::strlen(a_source), nullptr, nullptr,
				nullptr, "main", a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, a_out, &errors);
			if (FAILED(hr)) {
				logger::error("SnowSparkle: {} compile failed: {}", a_target,
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
				if (errors) {
					errors->Release();
				}
				return false;
			}
			if (errors) {
				errors->Release();
			}
			return true;
		}

		template <class T>
		void Drop(T*& a_object)
		{
			if (a_object) {
				a_object->Release();
				a_object = nullptr;
			}
		}

		struct GraphicsStateGuard
		{
			explicit GraphicsStateGuard(ID3D11DeviceContext* a_context) :
				_context(a_context)
			{
				_context->OMGetRenderTargets(8, _rtvs, &_dsv);
				_context->OMGetDepthStencilState(&_depthState, &_stencilRef);
				_context->OMGetBlendState(&_blendState, _blendFactor, &_sampleMask);
				_context->RSGetState(&_rasterState);
				_context->RSGetViewports(&_viewportCount, _viewports);
				_context->IAGetPrimitiveTopology(&_topology);
				_context->IAGetInputLayout(&_inputLayout);
				_context->VSGetShader(&_prevVS, nullptr, nullptr);
				_context->PSGetShader(&_prevPS, nullptr, nullptr);
				_context->GSGetShader(&_prevGS, nullptr, nullptr);
				_context->HSGetShader(&_prevHS, nullptr, nullptr);
				_context->DSGetShader(&_prevDS, nullptr, nullptr);
				_context->VSGetShaderResources(0, 1, &_vsSRV);
				_context->VSGetConstantBuffers(0, 1, &_vsCB);
				_context->PSGetConstantBuffers(0, 1, &_psCB);
			}

			~GraphicsStateGuard()
			{
				_context->OMSetRenderTargets(8, _rtvs, _dsv);
				_context->OMSetDepthStencilState(_depthState, _stencilRef);
				_context->OMSetBlendState(_blendState, _blendFactor, _sampleMask);
				_context->RSSetState(_rasterState);
				if (_viewportCount > 0) {
					_context->RSSetViewports(_viewportCount, _viewports);
				}
				_context->IASetPrimitiveTopology(_topology);
				_context->IASetInputLayout(_inputLayout);
				_context->VSSetShader(_prevVS, nullptr, 0);
				_context->PSSetShader(_prevPS, nullptr, 0);
				_context->GSSetShader(_prevGS, nullptr, 0);
				_context->HSSetShader(_prevHS, nullptr, 0);
				_context->DSSetShader(_prevDS, nullptr, 0);
				_context->VSSetShaderResources(0, 1, &_vsSRV);
				_context->VSSetConstantBuffers(0, 1, &_vsCB);
				_context->PSSetConstantBuffers(0, 1, &_psCB);

				for (auto*& rtv : _rtvs) {
					Drop(rtv);
				}
				Drop(_dsv);
				Drop(_depthState);
				Drop(_blendState);
				Drop(_rasterState);
				Drop(_inputLayout);
				Drop(_prevVS);
				Drop(_prevPS);
				Drop(_prevGS);
				Drop(_prevHS);
				Drop(_prevDS);
				Drop(_vsSRV);
				Drop(_vsCB);
				Drop(_psCB);
			}

			GraphicsStateGuard(const GraphicsStateGuard&) = delete;
			GraphicsStateGuard& operator=(const GraphicsStateGuard&) = delete;

		private:
			ID3D11DeviceContext*      _context{ nullptr };
			ID3D11RenderTargetView*   _rtvs[8]{};
			ID3D11DepthStencilView*   _dsv{ nullptr };
			ID3D11DepthStencilState*  _depthState{ nullptr };
			ID3D11BlendState*         _blendState{ nullptr };
			ID3D11RasterizerState*    _rasterState{ nullptr };
			ID3D11InputLayout*        _inputLayout{ nullptr };
			ID3D11VertexShader*       _prevVS{ nullptr };
			ID3D11PixelShader*        _prevPS{ nullptr };
			ID3D11GeometryShader*     _prevGS{ nullptr };
			ID3D11HullShader*         _prevHS{ nullptr };
			ID3D11DomainShader*       _prevDS{ nullptr };
			ID3D11ShaderResourceView* _vsSRV{ nullptr };
			ID3D11Buffer*             _vsCB{ nullptr };
			ID3D11Buffer*             _psCB{ nullptr };
			UINT                      _stencilRef{ 0 };
			FLOAT                     _blendFactor[4]{};
			UINT                      _sampleMask{ 0 };
			UINT _viewportCount{ D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE };
			D3D11_VIEWPORT
			_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
			D3D11_PRIMITIVE_TOPOLOGY _topology{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };
		};

		bool Create()
		{
			auto* device = globals::d3d::device;
			if (!device) {
				return false;
			}

			ID3DBlob* vsBlob = nullptr;
			ID3DBlob* psBlob = nullptr;
			if (!Compile(kSparkleVS, "vs_5_0", &vsBlob)) {
				return false;
			}
			if (!Compile(kSparklePS, "ps_5_0", &psBlob)) {
				vsBlob->Release();
				return false;
			}

			const bool ok = SUCCEEDED(device->CreateVertexShader(vsBlob->GetBufferPointer(),
								vsBlob->GetBufferSize(), nullptr, &g_vs)) &&
			                SUCCEEDED(device->CreatePixelShader(psBlob->GetBufferPointer(),
								psBlob->GetBufferSize(), nullptr, &g_ps));
			vsBlob->Release();
			psBlob->Release();
			if (!ok) {
				logger::error("SnowSparkle: shader creation failed");
				return false;
			}

			D3D11_BUFFER_DESC instDesc{};
			instDesc.ByteWidth = sizeof(GpuFlake) * kMaxFlakes;
			instDesc.Usage = D3D11_USAGE_DYNAMIC;
			instDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			instDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			instDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			instDesc.StructureByteStride = sizeof(GpuFlake);
			if (FAILED(device->CreateBuffer(&instDesc, nullptr, &g_instanceBuffer))) {
				logger::error("SnowSparkle: instance buffer creation failed");
				return false;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.FirstElement = 0;
			srvDesc.Buffer.NumElements = kMaxFlakes;
			if (FAILED(device->CreateShaderResourceView(g_instanceBuffer, &srvDesc,
					&g_instanceSRV))) {
				logger::error("SnowSparkle: instance SRV creation failed");
				return false;
			}

			D3D11_BUFFER_DESC cbDesc{};
			cbDesc.ByteWidth = sizeof(SparkleCB);
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &g_cb))) {
				logger::error("SnowSparkle: constant buffer creation failed");
				return false;
			}

			D3D11_BLEND_DESC blendDesc{};
			blendDesc.RenderTarget[0].BlendEnable = TRUE;
			blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(device->CreateBlendState(&blendDesc, &g_blend))) {
				logger::error("SnowSparkle: blend state creation failed");
				return false;
			}

			D3D11_RASTERIZER_DESC rasterDesc{};
			rasterDesc.FillMode = D3D11_FILL_SOLID;
			rasterDesc.CullMode = D3D11_CULL_NONE;
			rasterDesc.DepthClipEnable = TRUE;
			if (FAILED(device->CreateRasterizerState(&rasterDesc, &g_raster))) {
				logger::error("SnowSparkle: rasteriser state creation failed");
				return false;
			}

			D3D11_DEPTH_STENCIL_DESC depthDesc{};
			depthDesc.DepthEnable = TRUE;
			depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			depthDesc.StencilEnable = FALSE;
			depthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
			if (FAILED(device->CreateDepthStencilState(&depthDesc, &g_depthLess))) {
				logger::error("SnowSparkle: depth state creation failed");
				return false;
			}
			depthDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
			if (FAILED(device->CreateDepthStencilState(&depthDesc, &g_depthGreater))) {
				logger::error("SnowSparkle: depth state creation failed");
				return false;
			}

			logger::info("SnowSparkle ready (max {} flakes)", kMaxFlakes);
			return true;
		}

		bool Ready()
		{
			if (g_failed) {
				return false;
			}
			if (g_vs && g_ps && g_instanceSRV && g_cb && g_blend && g_raster && g_depthLess &&
				g_depthGreater) {
				return true;
			}
			if (!Create()) {
				g_failed = true;
				return false;
			}
			return true;
		}

		ID3D11DepthStencilState* DepthStateFor(float a_minDepth, float a_maxDepth)
		{
			return a_maxDepth >= a_minDepth ? g_depthLess : g_depthGreater;
		}
	}

	void NoteHookInstalled(bool a_installed) { g_hookInstalled = a_installed; }

	bool Enabled() { return Settings::snowSparkle && g_hookInstalled; }

	void NoteContact(Surfaces::Type a_surface, const RE::NiPoint3& a_at, float a_forwardX,
		float a_forwardY, float a_velX, float a_velY, float a_radius)
	{
		// Diagnostic taps on each gate.  Bounded per branch so a running player
		// cannot flood the log; every one of these is silent in normal
		// operation, so their presence pins the gate that is dropping the
		// spray.
		const bool tap = Settings::logContactSamples;

		if (!Enabled() || a_surface != Surfaces::Type::kSnow) {
			if (tap && g_tapDenied < kTapLimit) {
				++g_tapDenied;
				logger::info("Sparkle gate: rejected enabled={} hook={} surface={} (need snow)",
					Settings::snowSparkle, g_hookInstalled, static_cast<int>(a_surface));
			}
			return;
		}
		if (g_contactCount >= kMaxContacts || !std::isfinite(a_velX) || !std::isfinite(a_velY)) {
			if (tap && g_tapDenied < kTapLimit) {
				++g_tapDenied;
				logger::info("Sparkle gate: rejected contacts={}/{} velFinite={}",
					g_contactCount, kMaxContacts, std::isfinite(a_velX) && std::isfinite(a_velY));
			}
			return;
		}
		const float speed = std::hypot(a_velX, a_velY);
		if (speed < Settings::snowSparkleMinSpeed) {
			if (tap && g_tapSlow < kTapLimit) {
				++g_tapSlow;
				logger::info("Sparkle gate: too slow speed={:.2f} (need >= {:.2f}) vel=({:.2f},{:.2f})",
					speed, Settings::snowSparkleMinSpeed, a_velX, a_velY);
			}
			return;
		}
		if (tap && g_tapOk < kTapLimit) {
			++g_tapOk;
			logger::info("Sparkle gate: accepted speed={:.2f} at ({:.1f},{:.1f},{:.1f})",
				speed, a_at.x, a_at.y, a_at.z);
		}
		g_contacts[g_contactCount++] = Contact{ a_at.x, a_at.y, a_at.z, a_forwardX, a_forwardY,
			a_velX, a_velY, a_radius, speed };
	}

	void NoteBurst(Surfaces::Type a_surface, const RE::NiPoint3& a_at, float a_radius,
		float a_outwardSpeed, int a_points, float a_rate)
	{
		if (!Enabled() || a_surface != Surfaces::Type::kSnow) {
			return;
		}
		if (!std::isfinite(a_radius) || !std::isfinite(a_outwardSpeed) ||
			!std::isfinite(a_rate) || a_points <= 0 || a_radius <= 0.5f) {
			return;
		}
		if (g_burstCount >= kMaxBursts) {
			return;
		}

		Burst& burst = g_bursts[g_burstCount];
		burst.count = 0;
		burst.rate = std::max(a_rate, 0.0f);

		// The ring is walked rather than sampled at random so the spray
		// comes off the whole lip evenly.  Sampling a random point per
		// flake would clump: the flakes are drawn from these points, and
		// whichever ones happened to be picked would be seen as columns.
		const int points = std::min(a_points, static_cast<int>(kMaxBurstPoints));
		for (int i = 0; i < points; ++i) {
			const float th = 6.28318531f * static_cast<float>(i) /
				static_cast<float>(points);
			const float dx = std::cos(th), dy = std::sin(th);

			// Jittered radius per point, so the ring is a band of thrown
			// snow rather than a wire circle.  The jitter is deterministic
			// in the point index, so it does not shimmer frame to frame.
			const float wobble = 0.82f + 0.36f *
				(0.5f + 0.5f * std::sin(th * 5.0f + 1.7f));
			const float r = a_radius * wobble;

			Contact& c = burst.points[burst.count++];
			c.x = a_at.x + dx * r;
			c.y = a_at.y + dy * r;
			c.z = a_at.z;
			c.forwardX = dx;
			c.forwardY = dy;

			// Outward and slightly upward.  The speed is per-axis here
			// because Update() reads these as a horizontal velocity to
			// inherit, and the lip's snow travels away from the crater -
			// which is exactly what a fireball does to the snow it hits.
			c.velX = dx * a_outwardSpeed;
			c.velY = dy * a_outwardSpeed;
			c.radius = std::max(a_radius * 0.25f, 4.0f);

			// Set high so the throw is scaled up rather than down: Update()
			// clamps speed / reference into [0.35, 1.4] against
			// SnowSparkleFullSpeed, and a blast should sit at the top of
			// that range whatever a walk is doing.
			c.speed = std::max(a_outwardSpeed, 1.0f);
		}

		if (burst.count > 0) {
			++g_burstCount;
		}
	}

	void Update(float a_deltaSeconds)
	{
		if (!Enabled()) {
			g_flakes.clear();
			g_contactCount = 0;
			g_budget = 0.0f;
			return;
		}

		const float dt =
			std::isfinite(a_deltaSeconds) ? std::clamp(a_deltaSeconds, 0.0f, 0.1f) : 0.0f;

		if (dt > 0.0f && g_contactCount > 0) {
			float fastest = 0.0f;
			for (size_t i = 0; i < g_contactCount; ++i) {
				fastest = std::max(fastest, g_contacts[i].speed);
			}
			const float reference = std::max(Settings::snowSparkleFullSpeed, 1.0f);
			const float drive = std::clamp(fastest / reference, 0.0f, 1.5f);
			g_budget += Settings::snowSparkleRate * drive * dt;
		}

		// A blast's own budget, added on top.  It is spent as a lump rather
		// than as a rate over time on purpose: the throw happens on the
		// frame the spell lands and the spray should read as one burst that
		// then arcs and settles, not as a hose that keeps running.  The
		// flakes themselves already live SnowSparkleLife seconds and fall
		// under gravity, which is what turns one frame of spawning into a
		// visible shower.
		const float burstBudget = [&] {
			float total = 0.0f;
			for (size_t i = 0; i < g_burstCount; ++i) {
				total += g_bursts[i].rate;
			}
			return total;
		}();
		g_budget += burstBudget;

		const float gravity = std::max(Settings::snowSparkleGravity, 0.0f);
		const float life = std::max(Settings::snowSparkleLife, 0.05f);
		const float size = std::max(Settings::snowSparkleSize, 0.05f);
		const float reference = std::max(Settings::snowSparkleFullSpeed, 1.0f);

		int spawned = 0;
		while (g_budget >= 1.0f && (g_contactCount > 0 || g_burstCount > 0)) {
			g_budget -= 1.0f;
			if (g_flakes.size() >= kMaxFlakes) {
				g_budget = 0.0f;

				if (!g_reportedFull) {
					g_reportedFull = true;
					logger::warn(
						"SnowSparkle: the flake pool is full at {}. SnowSparkleRate {:.0f} "
						"with SnowSparkleLife {:.2f} asks for more than that, so the spray "
						"will not get any denser - lower the rate or the life, or accept "
						"this as the ceiling.",
						kMaxFlakes, Settings::snowSparkleRate, Settings::snowSparkleLife);
				}
				break;
			}

			// Choose a source.  When a blast is present the spray comes off
			// it, because a crater's throw is the louder event and should
			// own the frame it lands on; footprints keep the spray to
			// themselves on every other frame.
			const Contact* c = nullptr;
			bool fromBlast = false;
			if (g_burstCount > 0) {
				const auto& burst = g_bursts[static_cast<size_t>(
					Random01() * static_cast<float>(g_burstCount)) % g_burstCount];
				if (burst.count > 0) {
					c = &burst.points[static_cast<size_t>(
							   Random01() * static_cast<float>(burst.count)) %
						   burst.count];
					fromBlast = true;
				}
			}
			if (!c) {
				if (g_contactCount == 0) {
					break;
				}
				c = &g_contacts[static_cast<size_t>(
									Random01() * static_cast<float>(g_contactCount)) %
								g_contactCount];
			}

			const float speedNow = std::hypot(c->velX, c->velY);
			const float travelX = speedNow > 1.0f ? c->velX / speedNow : c->forwardX;
			const float travelY = speedNow > 1.0f ? c->velY / speedNow : c->forwardY;

			const float spread = RandomSigned();
			const float dirX = travelX + (-travelY) * spread * 0.9f;
			const float dirY = travelY + (travelX) * spread * 0.9f;
			const float len = std::hypot(dirX, dirY);
			const float ux = len > 0.001f ? dirX / len : 1.0f;
			const float uy = len > 0.001f ? dirY / len : 0.0f;

			const float throwSpeed = Settings::snowSparkleThrow *
									 (0.45f + 0.55f * Random01()) *
									 std::clamp(c->speed / reference, 0.35f, 1.4f);
			const float rise = Settings::snowSparkleRise * (0.5f + 0.7f * Random01());

			Flake f{};

			f.x = c->x + RandomSigned() * c->radius * 0.6f;
			f.y = c->y + RandomSigned() * c->radius * 0.6f;
			// Spawn above the contact point, not inside it.  The contact is
			// the actor's feet, so +2 buried the spray in the ground: this
			// layer draws after opaque geometry with LESS_EQUAL depth, so a
			// flake below the snow surface loses the depth test to the tile
			// it is sitting in and disappears until the camera rises over
			// the ground plane (which is why a jump is the only reliable way
			// to see a walk).  Lifting the spawn clear of the surface keeps
			// the flakes in front of the ground from the normal camera.
			//
			// A blast gets a taller offset than a foot does.  The lip is
			// already below the surrounding snow, so the same +6 that clears
			// level ground would still leave part of the spray inside the
			// crater wall for the frames the flakes spend rising.  The extra
			// height is scaled by the contact's radius, which for a blast is
			// a fraction of the crater's own radius - so a big crater throws
			// its snow from proportionally higher, which is also what reads
			// as a bigger blast.
			f.z = c->z + (fromBlast ? 6.0f + c->radius * 0.5f : 6.0f);

			const float inherit = std::clamp(Settings::snowSparkleInherit, 0.0f, 4.0f);
			f.vx = ux * throwSpeed + c->velX * inherit;
			f.vy = uy * throwSpeed + c->velY * inherit;
			f.vz = rise;
			f.age = 0.0f;
			f.life = life * (0.6f + 0.8f * Random01());
			f.size = size * (0.55f + 0.9f * Random01());
			f.seed = Random01() * 6.2831853f;
			f.floorZ = c->z - 4.0f;
			g_flakes.push_back(f);
			++spawned;
		}
		g_contactCount = 0;
		g_burstCount = 0;

		if (dt > 0.0f) {
			const float drag = 1.0f - std::exp(-std::max(Settings::snowSparkleDrag, 0.0f) * dt);
			const float wander = std::max(Settings::snowSparkleSwirl, 0.0f);
			for (size_t i = 0; i < g_flakes.size();) {
				auto& f = g_flakes[i];

				f.vx -= f.vx * drag;
				f.vy -= f.vy * drag;
				f.vz -= gravity * dt;

				if (wander > 0.0f) {
					f.vx += std::sin(f.age * 5.0f + f.seed) * wander * dt;
					f.vy += std::cos(f.age * 4.3f + f.seed * 1.7f) * wander * dt;
				}

				f.x += f.vx * dt;
				f.y += f.vy * dt;
				f.z += f.vz * dt;
				f.age += dt;

				if (f.age >= f.life || f.z < f.floorZ) {
					f = g_flakes.back();
					g_flakes.pop_back();
					continue;
				}
				++i;
			}
		}

		if (spawned > 0 && !g_reportedCount) {
			g_reportedCount = true;
			logger::info(
				"SnowSparkle: first flakes thrown; rate {:.1f}/s at {:.0f} units/s, size "
				"{:.1f}, life {:.2f}s",
				Settings::snowSparkleRate, Settings::snowSparkleFullSpeed, size, life);
		}
	}

	// One line per burst of render calls.  Update() proves flakes are created;
	// this proves whether they ever reach the draw call, and at what size.
	// A large g_renderCalls with a small g_renderDrawn means the draw is being
	// reached but the pool keeps draining before it can be seen.
	void ReportRender()
	{
		if (!Settings::logContactSamples) {
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		const bool first = (g_renderLast.time_since_epoch().count() == 0);
		const bool quiet = !first && (now - g_renderLast) > std::chrono::seconds(2);
		if (!first && !quiet && (g_renderCalls - g_renderReported) < 600) {
			return;
		}
		g_renderLast = now;
		g_renderReported = g_renderCalls;
		logger::info(
			"Sparkle render: calls={} empty={} drawn={} peakFlakes={} avgFlakes={:.1f} liveFlakes={} contacts={}",
			g_renderCalls, g_renderEmpty, g_renderDrawn, g_renderPeakFlakes,
			g_renderDrawn > 0 ? static_cast<float>(g_renderDrawnFlakes) /
				static_cast<float>(g_renderDrawn) : 0.0f,
			g_flakes.size(), g_contactCount);
	}

	void Render()
	{
		++g_renderCalls;
		if (!Enabled() || g_flakes.empty()) {
			++g_renderEmpty;
			ReportRender();
			return;
		}

		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto* shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
		auto* context = globals::d3d::context;
		if (!renderer || !shadowState || !context) {
			return;
		}

		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN];
		auto& depth =
			renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN];
		if (!main.RTV || !depth.views[0]) {
			return;
		}

		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
		UINT           viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		context->RSGetViewports(&viewportCount, viewports);
		if (viewportCount == 0) {
			return;
		}
		const float minDepth = viewports[0].MinDepth;
		const float maxDepth = viewports[0].MaxDepth;
		const auto  vpWidth = static_cast<uint32_t>(viewports[0].Width);
		const auto  vpHeight = static_cast<uint32_t>(viewports[0].Height);

		D3D11_TEXTURE2D_DESC mainDesc{};
		if (main.texture) {
			main.texture->GetDesc(&mainDesc);
		}
		if (mainDesc.Width == 0 || vpWidth * 2 < mainDesc.Width ||
			vpHeight * 2 < mainDesc.Height) {
			return;
		}

		if (!Ready()) {
			return;
		}

		const auto& view = shadowState->GetRuntimeData().cameraData.getEye();
		const auto& adjust = shadowState->GetRuntimeData().posAdjust.getEye();

		float ambient = 1.0f;
		if (auto* sky = RE::Sky::GetSingleton()) {
			const auto& c = sky->skyColor[RE::TESWeather::ColorTypes::kAmbient];
			ambient = std::clamp((c.red + c.green + c.blue) / 3.0f * 2.0f, 0.25f, 1.0f);
		}

		static std::vector<GpuFlake> upload;
		upload.clear();
		upload.reserve(g_flakes.size());
		for (const auto& f : g_flakes) {
			const float t = f.life > 0.0f ? std::clamp(f.age / f.life, 0.0f, 1.0f) : 1.0f;

			// Fade in over the first 12% of life, then out linearly.  The
			// curve used to be min(t / 0.12, 1) * (1 - t)^2, which squares
			// the falloff: by half a lifetime a flake was already down to
			// 0.25 alpha and by 70% it sat at 0.09, so a burst read as a
			// brief flash that dissolved into mist rather than as solid
			// snow.  A linear tail keeps the flake solid for most of its
			// life and still removes it cleanly at the end, which is what
			// makes a kick of snow look like grains instead of smoke.
			const float fade = std::min(t / 0.12f, 1.0f) * (1.0f - t);

			GpuFlake g{};
			g.x = f.x;
			g.y = f.y;
			g.z = f.z;
			g.size = f.size;
			g.alpha = std::clamp(fade * Settings::snowSparkleOpacity, 0.0f, 1.0f);
			g.spin = f.seed + f.age * 2.0f;
			g.seed = f.seed;
			g.sparkle =
				0.75f + 0.45f * (0.5f + 0.5f * std::sin(f.age * 11.0f + f.seed * 3.1f));
			upload.push_back(g);
		}
		if (upload.empty()) {
			return;
		}

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(g_instanceBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return;
		}
		std::memcpy(mapped.pData, upload.data(), upload.size() * sizeof(GpuFlake));
		context->Unmap(g_instanceBuffer, 0);

		SparkleCB cb{};
		std::memcpy(cb.view, view.viewMat.m, sizeof(cb.view));
		std::memcpy(cb.proj, view.projMat.m, sizeof(cb.proj));
		cb.posAdjust[0] = adjust.x;
		cb.posAdjust[1] = adjust.y;
		cb.posAdjust[2] = adjust.z;
		cb.params[0] = Settings::snowSparkleShape;
		cb.params[1] = Settings::snowSparkleBrightness * ambient;
		cb.params[2] = static_cast<float>(vpWidth);

		if (FAILED(context->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return;
		}
		std::memcpy(mapped.pData, &cb, sizeof(cb));
		context->Unmap(g_cb, 0);

		const GraphicsStateGuard guard(context);

		context->OMSetRenderTargets(1, &main.RTV, depth.views[0]);
		context->OMSetDepthStencilState(DepthStateFor(minDepth, maxDepth), 0);
		const FLOAT blendFactor[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
		context->OMSetBlendState(g_blend, blendFactor, 0xFFFFFFFF);
		context->RSSetState(g_raster);

		context->GSSetShader(nullptr, nullptr, 0);
		context->HSSetShader(nullptr, nullptr, 0);
		context->DSSetShader(nullptr, nullptr, 0);

		context->IASetInputLayout(nullptr);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		context->VSSetShader(g_vs, nullptr, 0);
		context->PSSetShader(g_ps, nullptr, 0);
		context->VSSetShaderResources(0, 1, &g_instanceSRV);
		context->VSSetConstantBuffers(0, 1, &g_cb);
		context->PSSetConstantBuffers(0, 1, &g_cb);

		context->Draw(static_cast<UINT>(upload.size()) * 6u, 0);
		++g_renderDrawn;
		g_renderDrawnFlakes += upload.size();
		if (upload.size() > g_renderPeakFlakes) {
			g_renderPeakFlakes = upload.size();
		}
		ReportRender();
	}

	void Reset()
	{
		Release();
		g_failed = false;
		g_reportedCount = false;
		g_reportedFull = false;
	}

	void Release()
	{
		g_flakes.clear();
		g_contactCount = 0;
		g_burstCount = 0;
		g_budget = 0.0f;
		Drop(g_vs);
		Drop(g_ps);
		Drop(g_instanceSRV);
		Drop(g_instanceBuffer);
		Drop(g_cb);
		Drop(g_blend);
		Drop(g_raster);
		Drop(g_depthLess);
		Drop(g_depthGreater);
	}
}
