// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#include "PCH.h"

#include "Settings.h"
#include "SurfaceProfiles.h"
#include "SurfaceTypes.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Surfaces
{
	namespace
	{

		constexpr auto kRoot = "Data/SKSE/Plugins/NMN_DeformableTerrain";
		constexpr auto kPatches = "Patches";
		constexpr auto kFile = "Surfaces.ini";

		struct Overrides
		{
			std::optional<float> depth;
			std::optional<float> radius;
			std::optional<float> shoulder;
			std::optional<float> decay;
			std::optional<float> rim;
			std::optional<float> print;
			std::optional<float> clearance;
		};

		struct Slot
		{
			std::optional<float> value;
			std::string          source;
		};

		struct StampDefaults
		{
			Slot snowRepose, snowSpan, snowNoise, snowLean, snowChurn;
			Slot repose, span, noise, lean, churn;
		};

		StampDefaults g_stampDefaults{};

		constexpr auto kStampSection = "stampdefaults";

		struct Profile
		{
			std::string name;
			std::string source;

			std::optional<Type> type;

			Overrides overrides{};

			std::vector<std::string> textures;

			std::vector<uint32_t> forms;
		};

		std::vector<Profile> g_profiles;

		std::unordered_map<std::string, int> g_byTexture;
		std::unordered_map<uint32_t, int>    g_byForm;

		std::mutex                           g_cacheLock;
		std::unordered_map<uint64_t, Ground> g_cache;
		constexpr size_t                     kMaxCacheEntries = 1024;

		std::string Lowered(std::string_view a_text)
		{
			std::string out(a_text);
			std::transform(out.begin(), out.end(), out.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return out;
		}

		std::string_view Trim(std::string_view a_text)
		{
			const auto first = a_text.find_first_not_of(" \t\r\n");
			if (first == std::string_view::npos) {
				return {};
			}
			const auto last = a_text.find_last_not_of(" \t\r\n");
			return a_text.substr(first, last - first + 1);
		}

		std::string TextureKey(std::string_view a_path)
		{
			const auto slash = a_path.find_last_of("\\/");
			auto       leaf = slash == std::string_view::npos ? a_path : a_path.substr(slash + 1);

			const auto dot = leaf.find_last_of('.');
			if (dot != std::string_view::npos && dot > 0) {
				leaf = leaf.substr(0, dot);
			}
			return Lowered(Trim(leaf));
		}

		Type TypeFromName(std::string_view a_name)
		{
			const auto lowered = Lowered(a_name);
			for (size_t i = 0; i < static_cast<size_t>(Type::kCount); ++i) {
				const auto type = static_cast<Type>(i);
				if (lowered == Name(type)) {
					return type;
				}
			}
			return Type::kCount;
		}

		void SplitInto(std::string_view a_value, std::vector<std::string>& a_out,
			bool a_asTextureKey)
		{
			size_t start = 0;
			while (start <= a_value.size()) {
				const auto comma = a_value.find(',', start);
				const auto end = comma == std::string_view::npos ? a_value.size() : comma;

				const auto item = Trim(a_value.substr(start, end - start));
				if (!item.empty()) {
					a_out.push_back(a_asTextureKey ? TextureKey(item) : Lowered(item));
				}

				if (comma == std::string_view::npos) {
					break;
				}
				start = comma + 1;
			}
		}

		bool ParseFloat(std::string_view a_value, float& a_out)
		{
			try {
				size_t     used = 0;
				const auto parsed = std::stof(std::string(a_value), &used);
				if (used == 0) {
					return false;
				}
				a_out = parsed;
				return true;
			} catch (...) {
				return false;
			}
		}

		bool ParseFormID(std::string_view a_value, uint32_t& a_out)
		{
			try {
				size_t     used = 0;
				const auto text = std::string(Trim(a_value));
				const auto parsed = std::stoul(text, &used, 0);
				if (used == 0) {
					return false;
				}
				a_out = static_cast<uint32_t>(parsed);
				return true;
			} catch (...) {
				return false;
			}
		}

		void ParseFile(const std::filesystem::path& a_path, std::string_view a_source)
		{
			std::ifstream file(a_path);
			if (!file) {
				return;
			}

			Profile     current{};
			bool        open = false;
			bool        inStamp = false;
			std::string line;

			const auto commit = [&]() {
				if (!open) {
					return;
				}
				if (current.textures.empty() && current.forms.empty()) {
					logger::warn("Surfaces: profile [{}] in {} matches nothing - it needs "
								 "Textures or LandTextures",
						current.name, a_source);
				} else {
					g_profiles.push_back(std::move(current));
				}
				current = Profile{};
				open = false;
			};

			while (std::getline(file, line)) {
				const auto trimmed = Trim(line);
				if (trimmed.empty() || trimmed.front() == ';' || trimmed.front() == '#') {
					continue;
				}

				if (trimmed.front() == '[') {
					commit();
					const auto close = trimmed.find(']');
					current.name = std::string(
						Trim(trimmed.substr(1, close == std::string_view::npos ?
													std::string_view::npos :
													close - 1)));
					current.source = std::string(a_source);

					inStamp = Lowered(current.name) == kStampSection;
					open = !inStamp;
					continue;
				}

				if (inStamp) {
					const auto eq = trimmed.find('=');
					if (eq == std::string_view::npos) {
						continue;
					}
					const auto skey = Lowered(Trim(trimmed.substr(0, eq)));
					auto       sbody = trimmed.substr(eq + 1);
					if (const auto c = sbody.find_first_of(";#");
						c != std::string_view::npos) {
						sbody = sbody.substr(0, c);
					}
					float sval = 0.0f;
					if (!ParseFloat(Trim(sbody), sval)) {
						logger::warn("Surfaces: [{}] in {} - '{}' is not a number for {}",
							current.name, a_source, Trim(sbody), skey);
						continue;
					}

					const auto take = [&](Slot& a_slot, float a_lo, float a_hi) {
						const float clamped = std::clamp(sval, a_lo, a_hi);
						if (clamped != sval) {
							logger::warn("Surfaces: {} in {} is outside {} to {}, using {}",
								skey, a_source, a_lo, a_hi, clamped);
						}
						if (a_slot.value && *a_slot.value != clamped) {
							logger::info("Surfaces: {} = {} in {} replaces {} from {}",
								skey, clamped, a_source, *a_slot.value, a_slot.source);
						}
						a_slot.value = clamped;
						a_slot.source = std::string(a_source);
					};

					if (skey == "snowstampreposerate") {
						take(g_stampDefaults.snowRepose, -1.0f, 1.0f);
					} else if (skey == "snowstamprimspan") {
						take(g_stampDefaults.snowSpan, -1.0f, 16.0f);
					} else if (skey == "snowstamprimnoise") {
						take(g_stampDefaults.snowNoise, -1.0f, 8.0f);
					} else if (skey == "snowstamprimlean") {
						take(g_stampDefaults.snowLean, -1.0f, 1.0f);
					} else if (skey == "snowstampchurn") {
						take(g_stampDefaults.snowChurn, -1.0f, 8.0f);
					} else if (skey == "stampreposerate") {
						take(g_stampDefaults.repose, 0.0f, 1.0f);
					} else if (skey == "stamprimspan") {
						take(g_stampDefaults.span, 0.0f, 16.0f);
					} else if (skey == "stamprimnoise") {
						take(g_stampDefaults.noise, 0.0f, 8.0f);
					} else if (skey == "stamprimlean") {
						take(g_stampDefaults.lean, 0.0f, 1.0f);
					} else if (skey == "stampchurn") {
						take(g_stampDefaults.churn, 0.0f, 8.0f);
					} else {
						logger::warn("Surfaces: [{}] in {} - '{}' is not a stamp default "
									 "this build knows",
							current.name, a_source, skey);
					}
					continue;
				}

				const auto equals = trimmed.find('=');
				if (equals == std::string_view::npos || !open) {
					continue;
				}

				const auto key = Lowered(Trim(trimmed.substr(0, equals)));

				auto       body = trimmed.substr(equals + 1);
				const auto comment = body.find_first_of(";#");
				if (comment != std::string_view::npos) {
					body = body.substr(0, comment);
				}
				const auto value = Trim(body);

				if (key == "textures" || key == "texture") {
					SplitInto(value, current.textures, true);
				} else if (key == "landtextures" || key == "landtexture") {
					std::vector<std::string> items;
					SplitInto(value, items, false);
					for (const auto& item : items) {
						uint32_t form = 0;
						if (ParseFormID(item, form)) {
							current.forms.push_back(form);
						} else {
							logger::warn("Surfaces: [{}] in {} - '{}' is not a form id",
								current.name, a_source, item);
						}
					}
				} else if (key == "type") {
					const auto type = TypeFromName(value);
					if (type == Type::kCount) {
						logger::warn("Surfaces: [{}] in {} - '{}' is not a surface type; "
									 "expected one of snow grass dirt mud sand ash gravel "
									 "stone. Leaving it out is fine and usually right.",
							current.name, a_source, value);
					} else {
						current.type = type;
					}
				} else {
					float parsed = 0.0f;
					if (!ParseFloat(value, parsed)) {
						logger::warn("Surfaces: [{}] in {} - '{}' is not a number for {}",
							current.name, a_source, value, key);
						continue;
					}

					if (key == "depth") {
						current.overrides.depth = parsed;
					} else if (key == "radius") {
						current.overrides.radius = parsed;
					} else if (key == "shoulder") {
						current.overrides.shoulder = std::clamp(parsed, 0.0f, 0.95f);
					} else if (key == "decay") {
						current.overrides.decay = parsed;
					} else if (key == "rim") {
						current.overrides.rim = parsed;
					} else if (key == "print") {
						current.overrides.print = parsed;
					} else if (key == "clearance") {
						current.overrides.clearance = parsed;
					} else {
						logger::warn("Surfaces: [{}] in {} - unknown key '{}'",
							current.name, a_source, key);
					}
				}
			}

			commit();
		}

		std::vector<std::filesystem::path> SortedFolders(const std::filesystem::path& a_root)
		{
			std::vector<std::filesystem::path> out;

			std::error_code ec;
			if (!std::filesystem::is_directory(a_root, ec)) {
				return out;
			}

			for (const auto& entry : std::filesystem::directory_iterator(a_root, ec)) {
				if (entry.is_directory(ec)) {
					out.push_back(entry.path());
				}
			}

			std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
				return Lowered(a.filename().string()) < Lowered(b.filename().string());
			});
			return out;
		}

		void BuildIndex(size_t a_firstPatch)
		{
			g_byTexture.clear();
			g_byForm.clear();

			for (size_t i = 0; i < g_profiles.size(); ++i) {
				const auto& profile = g_profiles[i];

				for (const auto& texture : profile.textures) {
					const auto existing = g_byTexture.find(texture);
					if (existing != g_byTexture.end()) {

						logger::warn("Surfaces: '{}' is claimed by both [{}] in {} and "
									 "[{}] in {} - the {} wins",
							texture, g_profiles[existing->second].name,
							g_profiles[existing->second].source, profile.name,
							profile.source,
							i >= a_firstPatch ? "patch" : "later folder");
					}
					g_byTexture[texture] = static_cast<int>(i);
				}

				for (const auto form : profile.forms) {
					g_byForm[form] = static_cast<int>(i);
				}
			}
		}
	}

	void LoadProfiles()
	{
		g_profiles.clear();
		g_stampDefaults = StampDefaults{};
		{
			std::lock_guard lock(g_cacheLock);
			g_cache.clear();
		}

		const std::filesystem::path root{ kRoot };

		size_t modules = 0;
		for (const auto& folder : SortedFolders(root)) {
			if (Lowered(folder.filename().string()) == Lowered(kPatches)) {
				continue;
			}
			const auto      file = folder / kFile;
			std::error_code ec;
			if (std::filesystem::is_regular_file(file, ec)) {
				ParseFile(file, folder.filename().string());
				++modules;
			}
		}

		const auto applyStampDefaults = [] {
			int n = 0;
			const auto set = [&n](const char* a_name, const Slot& a_from, float& a_to) {
				if (!a_from.value) {
					return;
				}
				a_to = *a_from.value;
				++n;
				logger::info("Surfaces:   {} = {} (from {})", a_name, a_to,
					a_from.source);
			};
			set("SnowStampReposeRate", g_stampDefaults.snowRepose, Settings::snowStampReposeRate);
			set("SnowStampRimSpan", g_stampDefaults.snowSpan, Settings::snowStampRimSpan);
			set("SnowStampRimNoise", g_stampDefaults.snowNoise, Settings::snowStampRimNoise);
			set("SnowStampRimLean", g_stampDefaults.snowLean, Settings::snowStampRimLean);
			set("SnowStampChurn", g_stampDefaults.snowChurn, Settings::snowStampChurn);
			set("StampReposeRate", g_stampDefaults.repose, Settings::stampReposeRate);
			set("StampRimSpan", g_stampDefaults.span, Settings::stampRimSpan);
			set("StampRimNoise", g_stampDefaults.noise, Settings::stampRimNoise);
			set("StampRimLean", g_stampDefaults.lean, Settings::stampRimLean);
			set("StampChurn", g_stampDefaults.churn, Settings::stampChurn);
			if (n > 0) {
				logger::info("Surfaces: {} stamp default(s) applied from [StampDefaults] "
							 "- these OVERRIDE the main INI; last file to set one wins",
					n);
			}
		};

		const auto firstPatch = g_profiles.size();

		size_t patches = 0;
		for (const auto& folder : SortedFolders(root / kPatches)) {
			const auto      file = folder / kFile;
			std::error_code ec;
			if (std::filesystem::is_regular_file(file, ec)) {
				ParseFile(file, "Patches/" + folder.filename().string());
				++patches;
			}
		}

		BuildIndex(firstPatch);

		applyStampDefaults();

		// The carried-weapon settings, printed for the same reason the surface
		// values above are printed: a number typed into the ini and a number
		// the plugin is actually using are two different things, and the only
		// way to tell them apart from a log is for the log to say.  Nothing
		// under this file writes these keys - they are not profile
		// overridable - so what appears here is what the main ini supplied, or
		// the built-in default where the key was absent.
		logger::info("Carried weapon: FollowPose={} StampAtContact={} GateAtContact={} "
					 "SpanFromContact={} SpanMaxGap={:.2f} SpanSteps={} ContactLineLength={:.2f} "
					 "LowOffset={} SpanLengthScale={:.2f} LineMinLength={:.2f} "
					 "StampRim={:.2f} StampMinDepth={:.2f} FromMesh={} | "
					 "MarkAlignToFoot={} FootAspect={:.2f} MaxAspect={:.2f} "
					 "RimJitter={:.2f} RimJitterBand={:.2f} - the gap is measured at "
					 "the hull, so an object whose axis sits half a thickness above the ground "
					 "is already touching; FromMesh swaps that hull for the object's own "
					 "vertices, LowOffset hands the walk the distance from the object's centre "
					 "line down to its own lowest point at that point's own place, "
					 "SpanLengthScale widens the measured stretch to cover the snow the object "
					 "disturbed without sinking into it, and StampRim gives the furrow the "
					 "raised snow a footprint has (StampMinDepth is the floor under the depth "
					 "derived from the mark's width, so 1.0 presses a weapon as deep as a foot). "
					 "MarkAlignToFoot gives the mark the footprint's own shape: its half width "
					 "is taken from its half length times FootAspect, floored by the object's "
					 "thickness, and its length is capped at MaxAspect times that width, which "
					 "is what stops a long thin mark from stacking into parallel teeth.  "
					 "FootAspect is the knob for the mark's width - the width is the mark's "
					 "own length times it, so a target half width is that over the longest "
					 "mark, 24.  RimJitter roughens the mark's rim edge, which is otherwise a "
					 "smooth ellipse that re-stamping lays down as evenly spaced ridges; it "
					 "is only applied to line marks, so a footprint keeps the smooth shape "
					 "the weapon was aligned to",
			Settings::shaftFollowPose, Settings::shaftStampAtContact,
			Settings::shaftGateAtContact, Settings::shaftSpanFromContact,
			Settings::shaftSpanMaxGap, Settings::shaftSpanSteps,
			Settings::shaftContactLineLength, Settings::shaftLowOffset,
			Settings::shaftSpanLengthScale, Settings::shaftLineMinLength,
			Settings::stampShaftRim, Settings::shaftStampMinDepth,
			Settings::stampFromMesh, Settings::shaftMarkAlignToFoot,
			Settings::shaftMarkFootAspect, Settings::shaftMarkMaxAspect,
			Settings::shaftMarkRimJitter, Settings::shaftMarkRimJitterBand);

		if (g_profiles.empty()) {
			logger::info("Surfaces: no per-texture profiles found under {} - running on "
						 "the main INI's response table and the game's material ids",
				kRoot);
			return;
		}

		logger::info("Surfaces: {} profile{} covering {} texture{} and {} land texture{}, "
					 "from {} module{} and {} patch{} under {}",
			g_profiles.size(), g_profiles.size() == 1 ? "" : "s",
			g_byTexture.size(), g_byTexture.size() == 1 ? "" : "s",
			g_byForm.size(), g_byForm.size() == 1 ? "" : "s",
			modules, modules == 1 ? "" : "s",
			patches, patches == 1 ? "" : "es",
			kRoot);

		for (size_t i = 0; i < g_profiles.size(); ++i) {
			const auto& profile = g_profiles[i];
			const auto& o = profile.overrides;

			const auto show = [](const std::optional<float>& a_value) {
				return a_value ? std::format("{:.2f}", *a_value) : std::string{ "-" };
			};

			std::string names;
			for (const auto& form : profile.forms) {
				names += std::format(" ltex:{:08X}", form);
			}
			for (const auto& texture : profile.textures) {
				names += " " + texture;
			}

			logger::info("  [{}] {:<20} type={:<7} depth={} radius={} shoulder={} "
						 "decay={} rim={} print={} clearance={} |{}{}",
				profile.source, profile.name,
				profile.type ? Name(*profile.type) : "(auto)",
				show(o.depth), show(o.radius), show(o.shoulder), show(o.decay),
				show(o.rim), show(o.print), show(o.clearance), names,
				i >= firstPatch ? "  (patch)" : "");
		}

		logger::info("  a dash means the profile left that one alone, so the main INI's "
					 "Response row for the surface decides it");
	}

	void ReleaseProfiles()
	{
		g_profiles.clear();
		g_byTexture.clear();
		g_byForm.clear();
		std::lock_guard lock(g_cacheLock);
		g_cache.clear();
	}

	size_t ProfileCount()
	{
		return g_profiles.size();
	}

	std::filesystem::file_time_type ProfilesWriteTime()
	{
		std::filesystem::file_time_type newest{};

		const auto note = [&newest](const std::filesystem::path& a_path) {
			std::error_code ec;
			const auto      when = std::filesystem::last_write_time(a_path, ec);
			if (!ec && when > newest) {
				newest = when;
			}
		};

		const std::filesystem::path root{ kRoot };
		note(root);

		for (const auto& folder : SortedFolders(root)) {
			note(folder);
			note(folder / kFile);
		}

		const auto patches = root / kPatches;
		note(patches);
		for (const auto& folder : SortedFolders(patches)) {
			note(folder);
			note(folder / kFile);
		}

		return newest;
	}

	const char* ProfileName(int a_index)
	{
		if (a_index < 0 || static_cast<size_t>(a_index) >= g_profiles.size()) {
			return nullptr;
		}
		return g_profiles[static_cast<size_t>(a_index)].name.c_str();
	}

	namespace
	{

		const RE::TESLandTexture* LandTextureAt(const RE::NiPoint3& a_position)
		{
			auto* tes = RE::TES::GetSingleton();
			if (!tes) {
				return nullptr;
			}

			float landZ = 0.0f;
			if (!tes->GetLandHeight(a_position, landZ)) {
				return nullptr;
			}

			RE::NiPoint3 sample = a_position;
			sample.z = landZ + 2.0f;

			auto* ltex = tes->GetLandTexture(sample);
			if (ltex && ltex->GetFormID() != 0) {
				return ltex;
			}

			auto* cell = tes->GetCell(a_position);
			if (!cell || cell->IsInteriorCell()) {
				return ltex;
			}

			auto* land = cell->GetRuntimeData().cellLand;
			if (!land || !land->loadedData) {
				return ltex;
			}

			auto* coords = cell->GetCoordinates();
			if (!coords) {
				return ltex;
			}

			constexpr float kCellSize = 4096.0f;
			constexpr float kHalfCell = kCellSize * 0.5f;

			const float localX =
				std::clamp(a_position.x - coords->worldX, 0.0f, kCellSize - 0.001f);
			const float localY =
				std::clamp(a_position.y - coords->worldY, 0.0f, kCellSize - 0.001f);

			const int quadIndex = (localX >= kHalfCell ? 1 : 0) + (localY >= kHalfCell ? 2 : 0);

			if (auto* defaultQuad = land->loadedData->defQuadTextures[quadIndex]) {
				return defaultQuad;
			}
			return ltex;
		}

		RE::BGSTextureSet* ActiveTextureSet(const RE::TESLandTexture* a_ltex)
		{
			auto* set = a_ltex ? a_ltex->textureSet : nullptr;
			if (!Settings::seasonalTextureSwap || !set || set->pad12C == 0) {
				return set;
			}

			auto* swapped = RE::TESForm::LookupByID<RE::BGSTextureSet>(set->pad12C);
			if (!swapped || swapped == set || swapped->IsDeleted()) {
				return set;
			}
			return swapped;
		}

		std::string_view DiffusePath(RE::BGSTextureSet* a_set)
		{
			if (!a_set) {
				return {};
			}
			const char* path = a_set->GetTexturePath(RE::BSTextureSet::Textures::kDiffuse);
			return path ? std::string_view(path) : std::string_view{};
		}
	}

	Ground GroundAt(const RE::NiPoint3& a_position)
	{
		Ground ground{};
		ground.response = ResponseFor(Type::kUnknown);

		if (!Settings::enableSurfaceClassification) {
			return ground;
		}

		auto* tes = RE::TES::GetSingleton();
		if (!tes) {
			return ground;
		}

		const auto* ltex = LandTextureAt(a_position);

		if (!ltex) {
			ground.type = FromMaterialID(tes->GetLandMaterialType(a_position));
			ground.response = ResponseFor(ground.type);
			return ground;
		}

		const uint32_t form = ltex->GetFormID();

		auto* const    active = ActiveTextureSet(ltex);
		const uint32_t setForm = active ? active->GetFormID() : 0;
		const bool     swapped = active && active != ltex->textureSet;

		const uint64_t cacheKey =
			(static_cast<uint64_t>(form) << 32) | static_cast<uint64_t>(setForm);
		const bool cacheable = form != 0;

		if (cacheable) {
			std::lock_guard lock(g_cacheLock);
			const auto      found = g_cache.find(cacheKey);
			if (found != g_cache.end()) {
				return found->second;
			}
		}

		if (const auto byForm = g_byForm.find(form); byForm != g_byForm.end()) {
			ground.profile = byForm->second;
		}

		const auto path = DiffusePath(active);
		const auto key = TextureKey(path);

		if (ground.profile < 0) {
			if (const auto byTexture = g_byTexture.find(key); byTexture != g_byTexture.end()) {
				ground.profile = byTexture->second;
			}
		}

		const Profile* profile =
			ground.profile >= 0 ? &g_profiles[static_cast<size_t>(ground.profile)] : nullptr;

		if (profile && profile->type) {
			ground.type = *profile->type;
		} else {

			const bool profiled = profile != nullptr;

			const auto fromMaterial = ltex->materialType ?
										  FromMaterialID(ltex->materialType->materialID) :
										  FromMaterialID(tes->GetLandMaterialType(a_position));

			const auto fromName = ClassifyTexturePath(path, profiled);

			ground.type = swapped ?
							  (fromName != Type::kUnknown ? fromName : fromMaterial) :
							  (fromMaterial != Type::kUnknown ? fromMaterial : fromName);

			if (profiled && ground.type == Type::kSnow) {
				ground.type = Type::kUnknown;
			}
		}

		ground.response = ResponseFor(ground.type);

		if (profile) {
			const auto& o = profile->overrides;
			if (o.depth) {
				ground.response.depthScale = *o.depth;
			}
			if (o.radius) {
				ground.response.radiusScale = *o.radius;
			}
			if (o.shoulder) {
				ground.response.shoulder = *o.shoulder;
			}
			if (o.decay) {
				ground.response.decayScale = *o.decay;
			}
			if (o.rim) {
				ground.response.rimScale = *o.rim;
			}
			if (o.print) {
				ground.response.print = *o.print;
			}
			if (o.clearance) {
				ground.response.clearanceScale = *o.clearance;
			}
		}

		if (Settings::logStampSurfaces) {
			static std::mutex                    reportLock;
			static std::unordered_map<uint32_t, bool> reported;
			std::lock_guard                      lock(reportLock);
			if (reported.size() < 64 && reported.try_emplace(form, true).second) {
				logger::info("Ground at ({:.0f}, {:.0f}): texture '{}'{} ltex {:08X} -> {}{}",
					a_position.x, a_position.y, key.empty() ? "<none>" : key,
					swapped ? std::format(" (season swap from txst {:08X})",
									ltex->textureSet ? ltex->textureSet->GetFormID() : 0) :
							  std::string{},
					form,
					profile ? std::format("profile [{}]", profile->name) :
							  std::format("no profile, treated as {}", Name(ground.type)),
					profile && !profile->type ?
						std::format(" (no Type given, so it is {} and NOT snow)",
							Name(ground.type)) :
						std::string{});
			}
		}

		if (cacheable) {
			std::lock_guard lock(g_cacheLock);

			if (g_cache.size() >= kMaxCacheEntries) {
				g_cache.clear();
			}
			g_cache[cacheKey] = ground;
		}

		return ground;
	}
}
