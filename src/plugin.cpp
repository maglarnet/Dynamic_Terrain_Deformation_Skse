// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "Clipmap.h"
#include "Globals.h"
#include "Hooks.h"
#include "MagicImpacts.h"
#include "Settings.h"
#include "ShaderRegistry.h"
#include "SnowCoverage.h"
#include "Shelter.h"
#include "SurfaceProfiles.h"

namespace
{
	void InitLogging()
	{
		auto path = SKSE::log::log_directory();
		if (!path) {
			return;
		}

		*path /= std::format("{}.log", SKSE::PluginDeclaration::GetSingleton()->GetName());

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));

		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);

		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
	}

	void OnDataLoaded()
	{
		Settings::Load();

		Surfaces::LoadProfiles();

		if (!globals::Initialize()) {
			logger::error("D3D device/context unavailable - renderer work disabled");
			return;
		}

		logger::info(
			"D3D ready: device={} context={}",
			static_cast<const void*>(globals::d3d::device),
			static_cast<const void*>(globals::d3d::context));

		if (!ShaderRegistry::Install(globals::d3d::device)) {
			logger::error("ShaderRegistry: CreateVertexShader not hooked - draws under a "
						  "replaced vertex shader cannot be resolved");
		}
		if (!ShaderRegistry::InstallContextObservers(globals::d3d::context)) {
			logger::error("Terrain blending: renderer context observers unavailable");
		}
		logger::info("ShaderRegistry: {} vertex shader(s) recorded so far, {:.1f} MB of "
					 "bytecode",
			ShaderRegistry::Count(),
			static_cast<double>(ShaderRegistry::Bytes()) / (1024.0 * 1024.0));

		if (Settings::useClipmap && !Clipmap::Initialize()) {
			logger::error("Clipmap unavailable - displacement will fall back to the wave");
		}

		if (Settings::enableSnowRaise && !SnowCoverage::Initialize()) {
			logger::error("SnowCoverage unavailable - the snow raise will lift nothing");
		}


		Hooks::Install();
		MagicImpacts::Install();
	}
}

SKSEPluginInfo(
	.Version = REL::Version{ 0, 1, 0, 0 },
	.Name = "NMN_DeformableTerrain"sv,
	.Author = "maglarnet"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitLogging();
	SKSE::Init(a_skse);

	SKSE::AllocTrampoline(1 << 8);

	const auto* decl = SKSE::PluginDeclaration::GetSingleton();
	logger::info("{} v{} loading", decl->GetName(), decl->GetVersion().string("."sv));

	if (!ShaderRegistry::InstallEarly()) {
		logger::warn("ShaderRegistry: could not intercept device creation - shaders made "
					 "before the device is handed to us will be invisible");
	}

	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging) {
		logger::error("No messaging interface");
		return false;
	}

	messaging->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		if (a_msg && a_msg->type == SKSE::MessagingInterface::kDataLoaded) {
			OnDataLoaded();
		}
		if (a_msg && (a_msg->type == SKSE::MessagingInterface::kPreLoadGame ||
			a_msg->type == SKSE::MessagingInterface::kPostLoadGame ||
			a_msg->type == SKSE::MessagingInterface::kNewGame)) {
			MagicImpacts::Reset();
			Clipmap::InvalidateGather();
			Shelter::InvalidateCPU();

		}
	});

	return true;
}
