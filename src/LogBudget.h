// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <cstdint>

// How often a diagnostic may write, and what a window of them adds up to.
//
// Both live in a header with no engine types in them, for the reason
// ContactPoint.h gives: a rule that cannot be called from the offline test is
// a rule that cannot be tested, and an untested rule drifts.
//
// The fault this solves was measured rather than imagined.  The carried-shaft
// lines each had a one-shot budget - 24 marks, 12 refusals, 32 mesh lines -
// and a single stretch of play can spend all of them inside its first five
// seconds, after which no shaft line appears at all.  The five seconds that
// are covered are the opening of the session, not the part where a carried
// weapon fails to leave a furrow, so the log cannot answer the one question it
// exists for.  A rate limit replaces the budget: the whole session is covered
// and no busy second can exhaust it.
//
// The same run shows why a rate limit is not enough on its own.  Twelve
// refusals were written inside four tenths of a second; they say a refusal
// happened, they do not say how the weapon was carried.  Two opposite
// readings fit them - the gate is stricter than it should be, or the weapon
// was simply carried clear of the snow - and they call for opposite fixes.
// The window separates them with one number: the smallest clearance seen
// among the refusals, which is how much closer the weapon would have had to
// come.

namespace LogBudget
{
	// Rate limit for a single line.
	//
	// a_last is the timestamp of the previous write, with 0 meaning "never".
	// The first call has to pass, or a session would open with a silent gap in
	// which the reader cannot tell "not written yet" from "not happening".
	inline bool Allow(int64_t& a_last, int64_t a_now, int64_t a_gapMs)
	{
		if (a_gapMs > 0 && a_last != 0 && a_now - a_last < a_gapMs) {
			return false;
		}
		a_last = a_now;
		return true;
	}

	// A time window of carried-shaft decisions, as one weapon's walk sees them.
	//
	// The three counts are nested on purpose.  `seen` is every object the
	// shaft test accepted, which is what "a weapon was carried" means;
	// `walked` is the subset the axis walk actually judged, and the gap
	// between the two is made of the frames that had no land to read or had
	// the follow-pose route switched off.  `marked` and `refused` split
	// `walked`, so a window where a weapon was carried and produced nothing
	// still says which stage it stopped at instead of being empty.
	struct ShaftWindow
	{
		uint32_t seen = 0;             // objects the shaft test accepted
		uint32_t walked = 0;           // of those, the ones the walk judged
		uint32_t marked = 0;           // judged and turned into a stamp
		uint32_t markedLine = 0;       // of those, drawn as a line rather than a disc
		uint32_t refused = 0;          // judged and carried clear of the snow
		uint32_t clearanceSamples = 0;
		float    clearanceLeast = 0.0f;  // the closest a refusal came to the limit
		float    clearanceMost = 0.0f;   // the furthest a refusal sat above it

		// The smallest limit any refusal in this window was measured against,
		// and the largest.
		//
		// A refusal is only meaningful next to the bound it failed, and that
		// bound is no longer a constant: it is derived from the weapon's own
		// thickness (ContactPoint::ContactReach), so two weapons of different
		// thicknesses in the same window failed two different limits.  Storing
		// the raw clearance alone would leave the window unable to say which
		// bound to compare it to, and comparing a clearance against the wrong
		// bound is how a gate reads as binding when it is not - or as
		// innocent when it is.
		//
		// Both ends are kept because the verdict is a comparison against the
		// tightest bound in the window: if even the strictest limit a refusal
		// failed is far below the clearance, then no reach this window could
		// have applied would have passed it.
		float    limitLeast = 0.0f;
		float    limitMost = 0.0f;
		uint32_t limitSamples = 0;

		void Notice() { ++seen; }

		void Evaluate() { ++walked; }

		void Mark(bool a_line)
		{
			++marked;
			if (a_line) { ++markedLine; }
		}

		// a_clearance is the measured distance above the snow and a_limit is
		// the bound it was compared against, both as the gate saw them.
		void Refuse(float a_clearance, float a_limit)
		{
			if (a_limit == a_limit) {  // NaN must not poison the bounds either
				if (limitSamples == 0 || a_limit < limitLeast) {
					limitLeast = a_limit;
				}
				if (limitSamples == 0 || a_limit > limitMost) {
					limitMost = a_limit;
				}
				++limitSamples;
			}

			if (!(a_clearance == a_clearance)) {  // NaN must not poison the extremes
				++refused;
				return;
			}
			if (clearanceSamples == 0 || a_clearance < clearanceLeast) {
				clearanceLeast = a_clearance;
			}
			if (clearanceSamples == 0 || a_clearance > clearanceMost) {
				clearanceMost = a_clearance;
			}
			++clearanceSamples;
			++refused;
		}

		// The widest bound any refusal in this window was measured against.
		// Zero when nothing was refused, which no caller compares against.
		float WidestLimit() const { return limitMost; }

		// The tightest bound any refusal in this window was measured against.
		float TightestLimit() const { return limitLeast; }

		bool Empty() const { return seen == 0 && walked == 0 && marked == 0 && refused == 0; }

		void Reset() { *this = ShaftWindow{}; }

		// Whether the gate is what stands between the carried weapon and a mark.
		//
		// Three times the limit is the line between the two readings.  It is
		// not a physical constant, but it is not a guess either, and what
		// matters is that it is falsifiable: easing the limit to the clearance
		// seen would have to more than triple it to change anything, which is
		// a change of behaviour rather than an adjustment - so a refusal that
		// close is the gate's doing and a refusal further out is the carry's.
		// With no refusal sampled there is nothing to blame the gate for.
		bool GateIsBinding(float a_limit) const
		{
			return clearanceSamples > 0 && clearanceLeast <= 3.0f * a_limit;
		}
	};
}
