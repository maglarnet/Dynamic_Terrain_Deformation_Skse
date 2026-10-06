// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#ifndef NOMINMAX
#	define NOMINMAX
#endif
// Adapted from qtuna / Iwillbewarlock, PR #2, commit 43b6d0f (GPL-3.0-only).
#include <windows.h>

#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "ClipmapUpdateCS.h"
#include "ClipmapMetadata.h"
#include "ClipmapUpdateCSBaseline.h"

namespace
{
	using Microsoft::WRL::ComPtr;

	constexpr UINT kTexels = Clipmap::kTexels;
	constexpr UINT kLevels = Clipmap::kMaxLevels;
	constexpr UINT kStamps = Clipmap::kMaxStamps;
	constexpr UINT kActivity = Clipmap::kActivityTexels;

	struct Params
	{
		float window[4]{};
		float control[4]{};
		float weather[4]{};
		float stamps[kStamps][4]{};
		float stampParams[kStamps][4]{};
		float stampShape[kStamps][4]{};
		float stampMotion[kStamps][4]{};
		float coarse[4]{};
		float rimShape[4]{};
		float snowRim[4]{};
		float raise[4]{};
		float raiseWindow[4]{};
		float stampBounds[kStamps][4]{};
	};
	static_assert(sizeof(Params) % 16 == 0);
	static_assert(offsetof(Params, stampBounds) == (4 * kStamps + 8) * 16);
	static_assert(sizeof(Params) == (5 * kStamps + 8) * 16);

	void Require(bool a_ok, const std::string& a_message)
	{
		if (!a_ok) {
			throw std::runtime_error(a_message);
		}
	}

	void Check(HRESULT a_hr, const char* a_what)
	{
		if (FAILED(a_hr)) {
			char code[32];
			std::snprintf(code, sizeof(code), " (hr=0x%08X)", static_cast<unsigned>(a_hr));
			throw std::runtime_error(std::string(a_what) + code);
		}
	}

	bool SkyrimRunning()
	{
		const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
		if (snapshot == INVALID_HANDLE_VALUE) {
			return false;
		}
		PROCESSENTRY32W entry{};
		entry.dwSize = sizeof(entry);
		bool found = false;
		for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found;
			 ok = Process32NextW(snapshot, &entry)) {
			found = _wcsicmp(entry.szExeFile, L"SkyrimSE.exe") == 0;
		}
		CloseHandle(snapshot);
		return found;
	}

	std::string CurrentSource()
	{

		auto source = Clipmap::UpdateShaderSource();
		const std::string declaration = "Texture2D<float> FieldBefore : register(t5);";
		source.erase(source.find(declaration), declaration.size());
		const std::string from = "FieldBefore[";
		for (auto at = source.find(from); at != std::string::npos; at = source.find(from)) {
			source.replace(at, from.size(), "Field[");
		}
		return source;
	}

	std::string BaselineSource()
	{
		const std::string current = Clipmap::UpdateShaderSource();
		const size_t      body = std::strlen(Clipmap::kUpdateShader);
		Require(current.size() > body &&
					current.compare(current.size() - body, body, Clipmap::kUpdateShader) == 0,
			"UpdateShaderSource() no longer ends with kUpdateShader");
		return current.substr(0, current.size() - body) +
			"static const float kPrintCoreLo = 0.06f;\n"
			"static const float kPrintCoreHi = 0.96f;\n"
			"static const float kPrintBandRiseLo = 0.14f;\n"
			"static const float kPrintBandRiseHi = 0.34f;\n"
			"static const float kPrintBandFallLo = 0.52f;\n"
			"static const float kPrintBandFallHi = 0.84f;\n" + ClipmapBaseline::kUpdateShader;
	}

	std::string WithoutNeighbourRace(std::string a_source)
	{
		const std::string from = "= Field[uint2((int2(id.xy) + int2(";
		const std::string to = "= FieldBefore[uint2((int2(id.xy) + int2(";
		int               replaced = 0;
		for (size_t at = a_source.find(from); at != std::string::npos;
			 at = a_source.find(from, at + to.size())) {
			a_source.replace(at, from.size(), to);
			++replaced;
		}
		Require(replaced == 4, "expected the four repose neighbour reads");

		const std::string field = "RWTexture2D<float> Field : register(u0);";
		const size_t      at = a_source.find(field);
		Require(at != std::string::npos, "Field declaration not found");
		a_source.insert(at + field.size(), "\nTexture2D<float> FieldBefore : register(t5);");
		return a_source;
	}

	ComPtr<ID3DBlob> Compile(const std::string& a_source, const char* a_label)
	{
		const std::string      maxStamps = std::to_string(kStamps);
		const D3D_SHADER_MACRO defines[] = {
			{ "MAX_STAMPS", maxStamps.c_str() },
			{ nullptr, nullptr }
		};
		ComPtr<ID3DBlob> code;
		ComPtr<ID3DBlob> errors;
		const HRESULT    hr = D3DCompile(a_source.c_str(), a_source.size(), "ClipmapUpdateCS", defines,
			   nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
		if (FAILED(hr)) {
			std::fprintf(stderr, "%s:\n%s\n", a_label,
				errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
		}
		Check(hr, a_label);
		return code;
	}

	void WriteDisassembly(ID3DBlob* a_code, const std::string& a_path)
	{
		ComPtr<ID3DBlob> text;
		Check(D3DDisassemble(a_code->GetBufferPointer(), a_code->GetBufferSize(), 0, nullptr, &text),
			"D3DDisassemble");
		std::ofstream(a_path, std::ios::binary)
			.write(static_cast<const char*>(text->GetBufferPointer()),
				static_cast<std::streamsize>(text->GetBufferSize() - 1));
		std::printf("  wrote %s\n", a_path.c_str());
	}

	void CheckLayout(ID3DBlob* code)
	{
		ComPtr<ID3D11ShaderReflection> reflection;
		Check(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(),
			__uuidof(ID3D11ShaderReflection), &reflection), "Reflect params");
		auto* buffer = reflection->GetConstantBufferByName("Params");
		D3D11_SHADER_BUFFER_DESC desc{};
		Check(buffer->GetDesc(&desc), "Params layout");
		Require(desc.Size == sizeof(Params), "Params CPU/HLSL size mismatch");
		D3D11_SHADER_VARIABLE_DESC bounds{};
		Check(buffer->GetVariableByName("StampBounds")->GetDesc(&bounds), "StampBounds layout");
		Require(bounds.StartOffset == offsetof(Params, stampBounds) &&
			bounds.Size == sizeof(Params::stampBounds), "StampBounds CPU/HLSL layout mismatch");
		std::puts("PASS CPU/HLSL Params size and StampBounds offset");
	}

	struct Level
	{
		ComPtr<ID3D11Texture2D>           field;
		ComPtr<ID3D11UnorderedAccessView> fieldUAV;
		ComPtr<ID3D11ShaderResourceView>  fieldSRV;
		ComPtr<ID3D11Texture2D>           decay;
		ComPtr<ID3D11UnorderedAccessView> decayUAV;
		ComPtr<ID3D11ShaderResourceView>  decaySRV;
		ComPtr<ID3D11Texture2D>           activity;
		ComPtr<ID3D11UnorderedAccessView> activityUAV;
	};

	struct Side
	{
		ComPtr<ID3D11ComputeShader>      shader;
		Level                            level[kLevels];
		ComPtr<ID3D11Texture2D>          before;
		ComPtr<ID3D11ShaderResourceView> beforeSRV;
		ComPtr<ID3D11UnorderedAccessView> beforeUAV;
		bool pingPong = false;
	};

	class Gpu
	{
	public:
		explicit Gpu(bool a_warp)
		{
			const D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0;
			Check(D3D11CreateDevice(nullptr, a_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
					  nullptr, 0, &wanted, 1, D3D11_SDK_VERSION, &_device, nullptr, &_context),
				"D3D11CreateDevice");

			ComPtr<IDXGIDevice>  dxgi;
			ComPtr<IDXGIAdapter> adapter;
			DXGI_ADAPTER_DESC    desc{};
			if (SUCCEEDED(_device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
				SUCCEEDED(adapter->GetDesc(&desc))) {
				std::printf("Adapter: %ls\n", desc.Description);
			}

			std::mt19937 rng(0x5EED0001u);
			_shape = MakeTexture(256, DXGI_FORMAT_R8G8B8A8_UNORM, 4, rng);
			_coverage = MakeTexture(SnowCoverage::kTexels, DXGI_FORMAT_R8_UNORM, 1, rng);
			_meshCap = MakeTexture(Shelter::kTexels, DXGI_FORMAT_R8_UNORM, 1, rng);

			D3D11_SAMPLER_DESC samplerDesc{};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			Check(_device->CreateSamplerState(&samplerDesc, &_sampler), "CreateSamplerState");

			D3D11_BUFFER_DESC cbDesc{};
			cbDesc.ByteWidth = sizeof(Params);
			cbDesc.Usage = D3D11_USAGE_DYNAMIC;
			cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			Check(_device->CreateBuffer(&cbDesc, nullptr, &_params), "CreateBuffer");

			_readField = Staging(kTexels, DXGI_FORMAT_R32_FLOAT);
			_readDecay = Staging(kTexels, DXGI_FORMAT_R8G8_UNORM);
			_readDecay16 = Staging(kTexels, Clipmap::kMetadataFormat);
			_readActivity = Staging(kActivity, DXGI_FORMAT_R32_UINT);

			D3D11_QUERY_DESC query{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			Check(_device->CreateQuery(&query, &_disjoint), "CreateQuery");
			query.Query = D3D11_QUERY_TIMESTAMP;
			Check(_device->CreateQuery(&query, &_start), "CreateQuery");
			Check(_device->CreateQuery(&query, &_end), "CreateQuery");
		}

		void CreateSide(Side& a_side, ID3DBlob* a_code, DXGI_FORMAT metadataFormat = DXGI_FORMAT_R8G8_UNORM)
		{
			Check(_device->CreateComputeShader(a_code->GetBufferPointer(), a_code->GetBufferSize(), nullptr,
					  &a_side.shader),
				"CreateComputeShader");

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kTexels;
			desc.Height = kTexels;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			for (auto& level : a_side.level) {
				desc.Width = desc.Height = kTexels;
				desc.Format = DXGI_FORMAT_R32_FLOAT;
				Check(_device->CreateTexture2D(&desc, nullptr, &level.field), "field");
				Check(_device->CreateUnorderedAccessView(level.field.Get(), nullptr, &level.fieldUAV), "field UAV");
				Check(_device->CreateShaderResourceView(level.field.Get(), nullptr, &level.fieldSRV), "field SRV");
				desc.Format = metadataFormat;
				Check(_device->CreateTexture2D(&desc, nullptr, &level.decay), "decay");
				Check(_device->CreateUnorderedAccessView(level.decay.Get(), nullptr, &level.decayUAV), "decay UAV");
				Check(_device->CreateShaderResourceView(level.decay.Get(), nullptr, &level.decaySRV), "decay SRV");
				desc.Width = desc.Height = kActivity;
				desc.Format = DXGI_FORMAT_R32_UINT;
				Check(_device->CreateTexture2D(&desc, nullptr, &level.activity), "activity");
				Check(_device->CreateUnorderedAccessView(level.activity.Get(), nullptr, &level.activityUAV),
					"activity UAV");
			}
			desc.Width = desc.Height = kTexels;
			desc.Format = DXGI_FORMAT_R32_FLOAT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			Check(_device->CreateTexture2D(&desc, nullptr, &a_side.before), "before");
			Check(_device->CreateShaderResourceView(a_side.before.Get(), nullptr, &a_side.beforeSRV), "before SRV");
			Check(_device->CreateUnorderedAccessView(a_side.before.Get(), nullptr, &a_side.beforeUAV), "before UAV");
		}

		void Load(Side& a_side, uint32_t a_level, const std::vector<float>& a_field,
			const std::vector<uint8_t>& a_decay)
		{
			auto& level = a_side.level[a_level];
			_context->UpdateSubresource(level.field.Get(), 0, nullptr, a_field.data(), kTexels * 4, 0);
			D3D11_TEXTURE2D_DESC desc{};
			level.decay->GetDesc(&desc);
			if (desc.Format == Clipmap::kMetadataFormat) {
				std::vector<uint16_t> precise(a_decay.size());
				for (size_t i = 0; i < a_decay.size(); ++i) { precise[i] = a_decay[i] * 257u; }
				_context->UpdateSubresource(level.decay.Get(), 0, nullptr, precise.data(), kTexels * 4, 0);
			} else {
				_context->UpdateSubresource(level.decay.Get(), 0, nullptr, a_decay.data(), kTexels * 2, 0);
			}
		}

		void Clear(Side& a_side)
		{
			const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			for (auto& level : a_side.level) {
				_context->ClearUnorderedAccessViewFloat(level.fieldUAV.Get(), zero);
				_context->ClearUnorderedAccessViewFloat(level.decayUAV.Get(), zero);
			}
		}

		void Update(Side& a_side, const Params (&a_levels)[kLevels], bool a_withoutRace)
		{
			const UINT noOffset[3] = { static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1) };

			ID3D11ShaderResourceView* shape = _shape.Get();
			_context->CSSetShaderResources(0, 1, &shape);
			ID3D11ShaderResourceView* floorMaps[2] = {
				a_levels[0].raise[0] > 0.0f ? _coverage.Get() : nullptr,
				a_levels[0].raise[3] > 0.0f ? _meshCap.Get() : nullptr
			};
			_context->CSSetShaderResources(3, 2, floorMaps);
			_context->CSSetSamplers(0, 1, _sampler.GetAddressOf());
			_context->CSSetShader(a_side.shader.Get(), nullptr, 0);

			for (uint32_t i = 0; i < kLevels; ++i) {
				const uint32_t level = kLevels - 1 - i;
				const bool     hasCoarser = level + 1 < kLevels;
				auto&          target = a_side.level[level];

				ID3D11ShaderResourceView* seeds[2] = {
					hasCoarser ? a_side.level[level + 1].fieldSRV.Get() : nullptr,
					hasCoarser ? a_side.level[level + 1].decaySRV.Get() : nullptr
				};
				_context->CSSetShaderResources(1, 2, seeds);

				D3D11_MAPPED_SUBRESOURCE mapped{};
				Check(_context->Map(_params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map params");
				std::memcpy(mapped.pData, &a_levels[level], sizeof(Params));
				_context->Unmap(_params.Get(), 0);

				const UINT zero[4] = { 0, 0, 0, 0 };
				_context->ClearUnorderedAccessViewUint(target.activityUAV.Get(), zero);

				if (a_withoutRace) {
					if (!a_side.pingPong) { _context->CopyResource(a_side.before.Get(), target.field.Get()); }
					ID3D11ShaderResourceView* before = a_side.pingPong ? target.fieldSRV.Get() : a_side.beforeSRV.Get();
					_context->CSSetShaderResources(5, 1, &before);
				}

				ID3D11UnorderedAccessView* uavs[3] = { a_side.pingPong ? a_side.beforeUAV.Get() : target.fieldUAV.Get(), target.decayUAV.Get(),
					target.activityUAV.Get() };
				_context->CSSetConstantBuffers(0, 1, _params.GetAddressOf());
				_context->CSSetUnorderedAccessViews(0, 3, uavs, noOffset);
				_context->Dispatch(kTexels / 8, kTexels / 8, 1);

				ID3D11UnorderedAccessView* nullUAVs[3] = { nullptr, nullptr, nullptr };
				_context->CSSetUnorderedAccessViews(0, 3, nullUAVs, noOffset);
				ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
				_context->CSSetShaderResources(1, 2, nullSRVs);
				_context->CSSetShaderResources(5, 1, nullSRVs);
				if (a_side.pingPong) {
					target.field.Swap(a_side.before);
					target.fieldSRV.Swap(a_side.beforeSRV);
					target.fieldUAV.Swap(a_side.beforeUAV);
				}
			}
			ID3D11ShaderResourceView* nullSRVs[5] = {};
			_context->CSSetShaderResources(0, 5, nullSRVs);
		}

		double TimedUpdate(Side& a_side, const Params (&a_levels)[kLevels], bool snapshot = false)
		{
			_context->Begin(_disjoint.Get());
			_context->End(_start.Get());
			Update(a_side, a_levels, snapshot);
			_context->End(_end.Get());
			_context->End(_disjoint.Get());

			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			while (_context->GetData(_disjoint.Get(), &disjoint, sizeof(disjoint), 0) != S_OK) {
			}
			UINT64 start = 0;
			UINT64 end = 0;
			while (_context->GetData(_start.Get(), &start, sizeof(start), 0) != S_OK) {
			}
			while (_context->GetData(_end.Get(), &end, sizeof(end), 0) != S_OK) {
			}
			if (disjoint.Disjoint) {
				return -1.0;
			}
			return static_cast<double>(end - start) * 1000.0 / static_cast<double>(disjoint.Frequency);
		}

		void Read(ID3D11Texture2D* a_source, std::vector<uint8_t>& a_out)
		{
			D3D11_TEXTURE2D_DESC desc{};
			a_source->GetDesc(&desc);
			ID3D11Texture2D* staging = desc.Format == DXGI_FORMAT_R32_FLOAT ? _readField.Get() :
			                           desc.Format == DXGI_FORMAT_R8G8_UNORM ? _readDecay.Get() :
			                           desc.Format == Clipmap::kMetadataFormat ? _readDecay16.Get() :
			                                                                   _readActivity.Get();
			const UINT rowBytes = desc.Width * (desc.Format == DXGI_FORMAT_R8G8_UNORM ? 2 : 4);

			_context->CopyResource(staging, a_source);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			Check(_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped), "Map readback");
			a_out.resize(static_cast<size_t>(rowBytes) * desc.Height);
			for (UINT y = 0; y < desc.Height; ++y) {
				std::memcpy(a_out.data() + static_cast<size_t>(y) * rowBytes,
					static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
			}
			_context->Unmap(staging, 0);
		}

	private:
		ComPtr<ID3D11ShaderResourceView> MakeTexture(UINT a_size, DXGI_FORMAT a_format, UINT a_bytes,
			std::mt19937& a_rng)
		{
			std::vector<uint8_t> data(static_cast<size_t>(a_size) * a_size * a_bytes);
			for (auto& value : data) {
				value = static_cast<uint8_t>(a_rng() >> 24);
			}
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = a_size;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = a_format;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			D3D11_SUBRESOURCE_DATA initial{ data.data(), a_size * a_bytes, 0 };
			ComPtr<ID3D11Texture2D>          texture;
			ComPtr<ID3D11ShaderResourceView> view;
			Check(_device->CreateTexture2D(&desc, &initial, &texture), "input texture");
			Check(_device->CreateShaderResourceView(texture.Get(), nullptr, &view), "input SRV");
			return view;
		}

		ComPtr<ID3D11Texture2D> Staging(UINT a_size, DXGI_FORMAT a_format)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = a_size;
			desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
			desc.Format = a_format;
			desc.Usage = D3D11_USAGE_STAGING;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ComPtr<ID3D11Texture2D> texture;
			Check(_device->CreateTexture2D(&desc, nullptr, &texture), "staging");
			return texture;
		}

		ComPtr<ID3D11Device>             _device;
		ComPtr<ID3D11DeviceContext>      _context;
		ComPtr<ID3D11ShaderResourceView> _shape;
		ComPtr<ID3D11ShaderResourceView> _coverage;
		ComPtr<ID3D11ShaderResourceView> _meshCap;
		ComPtr<ID3D11SamplerState>       _sampler;
		ComPtr<ID3D11Buffer>             _params;
		ComPtr<ID3D11Texture2D>          _readField;
		ComPtr<ID3D11Texture2D>          _readDecay;
		ComPtr<ID3D11Texture2D>          _readDecay16;
		ComPtr<ID3D11Texture2D>          _readActivity;
		ComPtr<ID3D11Query>              _disjoint;
		ComPtr<ID3D11Query>              _start;
		ComPtr<ID3D11Query>              _end;
	};

	struct Coverage
	{
		uint64_t kinds[4]{};
		uint64_t snow{ 0 };
		uint64_t moving{ 0 };
		int      emptyFrames{ 0 };
		int      fullFrames{ 0 };
		int      jumps{ 0 };
		int      paused{ 0 };
		int      fillFrames{ 0 };
		int      reposeFrames{ 0 };
		int      raiseFrames{ 0 };
		int      tieFrames{ 0 };
	};

	class Scenario
	{
	public:
		Scenario(uint32_t a_seed, bool a_repose) :
			_rng(a_seed), _repose(a_repose)
		{
			_x = Uniform(-150000.0f, 150000.0f);
			_y = Uniform(-150000.0f, 150000.0f);
			RandomSettings();
		}

		void Next(Params (&a_levels)[kLevels], Coverage& a_coverage)
		{
			Move(a_coverage);

			if (++_frame % 12 == 0 || Chance(0.04f)) {
				RandomSettings();
			}

			Params p{};
			p.window[3] = static_cast<float>(kTexels);

			float dt = 1.0f / 60.0f + Uniform(-0.004f, 0.004f);
			if (Chance(0.1f)) {
				dt = 0.0f;
			} else if (Chance(0.1f)) {
				dt = Uniform(0.05f, 0.4f);
			} else if (Chance(0.1f)) {
				dt = Uniform(0.0f, 0.002f);
			}
			p.control[0] = std::clamp(dt, 0.0f, 0.25f);
			a_coverage.paused += p.control[0] == 0.0f;

			const int count = Chance(0.12f) ? 0 : Chance(0.15f) ? static_cast<int>(kStamps) :
			                                                      static_cast<int>(_rng() % (kStamps + 1));
			p.control[1] = static_cast<float>(count);
			a_coverage.emptyFrames += count == 0;
			a_coverage.fullFrames += count == static_cast<int>(kStamps);

			p.control[2] = static_cast<float>(kTexels / 2 - 2);
			p.control[3] = _rimSpan;
			p.weather[0] = _fill;
			p.weather[1] = _slope;
			p.weather[2] = _repose ? _reposeRate : 0.0f;
			p.weather[3] = _rimNoise;
			p.rimShape[0] = _lean;
			p.rimShape[1] = _churn;
			p.rimShape[2] = _snowRepose;
			for (int i = 0; i < 4; ++i) {
				p.snowRim[i] = _snowRim[i];
			}
			a_coverage.fillFrames += _fill > 0.0f;
			a_coverage.reposeFrames += _slope > 0.0f;

			const float cell0 = Clipmap::CellSizeFor(0);
			p.raise[0] = _raiseHeight;
			p.raise[1] = _raiseScale;
			p.raise[2] = _bite;
			p.raise[3] = _meshCap;
			p.raiseWindow[0] = std::floor(_x / cell0) * cell0;
			p.raiseWindow[1] = std::floor(_y / cell0) * cell0;
			p.raiseWindow[3] = _raiseDistance;
			p.raiseWindow[2] = std::max(_raiseDistance - _raiseBand, 0.0f);
			a_coverage.raiseFrames += _raiseHeight > 0.0f;

			for (int i = 0; i < count; ++i) {
				Stamp(p, i, a_coverage);
			}

			if (count > 1 && Chance(0.25f)) {
				++a_coverage.tieFrames;
				for (int copies = 1 + static_cast<int>(_rng() % 8); copies > 0; --copies) {
					const int from = static_cast<int>(_rng() % count);
					const int to = static_cast<int>(_rng() % count);
					for (int k = 0; k < 4; ++k) {
						p.stamps[to][k] = p.stamps[from][k];
						p.stampShape[to][k] = p.stampShape[from][k];
						p.stampMotion[to][k] = p.stampMotion[from][k];
						if (Chance(0.5f)) {
							p.stampParams[to][k] = p.stampParams[from][k];
						}
					}
					if (Chance(0.3f)) {
						p.stamps[to][3] = -p.stamps[to][3];
					}
				}
			}
			Clipmap::FillStampBounds(p, static_cast<uint32_t>(count));

			for (uint32_t i = 0; i < kLevels; ++i) {
				const uint32_t level = kLevels - 1 - i;
				const float    cell = Clipmap::CellSizeFor(level);
				Params&        out = a_levels[level];
				out = p;
				out.window[0] = std::floor(_x / cell);
				out.window[1] = std::floor(_y / cell);
				out.window[2] = cell;

				const bool hasCoarser = level + 1 < kLevels;
				out.coarse[0] = hasCoarser ? 1.0f : 0.0f;
				out.coarse[1] = Clipmap::WorldSizeFor(level + 1);
				out.coarse[2] = _smoothing;

				const auto nowX = static_cast<int32_t>(out.window[0]);
				const auto nowY = static_cast<int32_t>(out.window[1]);
				int32_t    moved = static_cast<int32_t>(kTexels);
				if (_prevValid[level]) {
					moved = std::max(std::abs(nowX - _prevX[level]), std::abs(nowY - _prevY[level]));
				}
				_prevX[level] = nowX;
				_prevY[level] = nowY;
				_prevValid[level] = true;
				out.coarse[3] = std::clamp(out.control[2] - static_cast<float>(moved) - 2.0f, 0.0f, out.control[2]);
			}
		}

	private:
		float Uniform(float a_lo, float a_hi) { return std::uniform_real_distribution<float>(a_lo, a_hi)(_rng); }
		bool  Chance(float a_p) { return Uniform(0.0f, 1.0f) < a_p; }

		void Move(Coverage& a_coverage)
		{
			if (Chance(0.015f)) {
				const float extent = Chance(0.3f) ? 400000.0f : 150000.0f;
				_x = Uniform(-extent, extent);
				_y = Uniform(-extent, extent);
				++a_coverage.jumps;
			} else if (Chance(0.04f)) {
				_x += Uniform(-3000.0f, 3000.0f);
				_y += Uniform(-3000.0f, 3000.0f);
				++a_coverage.jumps;
			} else if (!Chance(0.2f)) {
				_x += Uniform(-9.0f, 9.0f);
				_y += Uniform(-9.0f, 9.0f);
			}
		}

		void RandomSettings()
		{
			const float pick = Uniform(0.0f, 1.0f);
			_rimSpan = pick < 0.1f ? 0.0f : pick < 0.6f ? 1.0f : Uniform(0.0f, 3.0f);
			_fill = Chance(0.35f) ? Uniform(0.001f, 0.3f) : 0.0f;
			_slope = _repose && Chance(0.8f) ? std::tan(Uniform(10.0f, 80.0f) * 0.017453292f) : 0.0f;
			_reposeRate = Chance(0.5f) ? 0.01f : Uniform(0.0f, 1.0f);
			_rimNoise = Chance(0.5f) ? 0.0f : Uniform(0.0f, 8.0f);
			_lean = Chance(0.5f) ? 1.0f : Uniform(0.0f, 1.0f);
			_churn = Chance(0.4f) ? 0.45f : Chance(0.3f) ? 0.0f : Uniform(0.0f, 8.0f);
			_snowRepose = Chance(0.5f) ? _reposeRate : Uniform(0.0f, 1.0f);
			_snowRim[0] = Chance(0.3f) ? _rimSpan : Uniform(0.0f, 3.0f);
			_snowRim[1] = Chance(0.3f) ? _rimNoise : Uniform(0.0f, 8.0f);
			_snowRim[2] = Chance(0.3f) ? _lean : Uniform(0.0f, 1.0f);
			_snowRim[3] = Chance(0.3f) ? _churn : Uniform(0.0f, 8.0f);
			const bool floored = Chance(0.6f);
			_raiseHeight = floored ? (Chance(0.5f) ? 35.0f : Uniform(1.0f, 60.0f)) : 0.0f;
			_raiseScale = Uniform(0.0f, 1.5f);
			_bite = Chance(0.6f) ? 0.0f : Uniform(0.0f, 8.0f);
			_meshCap = floored && Chance(0.5f) ? 1.0f : 0.0f;
			_raiseDistance = Chance(0.5f) ? 7936.0f : Uniform(200.0f, 3000.0f);
			_raiseBand = Chance(0.5f) ? 4000.0f : Uniform(0.0f, 2000.0f);
			_smoothing = Chance(0.6f) ? 1.0f : Uniform(0.0f, 3.0f);
		}

		void Stamp(Params& a_p, int a_i, Coverage& a_coverage)
		{
			const float kind = Chance(0.22f) ? 1.0f : 0.0f;
			const float region = Chance(0.85f) ? 576.0f : Chance(0.67f) ? 3000.0f : 30000.0f;
			const float pick = Uniform(0.0f, 1.0f);
			const float radius = pick < 0.7f ? Uniform(4.0f, 60.0f) : pick < 0.9f ? Uniform(60.0f, 256.0f) :
			                     pick < 0.97f                     ? Uniform(0.5f, 4.0f) :
			                                                        0.0f;
			const float angle = Uniform(0.0f, 6.2831853f);
			const bool  snow = Chance(0.4f);

			a_p.stamps[a_i][0] = _x + Uniform(-region, region);
			a_p.stamps[a_i][1] = _y + Uniform(-region, region);
			a_p.stamps[a_i][2] = radius;
			a_p.stamps[a_i][3] = kind == 1.0f ? Uniform(0.0f, 1.2f) : (Chance(0.1f) ? 0.0f : Uniform(0.0f, 40.0f));

			a_p.stampParams[a_i][0] = Uniform(0.05f, 0.95f);
			a_p.stampParams[a_i][1] = Uniform(0.5f, 1.0f);
			a_p.stampParams[a_i][2] = kind;
			a_p.stampParams[a_i][3] = kind == 1.0f ? (Chance(0.5f) ? 0.0f : Uniform(-12.0f, 0.0f)) :
			                          Chance(0.4f) ? 0.0f :
			                                         Uniform(0.0f, 10.0f);

			const bool straight = Chance(0.2f);
			a_p.stampShape[a_i][0] = straight ? 0.0f : std::sin(angle);
			a_p.stampShape[a_i][1] = straight ? 1.0f : std::cos(angle);
			a_p.stampShape[a_i][2] = (Chance(0.5f) ? 0.0f : Uniform(2.0f, 60.0f));
			a_p.stampShape[a_i][3] = Chance(0.5f) ? 1.0f : -1.0f;

			const float motion = Uniform(0.0f, 1.0f);
			const float reach = motion < 0.6f ? 0.0f : motion < 0.9f ? 30.0f : 200.0f;
			a_p.stampMotion[a_i][0] = Uniform(-reach, reach);
			a_p.stampMotion[a_i][1] = Uniform(-reach, reach);
			a_p.stampMotion[a_i][2] = snow ? 1.0f : 0.0f;

			a_coverage.kinds[kind == 0.0f ? (a_p.stampParams[a_i][3] > 0.0f ? 1 : 0) : kind == 1.0f ? 2 : 3]++;
			a_coverage.snow += snow;
			a_coverage.moving += reach > 0.0f;
		}

		std::mt19937 _rng;
		bool         _repose;
		int          _frame{ 0 };
		float        _x{ 0.0f };
		float        _y{ 0.0f };
		int32_t      _prevX[kLevels]{};
		int32_t      _prevY[kLevels]{};
		bool         _prevValid[kLevels]{};

		float _rimSpan{ 1.0f };
		float _fill{ 0.0f };
		float _slope{ 0.0f };
		float _reposeRate{ 0.01f };
		float _rimNoise{ 0.0f };
		float _lean{ 1.0f };
		float _churn{ 0.45f };
		float _snowRepose{ 0.1f };
		float _snowRim[4]{};
		float _raiseHeight{ 0.0f };
		float _raiseScale{ 1.0f };
		float _bite{ 0.0f };
		float _meshCap{ 0.0f };
		float _raiseDistance{ 7936.0f };
		float _raiseBand{ 4000.0f };
		float _smoothing{ 1.0f };
	};

	void LoadStartingState(Gpu& a_gpu, Side& a_one, Side& a_two, uint32_t a_seed)
	{
		std::mt19937                          rng(a_seed);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		std::vector<float>                    field(static_cast<size_t>(kTexels) * kTexels);
		std::vector<uint8_t>                  decay(field.size() * 2);
		for (uint32_t level = 0; level < kLevels; ++level) {
			for (size_t i = 0; i < field.size(); ++i) {
				const float pick = unit(rng);
				field[i] = pick < 0.45f ? 0.0f : pick < 0.47f ? -0.0f : pick < 0.8f ? -30.0f * unit(rng) :
				           pick < 0.97f                          ? 8.0f * unit(rng) :
				                                                   400.0f * (unit(rng) - 0.5f);
				decay[i * 2] = static_cast<uint8_t>(rng() >> 24);
				decay[i * 2 + 1] = static_cast<uint8_t>(rng() >> 24);
			}
			a_gpu.Load(a_one, level, field, decay);
			a_gpu.Load(a_two, level, field, decay);
		}
	}

	struct Difference
	{
		size_t texels{ 0 };
		size_t first{ 0 };
		double maxAbs{ 0.0 };
	};

	Difference Compare(const std::vector<uint8_t>& a_one, const std::vector<uint8_t>& a_two, size_t a_stride,
		bool a_float)
	{
		Difference result;
		if (a_one.size() == a_two.size() && std::memcmp(a_one.data(), a_two.data(), a_one.size()) == 0) {
			return result;
		}
		for (size_t i = 0; i + a_stride <= a_one.size(); i += a_stride) {
			if (std::memcmp(a_one.data() + i, a_two.data() + i, a_stride) == 0) {
				continue;
			}
			if (result.texels++ == 0) {
				result.first = i / a_stride;
			}
			double one = 0.0;
			double two = 0.0;
			if (a_stride == 4) {
				float    f[2];
				uint32_t u[2];
				std::memcpy(&f[0], a_one.data() + i, 4);
				std::memcpy(&f[1], a_two.data() + i, 4);
				std::memcpy(&u[0], a_one.data() + i, 4);
				std::memcpy(&u[1], a_two.data() + i, 4);
				one = a_float ? f[0] : u[0];
				two = a_float ? f[1] : u[1];
			} else {
				one = a_one[i] + a_one[i + 1] * 256.0;
				two = a_two[i] + a_two[i + 1] * 256.0;
			}
			result.maxAbs = std::max(result.maxAbs, std::abs(one - two));
			if (std::isnan(one) != std::isnan(two)) {
				result.maxAbs = INFINITY;
			}
		}
		return result;
	}

	struct FrameDifference
	{
		Difference field, decay, activity;
		uint32_t   level{ 0 };
		bool       Any() const { return field.texels || decay.texels || activity.texels; }
	};

	FrameDifference CompareSides(Gpu& a_gpu, Side& a_one, Side& a_two)
	{
		static std::vector<uint8_t> one;
		static std::vector<uint8_t> two;
		FrameDifference             total;
		for (uint32_t level = 0; level < kLevels; ++level) {
			const auto add = [&](Difference& a_total, ID3D11Texture2D* a_a, ID3D11Texture2D* a_b, size_t a_stride,
								 bool a_float) {
				a_gpu.Read(a_a, one);
				a_gpu.Read(a_b, two);
				const auto d = Compare(one, two, a_stride, a_float);
				if (d.texels && !total.Any()) {
					total.level = level;
				}
				if (d.texels && !a_total.texels) {
					a_total.first = d.first;
				}
				a_total.texels += d.texels;
				a_total.maxAbs = std::max(a_total.maxAbs, d.maxAbs);
			};
			add(total.field, a_one.level[level].field.Get(), a_two.level[level].field.Get(), 4, true);
			D3D11_TEXTURE2D_DESC metadataDesc{};
			a_one.level[level].decay->GetDesc(&metadataDesc);
			add(total.decay, a_one.level[level].decay.Get(), a_two.level[level].decay.Get(),
				metadataDesc.Format == Clipmap::kMetadataFormat ? 4 : 2, false);
			add(total.activity, a_one.level[level].activity.Get(), a_two.level[level].activity.Get(), 4, false);
		}
		return total;
	}

	std::string Describe(const FrameDifference& a_d)
	{
		char text[256];
		std::snprintf(text, sizeof(text),
			"level %u: Field %zu texels (max |diff| %g, first at %zu,%zu), DecayRate %zu, Activity %zu",
			a_d.level, a_d.field.texels, a_d.field.maxAbs, a_d.field.first % kTexels, a_d.field.first / kTexels,
			a_d.decay.texels, a_d.activity.texels);
		return text;
	}

	int RunFrames(Gpu& a_gpu, Side& a_one, Side& a_two, const char* a_name, int a_frames, uint32_t a_seed,
		bool a_repose, bool a_withoutRace, bool a_exact)
	{
		LoadStartingState(a_gpu, a_one, a_two, a_seed * 7919u + 1u);
		Scenario scenario(a_seed, a_repose);
		Coverage coverage;
		int      differing = 0;
		size_t   worstTexels = 0;
		for (int frame = 0; frame < a_frames; ++frame) {
			Params levels[kLevels];
			scenario.Next(levels, coverage);
			a_gpu.Update(a_one, levels, a_withoutRace);
			a_gpu.Update(a_two, levels, a_withoutRace);
			const auto d = CompareSides(a_gpu, a_one, a_two);
			if (d.Any()) {
				++differing;
				worstTexels = std::max(worstTexels, d.field.texels + d.decay.texels + d.activity.texels);
				if (a_exact) {
					throw std::runtime_error(std::string(a_name) + ": frame " + std::to_string(frame) +
											 " differs, " + Describe(d));
				}
			}
		}
		std::printf("%s %s: %d frames x %u levels, %d differing (worst %zu texels)\n",
			a_exact ? (differing ? "FAIL" : "PASS") : "INFO", a_name, a_frames, kLevels, differing, worstTexels);
		std::printf("       stamps: %llu press, %llu press+rim, %llu melt, %llu retired kind (%llu snow, %llu swept); "
					"frames: %d empty, %d full, %d jumps, %d paused, %d fill, %d repose, %d raise, %d ties\n",
			coverage.kinds[0], coverage.kinds[1], coverage.kinds[2], coverage.kinds[3], coverage.snow,
			coverage.moving, coverage.emptyFrames, coverage.fullFrames, coverage.jumps, coverage.paused,
			coverage.fillFrames, coverage.reposeFrames, coverage.raiseFrames, coverage.tieFrames);
		return differing;
	}

	void TimingParams(int a_count, int a_mode, Params (&a_levels)[kLevels])
	{
		const float  px = 20000.3f;
		const float  py = -35000.7f;
		std::mt19937 rng(1234u + static_cast<uint32_t>(a_count * 3 + a_mode));
		std::uniform_real_distribution<float> close(-400.0f, 400.0f);
		std::uniform_real_distribution<float> wide(-550.0f, 550.0f);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);

		Params p{};
		p.window[3] = static_cast<float>(kTexels);
		p.control[0] = 1.0f / 60.0f;
		p.control[1] = static_cast<float>(a_count);
		p.control[2] = static_cast<float>(kTexels / 2 - 2);
		p.control[3] = 1.0f;
		p.weather[1] = std::tan(50.0f * 0.017453292f);
		p.weather[2] = 0.01f;
		p.rimShape[0] = 1.0f;
		p.rimShape[1] = 0.45f;
		p.rimShape[2] = 0.10f;
		p.snowRim[0] = 0.7f;
		p.snowRim[1] = 0.8f;
		p.snowRim[2] = 1.0f;
		p.snowRim[3] = 0.8f;
		p.raise[0] = 35.0f;
		p.raise[1] = 1.0f;
		p.raise[3] = 1.0f;
		p.raiseWindow[0] = std::floor(px / Clipmap::CellSizeFor(0)) * Clipmap::CellSizeFor(0);
		p.raiseWindow[1] = std::floor(py / Clipmap::CellSizeFor(0)) * Clipmap::CellSizeFor(0);
		p.raiseWindow[2] = 3936.0f;
		p.raiseWindow[3] = 7936.0f;
		for (int i = 0; i < a_count; ++i) {
			float r = 8.0f + 6.0f * unit(rng);
			float kind = 0.0f;
			float x = px + close(rng);
			float y = py + close(rng);
			float halfWidth = i % 3 == 0 ? r * 0.5f : 0.0f;
			float rim = 1.2f * 1.45f * 0.2f;
			float depth = 6.0f * 0.2f;
			if ((a_mode == 1 || a_mode == 2) && i < 40) {
				r = 20.0f + 40.0f * unit(rng);
				kind = 1.0f;
				x = px + wide(rng);
				y = py + wide(rng);
				halfWidth = 0.0f;
				rim = 0.0f;
				depth = 0.85f;
			}
			if (a_mode == 2 && i < 6) {
				r = 256.0f;
			}
			if (a_mode == 3) {
				x += 5000.0f;
			}
			p.stamps[i][0] = x;
			p.stamps[i][1] = y;
			p.stamps[i][2] = r;
			p.stamps[i][3] = depth;
			p.stampParams[i][0] = 0.45f;
			p.stampParams[i][1] = 1.0f;
			p.stampParams[i][2] = kind;
			p.stampParams[i][3] = rim;
			p.stampShape[i][1] = 1.0f;
			p.stampShape[i][2] = halfWidth;
			p.stampShape[i][3] = 1.0f;
		}
		Clipmap::FillStampBounds(p, static_cast<uint32_t>(a_count));
		for (uint32_t level = 0; level < kLevels; ++level) {
			const float cell = Clipmap::CellSizeFor(level);
			a_levels[level] = p;
			a_levels[level].window[0] = std::floor(px / cell);
			a_levels[level].window[1] = std::floor(py / cell);
			a_levels[level].window[2] = cell;
			a_levels[level].coarse[0] = level + 1 < kLevels ? 1.0f : 0.0f;
			a_levels[level].coarse[1] = Clipmap::WorldSizeFor(level + 1);
			a_levels[level].coarse[2] = 1.0f;
			a_levels[level].coarse[3] = p.control[2] - 2.0f;
		}
	}

	void Timing(Gpu& a_gpu, Side& a_old, Side& a_new, bool decayFormats = false, bool snapshot = false)
	{
		struct Case
		{
			int         count;
			int         mode;
			const char* name;
		};
		const Case cases[] = {
			{ 0, 0, "0 stamps" },
			{ 8, 0, "8 small" },
			{ 16, 0, "16 small" },
			{ 32, 0, "32 small" },
			{ 64, 0, "64 small" },
			{ 64, 1, "64 interior mix (40 melts r20-60)" },
			{ 64, 2, "64 incl. six r=256 melts" },
			{ 64, 3, "64 small, 5000 u away (all culled)" },
		};
		constexpr int kRounds = 10;
		constexpr int kWarmup = 10;
		constexpr int kFrames = 30;

		std::vector<double> times[std::size(cases)][2];
		for (int round = 0; round < kRounds; ++round) {
			for (size_t c = 0; c < std::size(cases); ++c) {
				Params levels[kLevels];
				TimingParams(cases[c].count, cases[c].mode, levels);
				for (int which = 0; which < 2; ++which) {
					Side& side = (round + which) % 2 ? a_new : a_old;
					auto& out = times[c][&side == &a_new];
					a_gpu.Clear(side);
					for (int frame = 0; frame < kWarmup + kFrames; ++frame) {
						const double ms = a_gpu.TimedUpdate(side, levels, snapshot && &side == &a_new);
						if (frame >= kWarmup && ms >= 0.0) {
							out.push_back(ms);
						}
					}
				}
			}
		}

		std::printf("\nGPU time per frame, both levels (median of %d rounds x %d frames; p10-p90)\n", kRounds, kFrames);
		std::printf("  %-36s %22s %22s %8s\n", "case", snapshot ? "current in-place" : decayFormats ? "current R8G8" : "perf-base", snapshot ? (a_new.pingPong ? "current ping-pong" : "current snapshot") : decayFormats ? "current R16G16" : "culled", "speedup");
		for (size_t c = 0; c < std::size(cases); ++c) {
			double median[2];
			double p10[2];
			double p90[2];
			for (int i = 0; i < 2; ++i) {
				auto& v = times[c][i];
				std::sort(v.begin(), v.end());
				median[i] = v[v.size() / 2];
				p10[i] = v[v.size() / 10];
				p90[i] = v[v.size() * 9 / 10];
			}
			std::printf("  %-36s %6.3f ms (%5.3f-%5.3f) %6.3f ms (%5.3f-%5.3f) %7.2fx\n", cases[c].name, median[0],
				p10[0], p90[0], median[1], p10[1], p90[1], median[0] / median[1]);
		}
	}
}

int main(int a_argc, char** a_argv)
{
	int         frames = 320;
	uint32_t    seed = 20260923u;
	bool        warp = false;
	bool        timing = true;
	bool        decayFormats = false;
	bool        snapshot = false;
	bool        pingPong = false;
	std::string asmDir;
	for (int i = 1; i < a_argc; ++i) {
		const std::string arg = a_argv[i];
		if (arg == "--frames" && i + 1 < a_argc) {
			frames = std::atoi(a_argv[++i]);
		} else if (arg == "--seed" && i + 1 < a_argc) {
			seed = static_cast<uint32_t>(std::strtoul(a_argv[++i], nullptr, 10));
		} else if (arg == "--repose-pingpong") {
			snapshot = true; pingPong = true;
		} else if (arg == "--repose-snapshot") {
			snapshot = true;
		} else if (arg == "--decay-formats") {
			decayFormats = true;
		} else if (arg == "--warp") {
			warp = true;
		} else if (arg == "--no-timing") {
			timing = false;
		} else if (arg == "--asm" && i + 1 < a_argc) {
			asmDir = a_argv[++i];
		} else {
			std::fprintf(stderr, "usage: ClipmapCSBench [--frames N] [--seed S] [--warp] [--no-timing] [--asm DIR] [--decay-formats] [--repose-snapshot] [--repose-pingpong]\n");
			return 2;
		}
	}

	try {
		const auto baseline = Compile(BaselineSource(), "perf-base shader");
		const auto current = Compile(CurrentSource(), "current shader");
		CheckLayout(current.Get());
		const auto baselineExact = Compile(WithoutNeighbourRace(BaselineSource()), "perf-base shader (race-free)");
		const auto currentExact = Compile(WithoutNeighbourRace(CurrentSource()), "current shader (race-free)");
		std::puts("PASS compiled perf-base and current update shaders (cs_5_0, O3, MAX_STAMPS=64)");
		if (!asmDir.empty()) {
			WriteDisassembly(baseline.Get(), asmDir + "/clipmap_update_perf_base.asm");
			WriteDisassembly(current.Get(), asmDir + "/clipmap_update_current.asm");
		}

		Gpu  gpu(warp);
		if (snapshot) {

			std::puts(pingPong ? "Repose ping-pong: R16G16 metadata, one reusable 16 MiB R32 texture, no copies" :
				"Repose snapshot: R16G16 metadata on both sides; one reusable 16 MiB R32 texture, two copies/frame");
			const auto pingShader = Compile(Clipmap::UpdateShaderSource(), "shipping ping-pong shader");
			if (pingPong) {
				Side reference, swapped;
				gpu.CreateSide(reference, currentExact.Get(), Clipmap::kMetadataFormat);
				gpu.CreateSide(swapped, pingShader.Get(), Clipmap::kMetadataFormat);
				swapped.pingPong = true;
				RunFrames(gpu, reference, swapped, "snapshot vs shipping ping-pong, R16G16", frames, seed, true, true, true);
			}
			if (!timing || warp || SkyrimRunning()) {
				std::puts("Timing skipped (disabled, WARP, or Skyrim running)");
				return 0;
			}
			Side inPlace, immutable;
			gpu.CreateSide(inPlace, current.Get(), Clipmap::kMetadataFormat);
			gpu.CreateSide(immutable, pingPong ? pingShader.Get() : currentExact.Get(), Clipmap::kMetadataFormat);
			immutable.pingPong = pingPong;
			Timing(gpu, inPlace, immutable, false, true);
			return 0;
		}
		if (decayFormats) {

			std::puts("Decay format benchmark: current shader on both sides, two 2048-square levels; +16 MiB");
			if (!timing || warp || SkyrimRunning()) {
				std::puts("Timing unavailable: enable timing on hardware with Skyrim closed");
				return 2;
			}
			Side byteMetadata, preciseMetadata;
			gpu.CreateSide(byteMetadata, current.Get());
			gpu.CreateSide(preciseMetadata, current.Get(), Clipmap::kMetadataFormat);
			Timing(gpu, byteMetadata, preciseMetadata, true);
			std::puts("Format timing complete; lifetime correctness is checked by StampSurfaceTest");
			return 0;
		}
		Side old;
		Side culled;
		gpu.CreateSide(old, baselineExact.Get());
		gpu.CreateSide(culled, currentExact.Get());
		RunFrames(gpu, old, culled, "bit-exact, repose on, neighbours from a pre-dispatch copy", frames, seed, true,
			true, true);

		Side oldShipping;
		Side culledShipping;
		gpu.CreateSide(oldShipping, baseline.Get());
		gpu.CreateSide(culledShipping, current.Get());
		RunFrames(gpu, oldShipping, culledShipping, "bit-exact, shipping shaders, repose off", frames, seed + 1,
			false, false, true);

		Side oldAgain;
		gpu.CreateSide(oldAgain, baseline.Get());
		RunFrames(gpu, oldShipping, oldAgain, "perf-base against itself, shipping, repose on", std::min(frames, 60),
			seed + 2, true, false, false);
		RunFrames(gpu, oldShipping, culledShipping, "perf-base against current, shipping, repose on",
			std::min(frames, 60), seed + 2, true, false, false);

		if (!timing) {
			std::puts("Timing skipped (--no-timing)");
		} else if (warp) {
			std::puts("Timing skipped (WARP)");
		} else if (SkyrimRunning()) {
			std::puts("Timing skipped: SkyrimSE.exe is running on this GPU");
		} else {
			Timing(gpu, oldShipping, culledShipping);
		}

		std::puts("\nALL PASS");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
