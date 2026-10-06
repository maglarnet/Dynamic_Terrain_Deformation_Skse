// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

namespace Profiler
{

	enum class Scope
	{

		kClipmap,

		kLandscape,

		kLandscapeDepth,

		kOtherRouted,

		kCount
	};

	enum class Count
	{
		kLandscapeSeen,
		kLandscapeCulled,
		kLandscapeRouted,

		kDepthSeen,
		kDepthCulled,
		kDepthShadowSkipped,
		kDepthRouted,


		kStaticSeen,
		kStaticRouted,
		kShaderBuilds,
		kShelterLandAttempts,
		kCoverageLandQueries,
		kCoverageLandMisses,
		kCoveragePending,
		kShelterLandCached,
		kShelterLandMisses,
		kShelterRays,
		kShelterFadeVisited,
		kShelterFadeFull,
		kCoverageScaled,
		kCoverageCombineFull,
		kActorCacheHits,
		kActorCacheBuilds,
		kActorCollisionReads,
		kGatherPrepared,
		kGatherRejected,
		kGatherEmpty,
		kShelterCPUUpdates,
		kShelterUploads,
		kCount
	};

	bool Enabled();

	void Frame(float a_rawDeltaSeconds);

	void GpuBegin(Scope a_scope);
	void GpuEnd();

	void Tally(Count a_what, uint32_t a_howMany = 1);

	enum class CpuScope
	{

		kDrawBracket,

		kGatherStamps,
		kGatherHandoff,

		kSnowCoverage,

		kShelter,
		kShelterUpload,
		kObjectScan,
		kShaderPrepare,
		kMagicImpacts,
		kBloodUpdate,
		kBloodLookup,
		kSparkleUpdate,
		kCoverageFilter,
		kShelterFilter,
		kCoverageCombine,

		kCount
	};

	int64_t Ticks();
	void    AddCpuTicks(CpuScope a_scope, int64_t a_ticks);

	void Reset();

	void Release();
}
