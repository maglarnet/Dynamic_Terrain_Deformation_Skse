// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "Clipmap.h"
#include "Globals.h"
#include "Hooks.h"
#include "ImpactPatterns.h"
#include "MagicImpacts.h"
#include "Settings.h"
#include "ShaderRegistry.h"
#include "SnowCoverage.h"
#include "StampShapes.h"
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
		// First line of the slow path, and deliberately unconditional: this
		// mod's log stopped right after "loading" in game once, and there was
		// no way to tell from the outside whether the messages arrived, the
		// settings file failed, or the code was never reached.  One line here
		// answers all three, so it stays.
		logger::info("Data loaded - the slow path has begun");

		// Build identity, printed before anything can fail.
		//
		// This exists because the build shipped revisions whose only change
		// was to numbers inside ImpactPatterns.h, and when they failed to
		// show up in game there was no way to tell whether the DLL had not
		// been loaded or whether the numbers had simply been too small to
		// see.  Every other check in the project - md5 of the build output,
		// timestamp, file comparison - can only prove that a *file* is the
		// one on disk.  This line is the only one that proves the *running
		// code* is that file, because it can only appear if this translation
		// unit was compiled.  Keep it, and bump the tag when a change is
		// meant to be visible.
		logger::info("Build tag: {}", std::string_view{ "0.1.0.2" });

		Settings::Load();

		Surfaces::LoadProfiles();

		// The ring constants travel in a header, so they are baked into the
		// binary and cannot be read out of an INI.  Printing them here is
		// what makes "did my change reach the game" answerable from the log
		// alone rather than by inferring it from a screenshot.
		{
			const auto ring = ImpactPatterns::ExplosionRingConstants();
			logger::info(
				"Explosion ring: heaps={} distance={:.3f} size={:.3f} fill={:.3f} "
				"cap={:.3f} wobble={:.3f} height={:.3f} rimBulge={:.3f}",
				ring.heaps, ring.distance, ring.size, ring.fill, ring.cap,
				ring.wobble, ring.height,
				static_cast<double>(Settings::explosionImpactRimBulge));
		}

		// Snow particle parameters, printed for the same reason the ring
		// constants are: they are read from the INI, and a player who edits
		// them has no way to tell a value that was ignored from one that was
		// too small to see.  Nothing printed these before, so every particle
		// change had to be judged from a screenshot.
		logger::info(
			"Snow sparkle: enabled={} rate={:.0f}/s size={:.2f} life={:.2f}s "
			"throw={:.0f} rise={:.0f} gravity={:.0f} brightness={:.2f} opacity={:.2f}",
			Settings::snowSparkle, static_cast<double>(Settings::snowSparkleRate),
			static_cast<double>(Settings::snowSparkleSize),
			static_cast<double>(Settings::snowSparkleLife),
			static_cast<double>(Settings::snowSparkleThrow),
			static_cast<double>(Settings::snowSparkleRise),
			static_cast<double>(Settings::snowSparkleGravity),
			static_cast<double>(Settings::snowSparkleBrightness),
			static_cast<double>(Settings::snowSparkleOpacity));

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

		if (Settings::enableStampShapes) {
			StampShapes::Initialize();

			StampShapes::RequestAnalysis();
		}

		Hooks::Install();
		MagicImpacts::Install();
	}
}

SKSEPluginInfo(
	.Version = REL::Version{ 0, 1, 0, 2 },
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
			logger::info("Received the data-loaded message");
			OnDataLoaded();
			logger::info("Data-loaded handling returned");
		}
		if (a_msg && (a_msg->type == SKSE::MessagingInterface::kPreLoadGame ||
			a_msg->type == SKSE::MessagingInterface::kPostLoadGame ||
			a_msg->type == SKSE::MessagingInterface::kNewGame)) {
			MagicImpacts::Reset();

		}
	});

	return true;
}
