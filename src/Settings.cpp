// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "Clipmap.h"
#include "Settings.h"
#include "Shelter.h"
#include "SnowCoverage.h"
#include "SurfaceProfiles.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace Settings
{
	namespace
	{
		constexpr auto kPath = "Data/SKSE/Plugins/NMN_DeformableTerrain.ini";

		std::filesystem::file_time_type g_lastWrite{};
		float                           g_pollTimer{ 0.0f };

		std::filesystem::file_time_type WriteTimeOrZero()
		{
			std::error_code ec;
			const auto      when = std::filesystem::last_write_time(kPath, ec);
			auto            newest = ec ? std::filesystem::file_time_type{} : when;

			const auto profiles = Surfaces::ProfilesWriteTime();
			return profiles > newest ? profiles : newest;
		}

		std::string Trim(std::string_view a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_text.find_last_not_of(" \t\r\n");
			return std::string(a_text.substr(first, last - first + 1));
		}

		float AsFloat(const std::string& a_key, const std::string& a_value, float a_fallback)
		{
			try {
				return std::stof(a_value);
			} catch (const std::exception&) {
				logger::warn("{}: '{}' is not a number, using {}", a_key, a_value, a_fallback);
				return a_fallback;
			}
		}

		float Clamped(const std::string& a_key, float a_value, float a_low, float a_high)
		{
			const float out = std::clamp(a_value, a_low, a_high);
			if (out != a_value) {
				logger::warn("{}: {} is outside {} to {}, using {}", a_key, a_value, a_low,
					a_high, out);
			}
			return out;
		}

		std::string AsLower(std::string a_value)
		{
			std::transform(a_value.begin(), a_value.end(), a_value.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return a_value;
		}

		bool AsBool(const std::string& a_value)
		{
			const std::string lowered = AsLower(a_value);
			return lowered == "1" || lowered == "true" || lowered == "yes";
		}

		std::string AsKeywords(const std::string& a_value, const std::string& a_fallback)
		{
			std::string out;
			out.reserve(a_value.size());

			for (const char c : AsLower(a_value)) {
				if (c != ' ' && c != '\t') {
					out += c;
				}
			}

			return out.empty() ? a_fallback : out;
		}

		void ParseResponse(const std::string& a_key, const std::string& a_value,
			Surfaces::Response& a_out)
		{
			float*       fields[] = { &a_out.depthScale, &a_out.radiusScale, &a_out.shoulder,
				&a_out.decayScale, &a_out.rimScale, &a_out.print, &a_out.clearanceScale };
			const float  lower[] = { 0.0f, 0.05f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
			const float  upper[] = { 8.0f, 8.0f, 0.95f, 2.0f, 4.0f, 1.0f, 256.0f };

			const char*  names[] = { "depth", "radius", "shoulder", "decay", "rim",
				"print", "clearance" };
			const size_t count = std::size(fields);

			size_t index = 0;
			size_t start = 0;
			while (index < count && start <= a_value.size()) {
				const auto comma = a_value.find(',', start);
				const auto end = comma == std::string::npos ? a_value.size() : comma;

				const auto piece = Trim(std::string_view(a_value).substr(start, end - start));
				if (!piece.empty()) {

					*fields[index] = Clamped(std::string(a_key) + " " + names[index],
						AsFloat(a_key, piece, *fields[index]), lower[index], upper[index]);
				}

				++index;
				if (comma == std::string::npos) {
					break;
				}
				start = comma + 1;
			}
		}

		Surfaces::Response& Response(Surfaces::Type a_type)
		{
			return surfaceResponse[static_cast<size_t>(a_type)];
		}

	}

	namespace
	{

		bool ApplyTessellationSetting(const std::string& key, const std::string& value)
		{
		if (key == "EnableTessellation") {
			enableTessellation = AsBool(value);
		} else if (key == "TessellationWinding") {
			tessellationWinding = value == "ccw" ? "ccw" : "cw";
		} else if (key == "LogGeneratedShaders") {
			logGeneratedShaders = AsBool(value);
		} else if (key == "LogTerrainBlendDraws") {
			logTerrainBlendDraws = AsBool(value);
		} else if (key == "TerrainBlendingCompatibility") {
			terrainBlendingCompatibility = AsBool(value);
		} else if (key == "TerrainBlendingCullBackfaces") {
			terrainBlendingCullBackfaces = AsBool(value);
		} else if (key == "EnableDepthPass") {
			enableDepthPass = AsBool(value);
		} else if (key == "TessellationMaxFactor") {
			tessellationMaxFactor = std::clamp(AsFloat(key, value, 64.0f), 1.0f, 64.0f);
		} else if (key == "TessellationTargetSpacing") {
			tessellationTargetSpacing = std::max(AsFloat(key, value, 2.0f), 0.01f);
		} else if (key == "TessellationRaiseSpacing") {
			tessellationRaiseSpacing =
				Clamped(key, AsFloat(key, value, 64.0f), 0.0f, 4096.0f);
		} else if (key == "TessellationBlanketSpacing") {
			tessellationBlanketSpacing =
				Clamped(key, AsFloat(key, value, 8.0f), 0.0f, 4096.0f);
		} else if (key == "DebugWorldZOffset") {
			debugWorldZOffset = AsFloat(key, value, 0.0f);
		} else if (key == "DebugWaveAmplitude") {
			debugWaveAmplitude = AsFloat(key, value, 0.0f);
			} else {
				return false;
			}
			return true;
		}

		bool ApplyMarkSetting(const std::string& key, const std::string& value)
		{
		if (key == "UseClipmap") {
			useClipmap = AsBool(value);
		} else if (key == "ClipmapLevels") {
			clipmapLevels = static_cast<uint32_t>(
				Clamped(key, AsFloat(key, value, 2.0f), 1.0f, 2.0f));
		} else if (key == "ClipmapSeedSmoothing") {
			clipmapSeedSmoothing = Clamped(key, AsFloat(key, value, 1.0f), 0.0f, 16.0f);
		} else if (key == "EnableTessellationBounds") {
			enableTessellationBounds = AsBool(value);
		} else if (key == "TessellationBoundDepth") {
			tessellationBoundDepth = Clamped(key, AsFloat(key, value, 0.25f), 0.0f, 256.0f);
		} else if (key == "StampRadiusScale") {
			stampRadiusScale = std::clamp(AsFloat(key, value, 1.0f), 0.05f, 16.0f);
		} else if (key == "StampFootReach") {
			stampFootReach = std::max(AsFloat(key, value, 48.0f), 0.0f);
		} else if (key == "StampGroundClearance") {
			stampGroundClearance = Clamped(key, AsFloat(key, value, 0.1f), 0.0f, 1024.0f);
		} else if (key == "StampDepth") {
			stampDepth = AsFloat(key, value, 6.0f);
		} else if (key == "StampDecayPerSecond") {
			stampDecayPerSecond = std::clamp(AsFloat(key, value, 0.99f), 0.0f, 1.0f);
		} else if (key == "EnableWeather") {
			enableWeather = AsBool(value);
		} else if (key == "WeatherSoftenHours") {
			weatherSoftenHours = Clamped(key, AsFloat(key, value, 1.0f), 0.05f, 720.0f);
		} else if (key == "WeatherFirmHours") {
			weatherFirmHours = Clamped(key, AsFloat(key, value, 1.0f), 0.05f, 720.0f);
		} else if (key == "WeatherSnowSoften") {
			weatherSnowSoften = Clamped(key, AsFloat(key, value, 1.0f), 0.0f, 16.0f);
		} else if (key == "WeatherRainSoften") {
			weatherRainSoften = Clamped(key, AsFloat(key, value, 8.0f), 0.0f, 16.0f);
		} else if (key == "WeatherCloudyFirmScale") {
			weatherCloudyFirmScale = Clamped(key, AsFloat(key, value, 0.25f), 0.0f, 1.0f);
		} else if (key == "WeatherSoftDepthScale") {
			weatherSoftDepthScale = Clamped(key, AsFloat(key, value, 16.0f), 0.0f, 16.0f);
		} else if (key == "WeatherFirmDepthScale") {
			weatherFirmDepthScale = Clamped(key, AsFloat(key, value, 0.70f), 0.0f, 16.0f);
		} else if (key == "WeatherSoftDecayScale") {
			weatherSoftDecayScale = Clamped(key, AsFloat(key, value, 0.95f), 0.0f, 16.0f);
		} else if (key == "WeatherFirmDecayScale") {
			weatherFirmDecayScale = Clamped(key, AsFloat(key, value, 1.05f), 0.0f, 16.0f);
		} else if (key == "WeatherFillPerSecond") {
			weatherFillPerSecond = Clamped(key, AsFloat(key, value, 0.06f), 0.0f, 0.99f);
		} else if (key == "LogWeather") {
			logWeather = AsBool(value);
		} else if (key == "EnableSnowRaise") {
			enableSnowRaise = AsBool(value);
		} else if (key == "SnowRaiseHeight") {
			snowRaiseHeight = Clamped(key, AsFloat(key, value, 35.0f), 0.0f, 512.0f);
		} else if (key == "SnowRaiseWeather") {
			snowRaiseWeather = AsBool(value);
		} else if (key == "SnowRaiseDistance") {

			snowRaiseDistance = Clamped(key, AsFloat(key, value, 7936.0f), 0.0f,
				SnowCoverage::kWorldSize * 0.5f - SnowCoverage::kTexelSize * 4.0f);
		} else if (key == "SnowRaiseFadeBand") {
			snowRaiseFadeBand = Clamped(key, AsFloat(key, value, 4000.0f), 1.0f, 8192.0f);
		} else if (key == "SnowCoverageBudget") {
			snowCoverageBudget = static_cast<int>(
				Clamped(key, AsFloat(key, value, 2048), 1.0f, 65536.0f));
		} else if (key == "EnableStaticProbe") {
			enableStaticProbe = AsBool(value);
		} else if (key == "StaticProbeOffset") {
			staticProbeOffset = Clamped(key, AsFloat(key, value, 0.0f), -512.0f, 2048.0f);
		} else if (key == "StaticProbeMinTriangles") {
			staticProbeMinTriangles = static_cast<int>(
				Clamped(key, AsFloat(key, value, 2000), 0.0f, 65535.0f));
		} else if (key == "LogStaticProbe") {
			logStaticProbe = AsBool(value);
		} else if (key == "SnowTrenchDepth") {
			snowTrenchDepth = Clamped(key, AsFloat(key, value, 0.5f), 0.0f, 4.0f);
		} else if (key == "SnowTrenchCoverage") {
			snowTrenchCoverage = Clamped(key, AsFloat(key, value, 0.95f), 0.5f, 0.95f);
		} else if (key == "SnowGroundFloor") {
			snowGroundFloor = AsBool(value);
		} else if (key == "SnowGroundBite") {

			snowGroundBite = Clamped(key, AsFloat(key, value, 0.0f), 0.0f, 512.0f);
		} else if (key == "SnowSeamTexels") {
			snowSeamTexels = static_cast<int>(
				Clamped(key, AsFloat(key, value, 1), 0.0f,
					static_cast<float>(SnowCoverage::kTexels / 4)));
		} else if (key == "EnableShelter") {
			enableShelter = AsBool(value);
		} else if (key == "ShelterBudget") {
			shelterBudget = static_cast<int>(
				Clamped(key, AsFloat(key, value, 48), 1.0f, 4096.0f));
		} else if (key == "ShelterRefresh") {
			shelterRefresh = static_cast<int>(
				Clamped(key, AsFloat(key, value, 16), 0.0f, 2048.0f));
		} else if (key == "ShelterClearance") {
			shelterClearance = Clamped(key, AsFloat(key, value, 48.0f), 1.0f, 2048.0f);
		} else if (key == "ShelterHeight") {
			shelterHeight = Clamped(key, AsFloat(key, value, 1024.0f), 16.0f, 32768.0f);
		} else if (key == "ShelterMeshCap") {
			shelterMeshCap = AsBool(value);
		} else if (key == "ShelterFloorTolerance") {
			shelterFloorTolerance = Clamped(key, AsFloat(key, value, 8.0f), 1.0f, 512.0f);
		} else if (key == "ShelterSeamTexels") {
			shelterSeamTexels = static_cast<int>(
				Clamped(key, AsFloat(key, value, 1), 0.0f,
					static_cast<float>(Shelter::kTexels / 4)));
		} else if (key == "LogSnowCoverage") {
			logSnowCoverage = AsBool(value);
			} else {
				return false;
			}
			return true;
		}

		bool ApplyMagicSetting(const std::string& key, const std::string& value)
		{
		if (key == "EnableMagicImpacts") {
			enableMagicImpacts = AsBool(value);
		} else if (key == "EnableShoutImpacts") {
			enableShoutImpacts = AsBool(value);
		} else if (key == "EnableExplosionImpacts") {
			enableExplosionImpacts = AsBool(value);
		} else if (key == "LogMagicImpacts") {
			logMagicImpacts = AsBool(value);
		} else if (key == "HeatMaxRadius") {
			heatMaxRadius = Clamped(key, AsFloat(key, value, 256.0f), 0.0f, 8192.0f);
		} else if (key == "MagicImpactRadius") {
			magicImpactRadius = Clamped(key, AsFloat(key, value, 36.0f), 0.00f, 1024.00f);
		} else if (key == "MagicImpactDepth") {
			magicImpactDepth = Clamped(key, AsFloat(key, value, 32.0f), 0.00f, 256.00f);
		} else if (key == "ShoutImpactRange") {
			shoutImpactRange = Clamped(key, AsFloat(key, value, 1200.0f), 0.00f, 4096.00f);
		} else if (key == "ShoutImpactWidth") {
			shoutImpactWidth = Clamped(key, AsFloat(key, value, 500.0f), 0.00f, 2048.00f);
		} else if (key == "ShoutImpactDepth") {
			shoutImpactDepth = Clamped(key, AsFloat(key, value, 24.0f), 0.00f, 256.00f);
		} else if (key == "ExplosionImpactRadiusScale") {
			explosionImpactRadiusScale = Clamped(key, AsFloat(key, value, 0.5f), 0.00f, 16.00f);
		} else if (key == "ExplosionImpactMaxRadius") {
			explosionImpactMaxRadius = Clamped(key, AsFloat(key, value, 256.0f), 0.00f, 4096.00f);
		} else if (key == "ExplosionImpactDepth") {
			explosionImpactDepth = Clamped(key, AsFloat(key, value, 18.0f), 0.00f, 256.00f);
		} else if (key == "MagicImpactRimScale") {
			magicImpactRimScale = Clamped(key, AsFloat(key, value, 0.1f), 0.00f, 8.00f);
		} else if (key == "MagicImpactSnowMelt") {
			magicImpactSnowMelt = Clamped(key, AsFloat(key, value, 0.3f), 0.00f, 4.00f);
		} else { return false; }
		return true;
		}

		bool ApplySurfaceSetting(const std::string& key, const std::string& value)
		{
		if (key == "DebugBlendLayer") {
			debugBlendLayer = std::clamp(static_cast<int>(AsFloat(key, value, -1)), -1, 5);
		} else if (key == "BlendFullDepth") {
			blendFullDepth = std::max(AsFloat(key, value, 4.0f), 0.01f);
		} else if (key == "EnableSurfaceMaterial") {
			enableSurfaceMaterial = AsBool(value);
		} else if (key == "EnableSurfaceClassification") {
			enableSurfaceClassification = AsBool(value);
		} else if (key == "LogSurfaceClassification") {
			logSurfaceClassification = AsBool(value);
		} else if (key == "SurfaceSnow") {
			surfaceSnow = AsKeywords(value, surfaceSnow);
		} else if (key == "SurfaceGrass") {
			surfaceGrass = AsKeywords(value, surfaceGrass);
		} else if (key == "SurfaceDirt") {
			surfaceDirt = AsKeywords(value, surfaceDirt);
		} else if (key == "SurfaceMud") {
			surfaceMud = AsKeywords(value, surfaceMud);
		} else if (key == "SurfaceSand") {
			surfaceSand = AsKeywords(value, surfaceSand);
		} else if (key == "SurfaceAsh") {
			surfaceAsh = AsKeywords(value, surfaceAsh);
		} else if (key == "SurfaceGravel") {
			surfaceGravel = AsKeywords(value, surfaceGravel);
		} else if (key == "SurfaceStone") {
			surfaceStone = AsKeywords(value, surfaceStone);
		} else if (key == "BlendStrength") {
			blendStrength = std::clamp(AsFloat(key, value, 0.45f), 0.0f, 1.0f);
		} else if (key == "SnowRevealStrength") {
			snowRevealStrength = std::clamp(AsFloat(key, value, 0.4f), 0.0f, 1.0f);
		} else if (key == "SeasonalTextureSwap") {
			seasonalTextureSwap = AsBool(value);
		} else if (key == "SnowSparkle") {
			snowSparkle = AsBool(value);
		} else if (key == "SnowSparkleRate") {
			snowSparkleRate = Clamped(key, AsFloat(key, value, 1000.0f), 0.0f, 20000.0f);
		} else if (key == "SnowSparkleFullSpeed") {
			snowSparkleFullSpeed = Clamped(key, AsFloat(key, value, 350.0f), 1.0f, 8000.0f);
		} else if (key == "SnowSparkleMinSpeed") {
			snowSparkleMinSpeed = Clamped(key, AsFloat(key, value, 40.0f), 0.0f, 8000.0f);
		} else if (key == "SnowSparkleSize") {
			snowSparkleSize = Clamped(key, AsFloat(key, value, 1.0f), 0.1f, 256.0f);
		} else if (key == "SnowSparkleLife") {
			snowSparkleLife = Clamped(key, AsFloat(key, value, 1.0f), 0.05f, 60.0f);
		} else if (key == "SnowSparkleThrow") {
			snowSparkleThrow = Clamped(key, AsFloat(key, value, 500.0f), 0.0f, 8000.0f);
		} else if (key == "SnowSparkleRise") {
			snowSparkleRise = Clamped(key, AsFloat(key, value, 80.0f), 0.0f, 8000.0f);
		} else if (key == "SnowSparkleInherit") {
			snowSparkleInherit = Clamped(key, AsFloat(key, value, 1.0f), 0.0f, 4.0f);
		} else if (key == "SnowSparkleGravity") {
			snowSparkleGravity = Clamped(key, AsFloat(key, value, 300.0f), 0.0f, 16000.0f);
		} else if (key == "SnowSparkleDrag") {
			snowSparkleDrag = Clamped(key, AsFloat(key, value, 4.2f), 0.0f, 1000.0f);
		} else if (key == "SnowSparkleSwirl") {
			snowSparkleSwirl = Clamped(key, AsFloat(key, value, 100.0f), 0.0f, 4000.0f);
		} else if (key == "SnowSparkleBrightness") {
			snowSparkleBrightness = Clamped(key, AsFloat(key, value, 0.5f), 0.0f, 16.0f);
		} else if (key == "SnowSparkleOpacity") {
			snowSparkleOpacity = Clamped(key, AsFloat(key, value, 0.5f), 0.0f, 1.0f);
		} else if (key == "SnowSparkleShape") {
			snowSparkleShape = Clamped(key, AsFloat(key, value, 1.0f), 0.0f, 1.0f);

		} else if (key == "StampFootShape") {
			stampFootShape = AsBool(value);
		} else if (key == "EnableBloodDecals") {
			enableBloodDecals = AsBool(value);
		} else if (key == "BloodDecalsIgnoreShaderIdentity") {
			bloodDecalsIgnoreShaderIdentity = AsBool(value);
		} else if (key == "BloodDecalTexturePrefixes") {
			bloodDecalTexturePrefixes = value;
		} else if (key == "AsyncShaderCompilation") {
			asyncShaderCompilation = AsBool(value);
		} else if (key == "SnowStampRimSpan") {
			snowStampRimSpan = Clamped(key, AsFloat(key, value, 0.7f), -1.0f, 16.0f);
		} else if (key == "SnowStampReposeRate") {
			snowStampReposeRate = Clamped(key, AsFloat(key, value, 0.10f), -1.0f, 1.0f);
		} else if (key == "SnowStampRimNoise") {
			snowStampRimNoise = Clamped(key, AsFloat(key, value, 0.8f), -1.0f, 8.0f);
		} else if (key == "SnowStampRimLean") {
			snowStampRimLean = Clamped(key, AsFloat(key, value, 1.0f), -1.0f, 1.0f);
		} else if (key == "SnowStampChurn") {
			snowStampChurn = Clamped(key, AsFloat(key, value, 0.80f), -1.0f, 8.0f);
		} else if (key == "StampRimLean") {
			stampRimLean = Clamped(key, AsFloat(key, value, 1.0f), 0.0f, 1.0f);
		} else if (key == "StampChurn") {
			stampChurn = Clamped(key, AsFloat(key, value, 0.45f), 0.0f, 8.0f);
		} else if (key == "StampRimNoise") {
			stampRimNoise = Clamped(key, AsFloat(key, value, 0.0f), 0.0f, 8.0f);
		} else if (key == "StampReposeRate") {
			stampReposeRate = Clamped(key, AsFloat(key, value, 0.01f), 0.0f, 1.0f);
		} else if (key == "StampSlopeLimit") {
			stampSlopeLimit = Clamped(key, AsFloat(key, value, 50.0f), 0.0f, 89.0f);
		} else if (key == "StampFootLength") {
			stampFootLength = Clamped(key, AsFloat(key, value, 1.3f), 0.1f, 16.0f);
		} else if (key == "StampFootAspect") {
			stampFootAspect = Clamped(key, AsFloat(key, value, 0.50f), 0.1f, 8.0f);
		} else if (key == "StampFootSeparation") {
			stampFootSeparation = Clamped(key, AsFloat(key, value, 0.0f), 0.0f, 512.0f);
		} else if (key == "StampShapeKeyword") {
			stampShapeKeyword = Trim(value);
		} else if (key == "StampRimHeight") {
			stampRimHeight = Clamped(key, AsFloat(key, value, 0.2f), 0.0f, 16.0f);
		} else if (key == "StampRimIndependence") {
			stampRimIndependence = Clamped(key, AsFloat(key, value, 1.0f), 0.0f, 1.0f);
		} else if (key == "StampRimSpan") {
			stampRimSpan = Clamped(key, AsFloat(key, value, 1.0f), 0.0f, 16.0f);
		} else if (key == "EnableObjectStamps") {
			enableObjectStamps = AsBool(value);
		} else if (key == "ObjectStampRadiusScale") {
			objectStampRadiusScale = Clamped(key, AsFloat(key, value, 1.0f), 0.0f, 16.0f);
		} else if (key == "ObjectStampDepthScale") {
			objectStampDepthScale = Clamped(key, AsFloat(key, value, 1.0f), 0.0f, 16.0f);
		} else if (key == "ObjectFullSizeRadius") {
			objectFullSizeRadius = Clamped(key, AsFloat(key, value, 1.0f), 1.0f, 4096.0f);
		} else if (key == "ObjectContactTolerance") {
			objectContactTolerance = Clamped(key, AsFloat(key, value, 48.0f), 0.0f, 1024.0f);
		} else if (key == "ObjectSinkLimit") {
			objectSinkLimit = Clamped(key, AsFloat(key, value, 40.0f), 0.0f, 4096.0f);
		} else if (key == "ObjectStampInterval") {
			objectStampInterval = Clamped(key, AsFloat(key, value, 0.20f), 0.0f, 60.0f);
		} else if (key == "LogObjectStamps") {
			logObjectStamps = AsBool(value);
		} else if (key == "EnableHeatSources") {
			enableHeatSources = AsBool(value);
		} else if (key == "HeatKeywords") {
			heatKeywords = AsKeywords(value, heatKeywords);
		} else if (key == "HeatRadius") {
			heatRadius = Clamped(key, AsFloat(key, value, 2.5f), 0.0f, 8192.0f);
		} else if (key == "HeatRadiusScale") {
			heatRadiusScale = Clamped(key, AsFloat(key, value, 1.0f), 0.0f, 64.0f);
		} else if (key == "HeatMeltPerSecond") {
			heatMeltPerSecond = Clamped(key, AsFloat(key, value, 0.85f), 0.0f, 1.0f);
		} else if (key == "HeatShoulder") {
			heatShoulder = Clamped(key, AsFloat(key, value, 0.20f), 0.0f, 0.95f);
		} else if (key == "HeatFromTorches") {
			heatFromTorches = AsBool(value);
		} else if (key == "HeatTorchRadius") {
			heatTorchRadius = Clamped(key, AsFloat(key, value, 40.0f), 0.0f, 8192.0f);
		} else if (key == "HeatTorchScale") {
			heatTorchScale = Clamped(key, AsFloat(key, value, 0.45f), 0.0f, 16.0f);
		} else if (key == "HeatMeltsSnow") {
			heatMeltsSnow = AsBool(value);
		} else if (key == "HeatMeltDepthScale") {
			heatMeltDepthScale = Clamped(key, AsFloat(key, value, 0.80f), 0.0f, 4.0f);
		} else if (key == "HeatFromExplosions") {
			heatFromExplosions = AsBool(value);
		} else if (key == "LogHeatSources") {
			logHeatSources = AsBool(value);
		} else if (key == "EnableLiveReload") {
			enableLiveReload = AsBool(value);
		} else if (key == "LiveReloadInterval") {
			liveReloadInterval = Clamped(key, AsFloat(key, value, 0.5f), 0.05f, 10.0f);
		} else if (key == "LogStampFeet") {
			logStampFeet = AsBool(value);
		} else if (key == "LogStampSurfaces") {
			logStampSurfaces = AsBool(value);

		} else if (key == "EnableLogging") {
			enableLogging = AsBool(value);
		} else if (key == "LogDraws") {
			logDraws = AsBool(value);
		} else if (key == "LogActorDraws") {
			logActorDraws = AsBool(value);
		} else if (key == "RouteOnlyPlayerCamera") {
			routeOnlyPlayerCamera = AsBool(value);
		} else if (key == "ResponseUnknown") {
			ParseResponse(key, value, Response(Surfaces::Type::kUnknown));
		} else if (key == "ResponseSnow") {
			ParseResponse(key, value, Response(Surfaces::Type::kSnow));
		} else if (key == "ResponseGrass") {
			ParseResponse(key, value, Response(Surfaces::Type::kGrass));
		} else if (key == "ResponseDirt") {
			ParseResponse(key, value, Response(Surfaces::Type::kDirt));
		} else if (key == "ResponseMud") {
			ParseResponse(key, value, Response(Surfaces::Type::kMud));
		} else if (key == "ResponseSand") {
			ParseResponse(key, value, Response(Surfaces::Type::kSand));
		} else if (key == "ResponseAsh") {
			ParseResponse(key, value, Response(Surfaces::Type::kAsh));
		} else if (key == "ResponseGravel") {
			ParseResponse(key, value, Response(Surfaces::Type::kGravel));
		} else if (key == "ResponseStone") {
			ParseResponse(key, value, Response(Surfaces::Type::kStone));
			} else {
				return false;
			}
			return true;
		}

		bool ApplyDiagnosticSetting(const std::string& key, const std::string& value)
		{
		if (key == "CullDistantDraws") {
			cullDistantDraws = AsBool(value);
		} else if (key == "CullMargin") {
			cullMargin = std::max(AsFloat(key, value, 256.0f), 0.0f);
		} else if (key == "RecomputeNormals") {
			recomputeNormals = AsBool(value);
		} else if (key == "NormalGradientEpsilon") {
			normalGradientEpsilon = std::max(AsFloat(key, value, 0.75f), 0.001f);
		} else if (key == "DebugFieldColour" || key == "DebugFieldColor") {
			debugFieldColour =
				static_cast<int>(Clamped(key, AsFloat(key, value, 0), 0.0f, 3.0f));
		} else if (key == "DebugWaveLength") {
			debugWaveLength = std::max(AsFloat(key, value, 256.0f), 1.0f);
		} else if (key == "EnableProfiler") {
			enableProfiler = AsBool(value);
		} else if (key == "ProfileInterval") {
			profileInterval = Clamped(key, AsFloat(key, value, 5.0f), 0.5f, 300.0f);
		} else if (key == "ProfileDrawScopes") {
			profileDrawScopes = AsBool(value);
			} else {
				return false;
			}
			return true;
		}
	}

	void ApplyLogLevel()
	{
		if (auto logger = spdlog::default_logger(); logger) {
			logger->set_level(enableLogging ? spdlog::level::info : spdlog::level::off);
		}
	}

	void Load()
	{
		std::ifstream file(kPath);
		if (!file) {

			ApplyLogLevel();
			logger::info("No {} - using defaults (tessellation disabled)", kPath);
			return;
		}

		snowStampRimSpan = snowStampReposeRate = snowStampRimNoise =
			snowStampRimLean = snowStampChurn = -1.0f;

		std::string line;
		while (std::getline(file, line)) {
			const auto trimmed = Trim(line);
			if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#' || trimmed[0] == '[') {
				continue;
			}

			const auto eq = trimmed.find('=');
			if (eq == std::string::npos) {
				continue;
			}

			const auto key = Trim(std::string_view(trimmed).substr(0, eq));

			auto       body = std::string_view(trimmed).substr(eq + 1);
			const auto comment = body.find_first_of(";#");
			if (comment != std::string_view::npos) {
				body = body.substr(0, comment);
			}
			const auto value = Trim(body);

			if (!ApplyTessellationSetting(key, value) &&
				!ApplyMarkSetting(key, value) &&
				!ApplyMagicSetting(key, value) &&
				!ApplySurfaceSetting(key, value) &&
				!ApplyDiagnosticSetting(key, value)) {

			}
		}

		ApplyLogLevel();

		logger::info(
			"Settings: EnableTessellation={} EnableDepthPass={} Winding={} MaxFactor={} "
			"TargetSpacing={} ZOffset={} Clipmap={} StampScale={} FootReach={} StampD={} Decay={} "
			"WaveAmp={} WaveLen={} "
			"Cull={} CullMargin={} BlendLayer={} Normals={} GradEps={} LogShaders={} "
			"SurfaceMaterial={} Classify={} BlendStrength={}",
			enableTessellation, enableDepthPass, tessellationWinding, tessellationMaxFactor,
			tessellationTargetSpacing, debugWorldZOffset, useClipmap, stampRadiusScale,
			stampFootReach, stampDepth, stampDecayPerSecond, debugWaveAmplitude, debugWaveLength,
			cullDistantDraws, cullMargin, debugBlendLayer, recomputeNormals, normalGradientEpsilon,
			logGeneratedShaders, enableSurfaceMaterial, enableSurfaceClassification,
			blendStrength);

		logger::info("Marks: depth={} decay={}/s | rim height={}*depth span={}*radius | "
					 "oriented={} keyword={} length={} aspect={} separation={} | "
					 "radius={}*bound reach={} clearance={} | repose={} deg",
			stampDepth, stampDecayPerSecond, stampRimHeight, stampRimSpan,
			stampFootShape, stampShapeKeyword, stampFootLength,
			stampFootAspect, stampFootSeparation, stampRadiusScale, stampFootReach,
			stampGroundClearance, stampSlopeLimit);

		if (stampGroundClearance <= 0.0f) {
			logger::info("  ground clearance is OFF - marks will be made in mid-air on a "
						 "jump, and the swinging foot will print alongside the planted one");
		}

		if (stampRimHeight <= 0.0f || stampRimSpan <= 0.0f) {
			logger::info("  the rim is OFF - both StampRimHeight and StampRimSpan must be "
						 "above 0 for material to bank around a mark");
		}

		logger::info("Weather: enabled={} | soften={}h firm={}h | snow={} rain={} cloudy={} | "
					 "depth {}..{} decay {}..{} | fill={}/s",
			enableWeather, weatherSoftenHours, weatherFirmHours, weatherSnowSoften,
			weatherRainSoften, weatherCloudyFirmScale, weatherFirmDepthScale,
			weatherSoftDepthScale, weatherFirmDecayScale, weatherSoftDecayScale,
			weatherFillPerSecond);

		if (enableWeather && weatherFillPerSecond <= 0.0f &&
			weatherSoftDepthScale == 1.0f && weatherFirmDepthScale == 1.0f &&
			weatherSoftDecayScale == 1.0f && weatherFirmDecayScale == 1.0f) {
			logger::info("  weather is ON but every consumer is neutral - the level will "
						 "integrate and change nothing");
		}

		logger::info("Tessellation budget: bounds={} | blanket spacing={} | distant spacing={} | mark spacing={} | maxfactor={}; blanket floor v1",
			enableTessellationBounds, tessellationBlanketSpacing, tessellationRaiseSpacing,
			tessellationTargetSpacing, tessellationMaxFactor);

		logger::info("Snow raise: enabled={} | height={} | weather={} | distance={} "
					 "fade={} | coverage budget={}/frame",
			enableSnowRaise, snowRaiseHeight, snowRaiseWeather, snowRaiseDistance,
			snowRaiseFadeBand, snowCoverageBudget);

			if (enableStaticProbe) {
				logger::warn("Trap 8 probe is ON: static meshes are routed through the hull "
							 "and domain stages and lifted by {} units. This is a DIAGNOSTIC - "
							 "turn it off when you are done looking.",
					staticProbeOffset);

				if (staticProbeOffset == 0.0f) {
					logger::info("  the offset is 0, so nothing will move. That is the NUMERIC "
								 "form of the probe - watch DS invocations in the profiler rather "
								 "than the world. Set 30 or so to answer it per mesh instead.");
				}
			}

			if (enableSnowRaise) {
				logger::info("  seam ramp {} texels = {} world units, on the snow side of the "
							 "boundary",
				snowSeamTexels,
				static_cast<float>(snowSeamTexels) * SnowCoverage::kTexelSize);

				logger::info("  trench reaches full depth at coverage {:.2f}{}",
					snowTrenchCoverage,
					snowTrenchCoverage <= 0.5f ?
						" (matched to the raise)" :
						" (pulled inside the seam)");
			}

		if (enableSnowRaise && snowRaiseHeight > 0.0f) {
			if (snowGroundFloor) {
				logger::info("  snow marks stop at the ground: the floor at a texel is the "
							 "blanket actually standing on it, plus {:.1f} units of the "
							 "ground itself",
					snowGroundBite);
			} else {
				logger::info("  SnowGroundFloor is OFF - churn and the impact depths may cut "
							 "below the land the blanket is lying on");
			}
		}

		if (enableSnowRaise && snowRaiseHeight > 0.0f && !SnowCoverage::Ready()) {
			logger::warn(
				"EnableSnowRaise is on but the coverage window does not exist yet. If "
				"nothing lifts, this line is why - and it should be gone by the next "
				"frame, because Update now creates it on demand.");
		}

		if (enableSnowRaise && !useClipmap) {
			logger::warn("EnableSnowRaise needs UseClipmap - the raise fades away from the "
						 "clipmap window's centre, which is the only place the domain shader "
						 "learns where the player is. The raise will not be emitted.");
		}

		if (enableSnowRaise && snowRaiseHeight <= 0.0f) {
			logger::info("  snow raise is ON but SnowRaiseHeight is 0 - nothing will lift, "
						 "though the coverage window still fills");
		}

		if (enableSnowRaise && snowRaiseFadeBand > snowRaiseDistance) {
			logger::warn("SnowRaiseFadeBand ({}) is wider than SnowRaiseDistance ({}), so the "
						 "raise is fading from the moment it starts and never reaches full "
						 "height anywhere.",
				snowRaiseFadeBand, snowRaiseDistance);
		}

		if (recomputeNormals) {
			const float cell = Clipmap::kCellSize;
			if (normalGradientEpsilon > cell * 2.0f || normalGradientEpsilon < cell * 0.5f) {
				logger::warn("NormalGradientEpsilon is {} but the field's cells are {} world "
							 "units. It is a step across the field and should be about one "
							 "cell; {} is the value that matches.",
					normalGradientEpsilon, cell, cell);
			}
		}

		g_lastWrite = WriteTimeOrZero();

	}

	bool PollForChanges(float a_deltaSeconds)
	{
		if (!enableLiveReload) {
			return false;
		}

		g_pollTimer -= std::max(a_deltaSeconds, 0.0f);
		if (g_pollTimer > 0.0f) {
			return false;
		}
		g_pollTimer = liveReloadInterval;

		const auto when = WriteTimeOrZero();
		if (when == std::filesystem::file_time_type{} || when == g_lastWrite) {
			return false;
		}

		std::error_code ec;
		if (std::filesystem::file_size(kPath, ec) == 0 || ec) {
			return false;
		}

		logger::info("Settings changed on disk - reloading");
		Load();
		return true;
	}
}
