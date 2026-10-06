// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "ClipmapUpdateCS.h"
#include "ShaderReflection.cpp"
#include "Tessellation.cpp"

namespace ShaderRegistry
{
	Bytecode For(ID3D11VertexShader*) { return {}; }
}
#include "UtilityRouting.h"

#include <filesystem>
#include <fstream>

namespace
{
	Reflection::SignatureElement Element(
		const char* a_semantic, uint32_t a_index, uint32_t a_reg, const char* a_type)
	{
		Reflection::SignatureElement e{};
		e.semanticName = a_semantic;
		e.semanticIndex = a_index;
		e.registerIndex = a_reg;
		e.hlslType = a_type;
		e.mask = std::string_view(a_type) == "float4" ? 0x0F :
		         std::string_view(a_type) == "float3" ? 0x07 :
		         std::string_view(a_type) == "float2" ? 0x03 :
		                                                0x01;
		e.componentType = 3;
		return e;
	}

	Reflection::Signature ColourSignature()
	{
		Reflection::Signature s{};
		s.valid = true;
		s.elements = {
			Element("SV_POSITION", 0, 0, "float4"),
			Element("TEXCOORD", 0, 1, "float4"),
			Element("TEXCOORD", 4, 2, "float3"),
			Element("TEXCOORD", 1, 3, "float3"),
			Element("TEXCOORD", 2, 4, "float3"),
			Element("TEXCOORD", 3, 5, "float3"),
			Element("TEXCOORD", 5, 6, "float3"),
			Element("TEXCOORD", 6, 7, "float4"),
			Element("TEXCOORD", 7, 8, "float4"),
			Element("TEXCOORD", 8, 9, "float3"),
			Element("TEXCOORD", 9, 10, "float3"),
			Element("TEXCOORD", 10, 11, "float3"),
			Element("POSITION", 1, 12, "float4"),
			Element("POSITION", 2, 13, "float4"),
			Element("COLOR", 0, 14, "float4"),
			Element("COLOR", 1, 15, "float4"),
		};
		return s;
	}

	Reflection::Signature DepthPrepassSignature()
	{
		Reflection::Signature s{};
		s.valid = true;
		s.elements = { Element("SV_POSITION", 0, 0, "float4") };
		return s;
	}

	Reflection::Signature ShadowMapSignature()
	{
		Reflection::Signature s{};
		s.valid = true;
		s.elements = {
			Element("SV_POSITION", 0, 0, "float4"),
			Element("TEXCOORD", 2, 1, "float3"),
		};
		return s;
	}

	int g_failures = 0;

	void Check(const std::string& a_source, const char* a_target, const std::string& a_label,
		const std::filesystem::path& a_outDir)
	{
		const auto path = a_outDir / (a_label + ".hlsl");
		std::ofstream(path) << a_source;

		ID3DBlob* code = nullptr;
		ID3DBlob* errors = nullptr;

		const HRESULT hr = D3DCompile(a_source.c_str(), a_source.size(), a_label.c_str(),
			nullptr, nullptr, "main", a_target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);

		if (SUCCEEDED(hr)) {
			std::printf("  PASS  %-28s %-7s %6zu bytes -> %s\n", a_label.c_str(), a_target,
				code->GetBufferSize(), path.string().c_str());
			ID3DBlob* assembly = nullptr;
			if (SUCCEEDED(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(),
				0, nullptr, &assembly))) {
				std::ofstream(a_outDir / (a_label + ".asm"))
					.write(static_cast<const char*>(assembly->GetBufferPointer()),
						static_cast<std::streamsize>(assembly->GetBufferSize() - 1));
				assembly->Release();
			}
		} else {
			++g_failures;
			std::printf("  FAIL  %-28s %-7s hr=0x%08X -> %s\n", a_label.c_str(), a_target,
				static_cast<uint32_t>(hr), path.string().c_str());
			if (errors) {
				std::printf("%.*s\n", static_cast<int>(errors->GetBufferSize()),
					static_cast<const char*>(errors->GetBufferPointer()));
			}
		}

		if (code) {
			code->Release();
		}
		if (errors) {
			errors->Release();
		}
	}

	void GenerateCompute(const std::filesystem::path& a_outDir)
	{
		const std::string maxStamps = std::to_string(Clipmap::kMaxStamps);
		const D3D_SHADER_MACRO defines[] = {
			{ "MAX_STAMPS", maxStamps.c_str() },
			{ nullptr, nullptr }
		};

		const std::string source(Clipmap::UpdateShaderSource());
		const auto        path = a_outDir / "clipmap_update.hlsl";
		std::ofstream(path) << source;

		ID3DBlob* code = nullptr;
		ID3DBlob* errors = nullptr;

		const HRESULT hr = D3DCompile(source.c_str(), source.size(), "ClipmapUpdateCS",
			defines, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
			&errors);

		if (SUCCEEDED(hr)) {
			std::printf("  PASS  %-28s %-7s %6zu bytes -> %s\n", "clipmap_update",
				"cs_5_0", code->GetBufferSize(), path.string().c_str());
		} else {
			++g_failures;
			std::printf("  FAIL  %-28s %-7s hr=0x%08X -> %s\n", "clipmap_update",
				"cs_5_0", static_cast<uint32_t>(hr), path.string().c_str());
			if (errors) {
				std::printf("%.*s\n", static_cast<int>(errors->GetBufferSize()),
					static_cast<const char*>(errors->GetBufferPointer()));
			}
		}

		if (code) {
			code->Release();
		}
		if (errors) {
			errors->Release();
		}
	}

	void Generate(const char* a_name, const Reflection::Signature& a_signature,
		const std::filesystem::path& a_outDir,
		Tessellation::Mode a_mode = Tessellation::Mode::kLandscape)
	{

		const bool displace = Tessellation::WantsDisplacement(a_mode);
		const bool tessellate = Tessellation::WantsSubdivision(a_mode);

		const std::string common = Tessellation::EmitStruct(a_signature) +
		                           Tessellation::EmitPrologue(displace, a_mode) +
		                           Tessellation::EmitPatchConstants(tessellate);

		Check(common + Tessellation::EmitHull(Settings::tessellationWinding), "hs_5_0",
			std::string(a_name) + "_hs", a_outDir);
		Check(common + Tessellation::EmitDomain(a_signature, displace, a_mode), "ds_5_0",
			std::string(a_name) + "_ds", a_outDir);
	}
}

void CheckAsyncCompiler()
{
    const auto signature = ColourSignature();
    const bool displace = Tessellation::WantsDisplacement(Tessellation::Mode::kLandscape);
    const auto common = Tessellation::EmitStruct(signature) +
        Tessellation::EmitPrologue(displace, Tessellation::Mode::kLandscape) +
        Tessellation::EmitPatchConstants(true);
    ShaderCompiler::Worker worker;
    auto job = std::make_shared<ShaderCompiler::Job>();
    job->hullSource = common + Tessellation::EmitHull(Settings::tessellationWinding);
    job->domainSource = common + Tessellation::EmitDomain(signature, displace, Tessellation::Mode::kLandscape);
    worker.Submit(job);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!job->ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!job->ready.load(std::memory_order_acquire) || !job->hull || !job->domain || !job->error.empty()) {
        ++g_failures;
        std::puts("FAIL asynchronous compiler publication");
    } else {
        std::printf("PASS asynchronous compiler publication (%.2f ms off caller thread)\n", job->milliseconds);
    }
    auto invalid = std::make_shared<ShaderCompiler::Job>();
    invalid->hullSource = "invalid shader";
    worker.Submit(invalid);
    const auto failureDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!invalid->ready.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < failureDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!invalid->ready.load(std::memory_order_acquire) || invalid->error.empty()) {
        ++g_failures;
        std::puts("FAIL asynchronous compile failure reporting");
    } else {
        std::puts("PASS asynchronous compile failure reporting");
    }
    worker.CancelQueued();
}

int main(int a_argc, char** a_argv)
{
	const std::filesystem::path outDir = a_argc > 1 ? a_argv[1] : "generated_shaders";
	std::filesystem::create_directories(outDir);

	Settings::enableTessellation = true;
	Settings::useClipmap = true;
	Settings::recomputeNormals = true;
	Settings::enableSurfaceMaterial = true;

	std::printf("Clipmap update pass\n");
	GenerateCompute(outDir);

	std::printf("\nDefault configuration (clipmap, normals, surface material)\n");
	Generate("colour", ColourSignature(), outDir);
	Generate("depth_prepass", DepthPrepassSignature(), outDir);
	Settings::terrainBlendingCompatibility = true;
	Generate("tb_depth_correction", DepthPrepassSignature(), outDir);
	Generate("tb_colour_correction", ColourSignature(), outDir);
	Settings::terrainBlendingCompatibility = false;
	Generate("shadow_map", ShadowMapSignature(), outDir);

	std::printf("\nSnow raise (all three passes must build the same displacement)\n");
	Settings::enableSnowRaise = true;
	Settings::snowRaiseHeight = 24.0f;
	Generate("raise_colour", ColourSignature(), outDir);
	Generate("raise_depth_prepass", DepthPrepassSignature(), outDir);
	Generate("raise_shadow_map", ShadowMapSignature(), outDir);

	Settings::recomputeNormals = false;
	Generate("raise_nonormals", ColourSignature(), outDir);
	Settings::recomputeNormals = true;

	Settings::enableTessellationBounds = false;
	Generate("blanket_bounds_off", ColourSignature(), outDir);
	Settings::enableTessellationBounds = true;
	Settings::tessellationBlanketSpacing = 0.0f;
	Generate("blanket_floor_off", ColourSignature(), outDir);
	Settings::tessellationBlanketSpacing = 8.0f;
	Generate("blanket_floor_on", ColourSignature(), outDir);
	Generate("blanket_floor_depth", DepthPrepassSignature(), outDir);

	Settings::snowRaiseHeight = 0.0f;
	Generate("raise_zero", ColourSignature(), outDir);
	Settings::enableSnowRaise = false;

	std::printf("\nSurface material off (the proven-vanilla generated source)\n");
	Settings::enableSurfaceMaterial = false;
	Generate("nomaterial", ColourSignature(), outDir);
	Settings::enableSurfaceMaterial = true;

	std::printf("\nRecomputed normals off\n");
	Settings::recomputeNormals = false;
	Generate("noNormals", ColourSignature(), outDir);
	Settings::recomputeNormals = true;

	std::printf("\nAnalytic wave instead of the clipmap\n");
	Settings::useClipmap = false;
	Settings::debugWaveAmplitude = 8.0f;
	Generate("wave", ColourSignature(), outDir);
	Settings::useClipmap = true;
	Settings::debugWaveAmplitude = 0.0f;

	std::printf("\nField debug: ramp, bands, and the blend-weight fallback\n");
	for (int mode = 1; mode <= 3; ++mode) {
		Settings::debugFieldColour = mode;
		const std::string name = "fielddebug_" + std::to_string(mode);
		Generate(name.c_str(), ColourSignature(), outDir);
	}
	Settings::debugFieldColour = 0;

	std::printf("\nTrap 8 probe (displaces at factor 1, reads nothing)\n");
	Settings::enableStaticProbe = true;
	Settings::staticProbeOffset = 30.0f;
	Generate("probe_lit", ColourSignature(), outDir, Tessellation::Mode::kStaticProbe);
	Generate("probe_depth", DepthPrepassSignature(), outDir,
		Tessellation::Mode::kStaticProbe);
	Generate("probe_shadow", ShadowMapSignature(), outDir,
		Tessellation::Mode::kStaticProbe);

	Settings::staticProbeOffset = 0.0f;
	Generate("probe_zero", ColourSignature(), outDir, Tessellation::Mode::kStaticProbe);

	Settings::enableSnowRaise = true;
	Settings::snowRaiseHeight = 24.0f;
	Settings::staticProbeOffset = 30.0f;
	Generate("probe_with_raise", ColourSignature(), outDir,
		Tessellation::Mode::kStaticProbe);
	Settings::enableSnowRaise = false;
	Settings::snowRaiseHeight = 0.0f;
	Settings::staticProbeOffset = 0.0f;
	Settings::enableStaticProbe = false;

	std::printf("\nCounter-clockwise winding\n");
	Settings::tessellationWinding = "ccw";
	Generate("ccw", ColourSignature(), outDir);

	CheckAsyncCompiler();

	Settings::useClipmap = true;
	Settings::enableSnowRaise = true;
	Settings::snowRaiseHeight = 35.0f;
	Settings::shelterMeshCap = true;
	Settings::enableTessellationBounds = true;
	Generate("blood_colour", ColourSignature(), outDir, Tessellation::Mode::kBloodDecal);
	Generate("blood_depth", DepthPrepassSignature(), outDir, Tessellation::Mode::kBloodDecal);
	Settings::enableTessellationBounds = false;
	Generate("blood_unbounded", ColourSignature(), outDir, Tessellation::Mode::kBloodDecal);
	Settings::enableSnowRaise = false;
	Generate("blood_no_raise", ColourSignature(), outDir, Tessellation::Mode::kBloodDecal);
	Settings::useClipmap = false;
	Generate("blood_no_field", ColourSignature(), outDir, Tessellation::Mode::kBloodDecal);

	for (const auto& [technique, expected] : std::initializer_list<std::pair<uint32_t, bool>>{
		{ 0x2000u, true }, { 0x2081u, true },
		{ 0xC000u, false }, { 0x14000u, false },
		{ 0xE000u, false },
		{ 0x1200u, false }, { 0x3200u, false },
		{ 0x202000u, false }, { 0x402000u, false },
		{ 0x802000u, false }, { 0x1002000u, false },
		{ 0x10002000u, false }, { 0u, false } }) {
		if (UtilityRouting::IsCameraDepth(technique) != expected) {
			std::printf("FAIL: Utility routing %08X\n", technique);
			++g_failures;
		}
	}

	{

		const float orthographic[16] = {
			0.0004f, 0.0001f, 0.0000f, 0.0000f,
			0.0000f, 0.0003f, -0.0000f, 0.0000f,
			0.0001f, -0.0002f, -0.0000f, 0.0000f,
			-1.0000f, 1.0000f, -0.0001f, 1.0000f
		};
		const float perspective[16] = {
			1.2071f, 0.0000f, 0.0000f, 0.0000f,
			0.0000f, 0.0000f, 1.0000f, 1.0000f,
			0.0000f, 2.1445f, 0.0000f, 0.0000f,
			0.0000f, 0.0000f, 14.7349f, 0.0000f
		};

		if (UtilityRouting::IsPerspectiveProjection(orthographic)) {
			std::puts("FAIL: the orthographic terrain pass read as the player's camera");
			++g_failures;
		}
		if (!UtilityRouting::IsPerspectiveProjection(perspective)) {
			std::puts("FAIL: a perspective camera was refused");
			++g_failures;
		}

		if (!UtilityRouting::IsPerspectiveProjection(nullptr)) {
			std::puts("FAIL: a missing camera must not refuse the draw");
			++g_failures;
		}
	}

	std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
		g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}

namespace globals
{
	bool Ready()
	{
		return false;
	}
}

namespace Clipmap
{
	bool Ready()
	{
		return true;
	}

	void BindDomain(ID3D11DeviceContext*) {}
	void UnbindDomain(ID3D11DeviceContext*) {}
}

namespace SnowCoverage
{
	bool Ready() { return true; }
	void BindDomain(ID3D11DeviceContext*) {}
	void UnbindDomain(ID3D11DeviceContext*) {}
}

namespace Shelter
{
	bool Ready() { return true; }
	bool Initialize() { return true; }
	void BindDomain(ID3D11DeviceContext*) {}
	void UnbindDomain(ID3D11DeviceContext*) {}
}

namespace Profiler
{
	void    GpuBegin(Scope) {}
	void    GpuEnd() {}
	void    Tally(Count, uint32_t) {}
	int64_t Ticks() { return 0; }
	void    AddCpuTicks(CpuScope, int64_t) {}
}
