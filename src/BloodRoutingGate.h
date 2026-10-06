// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <utility>

namespace BloodRoutingGate
{
	template <class Registered>
	bool Candidate(bool enabled, bool clipmap, bool skinned, bool decalFlags, Registered&& registered)
	{
		return enabled && clipmap && !skinned &&
			(decalFlags || std::forward<Registered>(registered)());
	}
	constexpr bool Relevant(bool probe, bool nearLandscape, bool bloodCandidate)
	{
		return probe || nearLandscape || bloodCandidate;
	}
}
