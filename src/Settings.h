// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include "SurfaceTypes.h"

namespace Settings
{

	inline bool enableTessellation{ true };

	inline std::string tessellationWinding{ "cw" };

	inline bool logGeneratedShaders{ false };

	inline bool logTerrainBlendDraws{ false };

	inline bool terrainBlendingCompatibility{ true };
	inline bool terrainBlendingCullBackfaces{ true };

	inline bool enableDepthPass{ true };

	inline float tessellationMaxFactor{ 64.0f };

	inline float tessellationTargetSpacing{ 2.0f };

	inline float debugWorldZOffset{ 0.0f };

	inline float debugWaveAmplitude{ 0.0f };

	inline bool useClipmap{ true };

	inline uint32_t clipmapLevels{ 2 };

	inline float clipmapSeedSmoothing{ 1.0f };

	inline bool enableTessellationBounds{ true };

	inline float tessellationBoundDepth{ 0.25f };

	inline float stampRadiusScale{ 1.0f };

	inline float stampFootReach{ 48.0f };

	inline float stampGroundClearance{ 0.1f };

	inline float stampDepth{ 6.0f };

	inline float stampDecayPerSecond{ 0.99f };

	inline bool enableSnowRaise{ true };

	inline float snowRaiseHeight{ 35.0f };

	inline bool snowRaiseWeather{ false };

	inline float snowRaiseDistance{ 7936.0f };
	inline float snowRaiseFadeBand{ 4000.0f };

	inline int snowCoverageBudget{ 2048 };

	inline float snowTrenchDepth{ 0.5f };

	inline float snowTrenchCoverage{ 0.95f };

	inline bool snowGroundFloor{ true };

	inline float snowGroundBite{ 0.0f };

	inline int snowSeamTexels{ 1 };

	inline bool enableShelter{ true };

	inline int shelterBudget{ 48 };

	inline int shelterRefresh{ 16 };

	inline float shelterClearance{ 48.0f };

	inline float shelterHeight{ 1024.0f };

	inline int shelterSeamTexels{ 1 };

	inline bool shelterMeshCap{ true };

	inline float shelterFloorTolerance{ 8.0f };

	inline bool enableStaticProbe{ false };

	inline float staticProbeOffset{ 0.0f };

	inline bool logStaticProbe{ true };

	inline int staticProbeMinTriangles{ 2000 };

	inline bool logSnowCoverage{ false };

	inline bool enableWeather{ false };

	inline float weatherSoftenHours{ 1.0f };
	inline float weatherFirmHours{ 1.0f };

	inline float weatherSnowSoften{ 1.0f };
	inline float weatherRainSoften{ 8.0f };

	inline float weatherCloudyFirmScale{ 0.25f };

	inline float weatherSoftDepthScale{ 16.0f };
	inline float weatherFirmDepthScale{ 0.70f };

	inline float weatherSoftDecayScale{ 0.95f };
	inline float weatherFirmDecayScale{ 1.05f };

	inline float weatherFillPerSecond{ 0.06f };

	inline bool logWeather{ false };

	inline float tessellationRaiseSpacing{ 64.0f };

	inline float tessellationBlanketSpacing{ 8.0f };

	inline bool cullDistantDraws{ true };

	inline float cullMargin{ 256.0f };

	inline bool enableSurfaceMaterial{ true };

	inline bool enableSurfaceClassification{ true };

	inline bool logSurfaceClassification{ false };

	inline int debugBlendLayer{ -1 };

	inline float blendFullDepth{ 4.0f };

	inline float blendStrength{ 0.45f };

	inline float snowRevealStrength{ 0.4f };

	inline std::string surfaceSnow{ "snow01,snow02,snowrocks01,dirtsnowpath01,snowcobble01,frozenmarshice01" };
	inline std::string surfaceGrass{ "tundra01,tundra02,fieldgrass01,fieldgrass02,fielddirtgrass01,fallforestgrass01,fallforestleaves01,frozenmarshgrass01,grasssnow01,pineforest03,reachgrass01,reachmoss01,coastbeachgrass01,winterforestleaves" };
	inline std::string surfaceDirt{ "dirt01,dirt02,dirtpath01,fallforestdirt01,cavedirt,blackreachdirt,reachdirt01,minefloordirt01,volcanictundradirt,frozenmarshdirtslopes01,pineforest01,pineforest02,soulcairndirt,soulcairnpath01,glowingforestdirt01,winterforestdirt01,volcanictundramineralpool01" };
	inline std::string surfaceMud{ "rivermud01,coastoceanfloor01,frozenmarshlichen01" };
	inline std::string surfaceSand{ "coastbeach01,coastbeach02" };
	inline std::string surfaceAsh{ "volcanicash" };
	inline std::string surfaceGravel{ "fallforestrocks01,volcanictundragravel01,tundrarocks01,riverbottom01,reachmossyrocks01,volcanictundraminerals01" };
	inline std::string surfaceStone{ "rocks01,riverbededge01,volcanictundrarocks01,soulcairnrock01" };

	inline Surfaces::Response surfaceResponse[static_cast<size_t>(Surfaces::Type::kCount)]{

		  { 0.20f, 1.50f, 0.45f, 2.00f, 0.45f, 0.00f, 1.0f },

		  { 0.80f, 1.00f, 0.10f, 1.05f, 1.45f, 0.00f, 25.0f },

		  { 0.20f, 1.50f, 0.45f, 2.00f, 1.45f, 0.00f, 1.0f },

		  { 0.20f, 1.50f, 0.45f, 2.00f, 1.45f, 0.00f, 1.0f },
		  { 0.20f, 1.50f, 0.45f, 2.00f, 1.45f, 0.00f, 1.0f },

		  { 0.20f, 1.50f, 0.45f, 2.00f, 1.45f, 0.00f, 1.0f },
		  { 0.20f, 1.50f, 0.45f, 2.00f, 1.45f, 0.00f, 1.0f },
		  { 0.35f, 1.00f, 0.20f, 0.97f, 0.50f, 0.0f, 1.0f },
		  { 0.00f, 1.00f, 0.30f, 1.00f, 0.00f, 0.0f, 1.0f },
	};

	inline bool logStampSurfaces{ false };

	inline bool seasonalTextureSwap{ true };


	inline float stampReposeRate{ 0.01f };

	inline float stampRimNoise{ 0.0f };

	inline float stampRimLean{ 1.0f };

	inline float stampChurn{ 0.45f };

	inline float snowStampRimSpan{ 0.7f };
	inline float snowStampReposeRate{ 0.10f };
	inline float snowStampRimNoise{ 0.8f };
	inline float snowStampRimLean{ 1.0f };
	inline float snowStampChurn{ 0.80f };
	inline bool asyncShaderCompilation{ true };

	inline float stampSlopeLimit{ 50.0f };

	inline bool stampFootShape{ true };

	inline float stampFootLength{ 1.3f };

	inline float stampFootAspect{ 0.50f };

	inline float stampFootSeparation{ 0.0f };

	inline bool logStampFeet{ false };

	inline std::string stampShapeKeyword{ "ActorTypeNPC" };

	inline float stampRimHeight{ 0.2f };

	inline float stampRimIndependence{ 1.0f };

	inline float stampRimSpan{ 1.0f };

	inline bool enableObjectStamps{ true };

	inline float objectStampRadiusScale{ 1.0f };

	inline float objectStampDepthScale{ 1.0f };

	inline float objectFullSizeRadius{ 1.0f };

	inline float objectContactTolerance{ 48.0f };

	inline float objectSinkLimit{ 40.0f };

	inline float objectStampInterval{ 0.20f };

	inline bool logObjectStamps{ false };

	inline bool enableHeatSources{ true };

	inline std::string heatKeywords{
		"fire,campfire,brazier,firepit,candle,torch,forge,smelt,hearth"
	};

	inline float heatRadius{ 2.5f };
	inline float heatRadiusScale{ 1.0f };
	inline float heatMaxRadius{ 256.0f };

	inline bool enableMagicImpacts{ true };
	inline bool enableShoutImpacts{ true };
	inline bool enableExplosionImpacts{ true };
	inline float magicImpactRadius{ 36.0f };
	inline float magicImpactDepth{ 32.0f };
	inline float shoutImpactRange{ 1200.0f };
	inline float shoutImpactWidth{ 500.0f };
	inline float shoutImpactDepth{ 24.0f };
	inline float explosionImpactRadiusScale{ 0.5f };
	inline float explosionImpactMaxRadius{ 256.0f };
	inline float explosionImpactDepth{ 18.0f };
	inline float magicImpactRimScale{ 0.1f };
	inline float magicImpactSnowMelt{ 0.3f };
	inline bool logMagicImpacts{ false };

	inline float heatMeltPerSecond{ 0.85f };

	inline float heatShoulder{ 0.20f };

	inline bool  heatFromTorches{ true };
	inline float heatTorchRadius{ 40.0f };

	inline float heatTorchScale{ 0.45f };

	inline bool heatMeltsSnow{ true };

	inline float heatMeltDepthScale{ 0.80f };

	inline bool heatFromExplosions{ true };

	inline bool logHeatSources{ false };


	inline bool enableLiveReload{ true };

	inline float liveReloadInterval{ 0.5f };

	inline bool enableBloodDecals{ true };

	inline bool bloodDecalsIgnoreShaderIdentity{ true };
	inline std::string bloodDecalTexturePrefixes{ "blood,decalsblood,bigspatter" };




	inline bool enableLogging{ false };

	inline bool logDraws{ false };

	inline bool logActorDraws{ false };

	inline bool routeOnlyPlayerCamera{ true };

	inline bool recomputeNormals{ true };

	inline float normalGradientEpsilon{ 0.75f };

	inline float debugWaveLength{ 256.0f };

	inline int debugFieldColour{ 0 };

	inline bool enableProfiler{ false };

	inline float profileInterval{ 5.0f };

	inline bool profileDrawScopes{ false };

	inline bool snowSparkle{ true };

	inline float snowSparkleRate{ 1000.0f };

	inline float snowSparkleFullSpeed{ 350.0f };
	inline float snowSparkleMinSpeed{ 40.0f };

	inline float snowSparkleSize{ 1.0f };

	inline float snowSparkleLife{ 1.0f };

	inline float snowSparkleThrow{ 500.0f };
	inline float snowSparkleRise{ 80.0f };

	inline float snowSparkleInherit{ 1.0f };

	inline float snowSparkleGravity{ 300.0f };
	inline float snowSparkleDrag{ 4.2f };
	inline float snowSparkleSwirl{ 100.0f };

	inline float snowSparkleBrightness{ 0.5f };
	inline float snowSparkleOpacity{ 0.5f };

	inline float snowSparkleShape{ 1.0f };

	void Load();

	bool PollForChanges(float a_deltaSeconds);

	void ApplyLogLevel();
}
