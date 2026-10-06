// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "ShaderRegistry.h"
#include "Tessellation.h"
#include "TerrainProcessingGuard.h"

#include <shared_mutex>
#include <unordered_map>

#include <dxgi.h>
#include <MinHook.h>

namespace ShaderRegistry
{
	namespace
	{

		constexpr size_t kCreateVertexShaderSlot = 12;

		using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(
			ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);

		CreateVertexShaderFn g_original{ nullptr };

		std::shared_mutex g_lock;

		size_t g_bytes{ 0 };

		std::unordered_map<ID3D11VertexShader*, std::vector<uint8_t>> g_bytecode;

		bool g_installed{ false };
		using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
		DrawFn g_nativeDraw{};
		void STDMETHODCALLTYPE NativeDraw(ID3D11DeviceContext* context, UINT count, UINT start)
		{
			TerrainProcessingGuard guard(context);
			g_nativeDraw(context, count, start);
		}
		void InstallProcessingGuard(ID3D11DeviceContext* renderer)
		{
			if (g_nativeDraw) { return; }
			ID3D11Device* device{}; renderer->GetDevice(&device);
			ID3D11DeviceContext* native{};
			if (device) { device->GetImmediateContext(&native); device->Release(); }
			if (!native) { logger::error("Terrain processing guard: native context unavailable"); return; }
			auto** table = *reinterpret_cast<void***>(native);
			const bool wrapped = table != *reinterpret_cast<void***>(renderer);
			if (wrapped) {
				const auto init = MH_Initialize();
				bool installed{};
				if (init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED) {
					void* target = table[13];
					if (MH_CreateHook(target, reinterpret_cast<void*>(&NativeDraw), reinterpret_cast<void**>(&g_nativeDraw)) == MH_OK) {
						installed = MH_EnableHook(target) == MH_OK;
						if (!installed) { MH_RemoveHook(target); g_nativeDraw = nullptr; }
					}
				}
				if (installed) { logger::info("Terrain processing guard: native Draw hook installed (independent of logging)"); }
				else { logger::error("Terrain processing guard: native Draw hook installation failed"); }
			}
			native->Release();
		}
		using SetVSFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
		SetVSFn g_setVS{};
		using SetRSFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11RasterizerState*);
		SetRSFn g_setRS{};
		void STDMETHODCALLTYPE SetRS(ID3D11DeviceContext* context, ID3D11RasterizerState* state)
		{
			g_setRS(context, Tessellation::RasterizerFor(context, state));
		}
		void InstallRasterObserver(ID3D11DeviceContext* context)
		{
			if (g_setRS || !context) { return; }

			auto** table = *reinterpret_cast<void***>(context);
			DWORD protection{};
			if (VirtualProtect(&table[43], sizeof(void*), PAGE_READWRITE, &protection)) {
				g_setRS = reinterpret_cast<SetRSFn>(table[43]);
				table[43] = reinterpret_cast<void*>(&SetRS);
				VirtualProtect(&table[43], sizeof(void*), protection, &protection);
			} else { logger::error("Terrain blending: raster observer installation failed"); }
		}
		void STDMETHODCALLTYPE SetVS(ID3D11DeviceContext* context, ID3D11VertexShader* shader,
			ID3D11ClassInstance* const* classes, UINT count)
		{
			g_setVS(context, shader, classes, count);
			Tessellation::VertexShaderBound(context, shader);
		}
		void InstallVSObserver(ID3D11DeviceContext* context)
		{
			if (g_setVS || !context) { return; }

			auto** table = *reinterpret_cast<void***>(context);
			DWORD protection{};
			if (VirtualProtect(&table[11], sizeof(void*), PAGE_READWRITE, &protection)) {
				g_setVS = reinterpret_cast<SetVSFn>(table[11]);
				table[11] = reinterpret_cast<void*>(&SetVS);
				VirtualProtect(&table[11], sizeof(void*), protection, &protection);
			} else { logger::error("Terrain blending: VS observer installation failed"); }
		}

		HRESULT STDMETHODCALLTYPE CreateVertexShaderDetour(ID3D11Device* a_self,
			const void* a_bytecode, SIZE_T a_length, ID3D11ClassLinkage* a_linkage,
			ID3D11VertexShader** a_shader)
		{
			const HRESULT hr =
				g_original(a_self, a_bytecode, a_length, a_linkage, a_shader);

			if (FAILED(hr) || !a_shader || !*a_shader || !a_bytecode || a_length == 0) {
				return hr;
			}

			const auto* bytes = static_cast<const uint8_t*>(a_bytecode);

			const std::unique_lock lock(g_lock);
			const auto [it, inserted] =
				g_bytecode.try_emplace(*a_shader, bytes, bytes + a_length);
			if (inserted) {
				(*a_shader)->AddRef();
				g_bytes += a_length;
			} else {

				g_bytes += a_length - it->second.size();
				it->second.assign(bytes, bytes + a_length);
			}

			return hr;
		}
	}

	namespace
	{
		using CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE,
			HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
			const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
			D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

		CreateDeviceAndSwapChainFn g_originalCreateDevice{ nullptr };

		HRESULT WINAPI CreateDeviceAndSwapChainDetour(IDXGIAdapter* a_adapter,
			D3D_DRIVER_TYPE a_driverType, HMODULE a_software, UINT a_flags,
			const D3D_FEATURE_LEVEL* a_featureLevels, UINT a_featureLevelCount,
			UINT a_sdkVersion, const DXGI_SWAP_CHAIN_DESC* a_swapChainDesc,
			IDXGISwapChain** a_swapChain, ID3D11Device** a_device,
			D3D_FEATURE_LEVEL* a_featureLevel, ID3D11DeviceContext** a_context)
		{
			const HRESULT hr = g_originalCreateDevice(a_adapter, a_driverType, a_software,
				a_flags, a_featureLevels, a_featureLevelCount, a_sdkVersion,
				a_swapChainDesc, a_swapChain, a_device, a_featureLevel, a_context);

			if (SUCCEEDED(hr) && a_device && *a_device) {
				Install(*a_device);
			}

			return hr;
		}

		bool PatchImport(const char* a_module, const char* a_function, void* a_detour,
			void** a_original)
		{
			auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
			if (!base) {
				return false;
			}

			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
				return false;
			}

			const auto* nt =
				reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) {
				return false;
			}

			const auto& directory =
				nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
			if (directory.VirtualAddress == 0) {
				return false;
			}

			for (auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
					 base + directory.VirtualAddress);
				 descriptor->Name != 0; ++descriptor) {
				const auto* name = reinterpret_cast<const char*>(base + descriptor->Name);
				if (_stricmp(name, a_module) != 0) {
					continue;
				}

				if (descriptor->OriginalFirstThunk == 0) {
					continue;
				}

				auto* named = reinterpret_cast<IMAGE_THUNK_DATA*>(
					base + descriptor->OriginalFirstThunk);
				auto* bound =
					reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);

				for (; named->u1.AddressOfData != 0; ++named, ++bound) {
					if (IMAGE_SNAP_BY_ORDINAL(named->u1.Ordinal)) {
						continue;
					}

					const auto* imported = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
						base + named->u1.AddressOfData);
					if (std::strcmp(imported->Name, a_function) != 0) {
						continue;
					}

					DWORD previous = 0;
					if (!VirtualProtect(&bound->u1.Function, sizeof(void*),
							PAGE_READWRITE, &previous)) {
						return false;
					}

					*a_original = reinterpret_cast<void*>(bound->u1.Function);
					bound->u1.Function = reinterpret_cast<ULONGLONG>(a_detour);

					VirtualProtect(
						&bound->u1.Function, sizeof(void*), previous, &previous);
					return true;
				}
			}

			return false;
		}
	}

	bool InstallEarly()
	{
		if (g_originalCreateDevice) {
			return true;
		}

		return PatchImport("d3d11.dll", "D3D11CreateDeviceAndSwapChain",
			reinterpret_cast<void*>(&CreateDeviceAndSwapChainDetour),
			reinterpret_cast<void**>(&g_originalCreateDevice));
	}

	bool InstallContextObservers(ID3D11DeviceContext* a_context)
	{
		if (!a_context) { return false; }
		InstallVSObserver(a_context);
		InstallRasterObserver(a_context);
		InstallProcessingGuard(a_context);
		logger::info("Terrain blending observers: renderer context={} VS installed={} raster installed={}",
			static_cast<const void*>(a_context), g_setVS != nullptr, g_setRS != nullptr);
		return g_setVS && g_setRS;
	}

	bool Install(ID3D11Device* a_device)
	{
		if (g_installed) {
			return true;
		}
		if (!a_device) {
			return false;
		}

		auto** vtable = *reinterpret_cast<void***>(a_device);
		if (!vtable) {
			return false;
		}

		void** slot = &vtable[kCreateVertexShaderSlot];

		DWORD previous = 0;
		if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) {
			logger::error("ShaderRegistry: could not unprotect the device vtable");
			return false;
		}

		{
			const std::unique_lock lock(g_lock);
			g_bytecode.reserve(8192);
		}

		g_original = reinterpret_cast<CreateVertexShaderFn>(*slot);
		*slot = reinterpret_cast<void*>(&CreateVertexShaderDetour);

		VirtualProtect(slot, sizeof(void*), previous, &previous);

		g_installed = g_original != nullptr;
		return g_installed;
	}

	Bytecode For(ID3D11VertexShader* a_shader)
	{
		if (!a_shader) {
			return {};
		}

		const std::shared_lock lock(g_lock);
		const auto             it = g_bytecode.find(a_shader);
		if (it == g_bytecode.end()) {
			return {};
		}

		return { it->second.data(), it->second.size() };
	}

	size_t Count()
	{
		const std::shared_lock lock(g_lock);
		return g_bytecode.size();
	}

	size_t Bytes()
	{
		const std::shared_lock lock(g_lock);
		return g_bytes;
	}
}
