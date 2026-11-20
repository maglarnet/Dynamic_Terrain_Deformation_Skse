// SPDX-License-Identifier: GPL-3.0-only
#include "AuthoredSnow.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "AuthoredSnowMath.h"
#include "Globals.h"
#include "PCH.h"
#include "Profiler.h"

namespace AuthoredSnow {
    namespace {
        using Clock = std::chrono::steady_clock;
        constexpr size_t kMaxMeshes = 256, kMaxTriangles = 400000;
        struct Mesh {
            uint64_t id{};
            const void* renderer{};
            RE::NiTransform transform;
            RE::NiBound bound;
            uint32_t vertices{}, triangles{};
            RE::ObjectRefHandle owner;
            Clock::time_point seen{};
            std::vector<AuthoredSnowMath::Triangle> faces;
            AuthoredSnowMath::FaceGrid grid;
            float minX{}, minY{}, minZ{}, maxX{}, maxY{}, maxZ{};
            Clock::time_point pressedUntil{};
            Clock::time_point capturedAt{};
            Clock::time_point lastMoveAt{};
            uint32_t smallMoves{};
            bool moving{};
        };
        struct Timed {
            int64_t started = Profiler::Ticks();
            ~Timed() { Profiler::AddCpuTicks(Profiler::CpuScope::kSnowMeshes, Profiler::Ticks() - started); }
        };
        std::mutex g_lock;
        std::unordered_map<const void*, Mesh> g_meshes;
        uint64_t g_nextID = 1;
        float g_scanCountdown{};
        uint32_t g_readbacks{};
        RE::NiPoint3 g_anchor{};
        std::unordered_map<const void*, Clock::time_point> g_retry;
        std::atomic<uint32_t> g_textureGeneration{1};
        uint32_t g_skipsLogged{};
        std::unordered_set<std::string> g_loggedTextures;
        Clock::time_point g_nextReport{};
        uint32_t g_reportFrames{}, g_reportRouted{}, g_reportCaptured{}, g_reportRefused{}, g_reportLeft{};
        uint32_t g_movingLogged{}, g_againLogged{}, g_settledLogged{}, g_slowLogged{};
        constexpr size_t kFrameTriangleBudget = 12000;
        size_t g_frameTriangles{};

        float Moved(const RE::NiTransform& a, const RE::NiTransform& b, float& turned) {
            turned = 0.0f;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) turned = std::max(turned, std::abs(a.rotate.entry[i][j] - b.rotate.entry[i][j]));
            return a.translate.GetDistance(b.translate);
        }

        bool SamePlace(const RE::NiTransform& a, const RE::NiTransform& b) {
            if (std::abs(a.scale - b.scale) > 1e-4f || std::abs(a.translate.x - b.translate.x) > 0.05f ||
                std::abs(a.translate.y - b.translate.y) > 0.05f || std::abs(a.translate.z - b.translate.z) > 0.05f)
                return false;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    if (std::abs(a.rotate.entry[i][j] - b.rotate.entry[i][j]) > 1e-4f) return false;
            return true;
        }

        bool Outside(const Mesh& m, float cx, float cy, float half) {
            return m.maxX < cx - half || m.minX > cx + half || m.maxY < cy - half || m.minY > cy + half;
        }

        void Skip(RE::BSGeometry* geometry, const char* reason) {
            ++g_reportRefused;
            if (g_skipsLogged >= 12) return;
            ++g_skipsLogged;
            logger::info("Snow mesh '{}' not captured: {}", geometry ? geometry->name.c_str() : "", reason);
        }

        void Report(Clock::time_point now) {
            ++g_reportFrames;
            if (now < g_nextReport) return;
            const bool first = g_nextReport == Clock::time_point{};
            g_nextReport = now + std::chrono::seconds(5);
            if (first || (g_meshes.empty() && !g_reportRouted && !g_reportCaptured && !g_reportRefused)) {
                g_reportFrames = g_reportRouted = g_reportCaptured = g_reportRefused = g_reportLeft = 0;
                return;
            }
            size_t triangles = 0, pressed = 0, moving = 0;
            for (const auto& [key, m] : g_meshes) {
                triangles += m.faces.size();
                pressed += now < m.pressedUntil ? 1 : 0;
                moving += m.moving ? 1 : 0;
            }
            logger::info("Snow meshes: {} held ({} triangles), {} with a dent, {} left out as moving | last 5 s: "
                         "{} captured, {} refused, {} dents dropped as their mesh left the field, "
                         "{:.1f} routed draws a frame (colour and depth)",
                         g_meshes.size(), triangles, pressed, moving, g_reportCaptured, g_reportRefused, g_reportLeft,
                         g_reportFrames ? float(g_reportRouted) / float(g_reportFrames) : 0.0f);
            g_reportFrames = g_reportRouted = g_reportCaptured = g_reportRefused = g_reportLeft = 0;
        }

        const char* Diffuse(RE::BSGeometry* geometry) {
            auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
            auto* material = property ? property->GetBaseMaterial() : nullptr;
            if (!material || material->GetType() != RE::BSShaderMaterial::Type::kLighting) return nullptr;
            using Feature = RE::BSShaderMaterial::Feature;
            switch (material->GetFeature()) {
            case Feature::kDefault:
            case Feature::kEnvironmentMap:
            case Feature::kGlowMap:
            case Feature::kParallax:
            case Feature::kParallaxOcc:
            case Feature::kMultilayerParallax:
                break;
            default:
                return nullptr;
            }
            auto* set = static_cast<RE::BSLightingShaderMaterialBase*>(material)->textureSet.get();
            return set ? set->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
        }

        bool SnowTexture(RE::BSGeometry* geometry) {
            if (Settings::meshSnowTextures.empty()) return false;
            auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
            auto* material = property ? property->GetBaseMaterial() : nullptr;
            if (!material || material->GetType() != RE::BSShaderMaterial::Type::kLighting) return false;
            const void* set = static_cast<RE::BSLightingShaderMaterialBase*>(material)->textureSet.get();
            if (!set) return false;
            const char* path = Diffuse(geometry);
            if (!path) return false;
            struct Cache {
                uint32_t generation{};
                std::unordered_map<const void*, std::pair<const char*, bool>> sets;
            };
            thread_local Cache cache;
            const auto generation = g_textureGeneration.load(std::memory_order_relaxed);
            if (cache.generation != generation || cache.sets.size() > 8192) {
                cache.sets.clear();
                cache.generation = generation;
            }
            auto [found, fresh] = cache.sets.try_emplace(set, path, false);
            if (fresh || found->second.first != path)
                found->second = {path, AuthoredSnowMath::MatchesTexture(path, Settings::meshSnowTextures)};
            return found->second.second;
        }

        bool ReadBuffer(ID3D11Buffer* buffer, size_t bytes, std::vector<uint8_t>& out) {
            if (!buffer || !globals::Ready()) return false;
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            if (bytes > desc.ByteWidth || desc.ByteWidth > 16 * 1024 * 1024) return false;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags = 0;
            desc.StructureByteStride = 0;
            ID3D11Buffer* staging{};
            if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, &staging))) return false;
            auto* ctx = globals::d3d::context;
            ctx->CopyResource(staging, buffer);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const bool ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
            if (ok) {
                out.resize(bytes);
                std::memcpy(out.data(), mapped.pData, bytes);
                ctx->Unmap(staging, 0);
            }
            staging->Release();
            return ok;
        }

        Mesh* Capture(RE::BSGeometry* geometry, bool readback, RE::ObjectRefHandle owner = {}) {
            if (!geometry || !Matches(geometry)) return nullptr;
            auto* tri = geometry->AsTriShape();
            auto& runtime = geometry->GetGeometryRuntimeData();
            auto* data = runtime.rendererData;
            if (!std::isfinite(geometry->worldBound.radius) || geometry->worldBound.radius <= 0 ||
                !std::isfinite(geometry->world.scale) || geometry->world.scale <= 0)
                return nullptr;
            if (!tri || !data || runtime.skinInstance ||
                geometry->GetType().any(RE::BSGeometry::Type::kDynamicTriShape)) {
                Skip(geometry, !tri ? "not a BSTriShape" : !data ? "no renderer data" :
                               runtime.skinInstance ? "skinned" : "dynamic");
                return nullptr;
            }
            const auto& counts = tri->GetTrishapeRuntimeData();
            const auto now = Clock::now();
            Clock::time_point pressedUntil{}, lastMoveAt{};
            uint32_t smallMoves{};
            auto existing = g_meshes.find(geometry);
            if (existing != g_meshes.end()) {
                auto& m = existing->second;
                const bool sameOwner = !owner || !m.owner || m.owner == owner;
                const bool sameShape = sameOwner && m.renderer == data && m.vertices == counts.vertexCount &&
                                       m.triangles == counts.triangleCount;
                bool settled = false;
                if (sameShape && m.moving) {
                    m.seen = now;
                    if (owner) m.owner = owner;
                    if (!SamePlace(m.transform, geometry->world)) {
                        m.transform = geometry->world;
                        m.lastMoveAt = now;
                        return nullptr;
                    }
                    if (now - m.lastMoveAt < std::chrono::seconds(2)) return nullptr;
                    if (g_frameTriangles > 0 && g_frameTriangles + counts.triangleCount > kFrameTriangleBudget)
                        return nullptr;
                    settled = true;
                    if (g_settledLogged < 12) {
                        ++g_settledLogged;
                        logger::info("Snow mesh '{}' under '{}' has stopped moving: taken back", geometry->name.c_str(),
                                     geometry->parent ? geometry->parent->name.c_str() : "");
                    }
                    g_meshes.erase(existing);
                } else if (sameShape && SamePlace(m.transform, geometry->world)) {
                    m.seen = now;
                    if (owner) m.owner = owner;
                    return &m;
                }
                if (!settled) {
                    float turned{};
                    const float moved = sameShape ? Moved(m.transform, geometry->world, turned) : 0.0f;
                    const bool nudge = sameShape && moved < 32.0f && turned < 0.05f;
                    lastMoveAt = m.lastMoveAt;
                    smallMoves = m.smallMoves;
                    if (nudge) {
                        smallMoves = now - m.lastMoveAt < std::chrono::seconds(2) ? smallMoves + 1 : 1;
                        lastMoveAt = now;
                    } else {
                        smallMoves = 0;
                    }
                    if (smallMoves >= 3) {
                        m.moving = true;
                        m.seen = now;
                        m.lastMoveAt = now;
                        m.transform = geometry->world;
                        m.faces = {};
                        m.grid = {};
                        m.pressedUntil = {};
                        if (g_movingLogged < 12) {
                            ++g_movingLogged;
                            logger::info("Snow mesh '{}' under '{}' moves: left out ({} moves within 2 s, last {:.2f} units)",
                                         geometry->name.c_str(), geometry->parent ? geometry->parent->name.c_str() : "",
                                         smallMoves, moved);
                        }
                        return nullptr;
                    }
                    if (g_frameTriangles > 0 && g_frameTriangles + counts.triangleCount > kFrameTriangleBudget) return nullptr;
                    if (g_againLogged < 8) {
                        ++g_againLogged;
                        logger::info("Snow mesh '{}' captured again: {}", geometry->name.c_str(),
                                     !sameOwner ? "another reference at the same address" :
                                     !sameShape ? "other shape data at the same address" :
                                     nudge ? std::format("moved {:.2f} units", moved) :
                                             std::format("replaced, {:.0f} units away", moved));
                    }
                    if (sameShape) pressedUntil = m.pressedUntil;
                    g_meshes.erase(existing);
                }
            }
            if (counts.vertexCount < 3 || counts.triangleCount == 0 || counts.triangleCount > 32768) {
                Skip(geometry, "triangle count out of range");
                return nullptr;
            }
            if (g_frameTriangles > 0 && g_frameTriangles + counts.triangleCount > kFrameTriangleBudget) return nullptr;
            if (g_meshes.size() >= kMaxMeshes) {
                Skip(geometry, "256 meshes already held");
                return nullptr;
            }
            size_t total = 0;
            for (const auto& [key, m] : g_meshes) total += m.faces.size();
            if (total + counts.triangleCount > kMaxTriangles) {
                Skip(geometry, "400,000 triangles already held");
                return nullptr;
            }
            const uint32_t stride = data->vertexDesc.GetSize();
            if (stride < 8 || stride > 128 || !data->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) {
                Skip(geometry, "unreadable vertex layout");
                return nullptr;
            }
            const auto* vertices = data->rawVertexData;
            const auto* indices = data->rawIndexData;
            std::vector<uint8_t> vb, ib;
            if (!vertices || !indices) {
                if (!readback || g_readbacks >= 2) return nullptr;
                ++g_readbacks;
                if (!vertices) {
                    if (!ReadBuffer(reinterpret_cast<ID3D11Buffer*>(data->vertexBuffer),
                                    size_t(stride) * counts.vertexCount, vb))
                        return nullptr;
                    vertices = vb.data();
                }
                if (!indices) {
                    if (!ReadBuffer(reinterpret_cast<ID3D11Buffer*>(data->indexBuffer),
                                    size_t(counts.triangleCount) * 6, ib))
                        return nullptr;
                    indices = reinterpret_cast<const uint16_t*>(ib.data());
                }
            }
            const auto started = Clock::now();
            std::vector<RE::NiPoint3> half, full;
            const float diameter = std::max(2 * geometry->GetModelData().modelBound.radius, 1.0f);
            auto decode = [&](bool fp, std::vector<RE::NiPoint3>& out) {
                if (stride < (fp ? 12u : 8u)) return std::numeric_limits<float>::infinity();
                out.resize(counts.vertexCount);
                RE::NiPoint3 lo{}, hi{};
                for (uint32_t i = 0; i < counts.vertexCount; ++i) {
                    auto& p = out[i];
                    const auto* src = vertices + size_t(i) * stride;
                    if (fp)
                        std::memcpy(&p, src, 12);
                    else {
                        uint16_t v[3];
                        std::memcpy(v, src, 6);
                        p = {DirectX::PackedVector::XMConvertHalfToFloat(v[0]),
                             DirectX::PackedVector::XMConvertHalfToFloat(v[1]),
                             DirectX::PackedVector::XMConvertHalfToFloat(v[2])};
                    }
                    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                        std::max({std::abs(p.x), std::abs(p.y), std::abs(p.z)}) > 1e6f)
                        return std::numeric_limits<float>::infinity();
                    if (i == 0)
                        lo = hi = p;
                    else {
                        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                    }
                }
                const float extent = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
                return extent > 1e-4f ? std::abs(std::log(extent / diameter)) : std::numeric_limits<float>::infinity();
            };
            const float halfScore = decode(false, half), fullScore = decode(true, full);
            if (!std::isfinite(halfScore) && !std::isfinite(fullScore)) {
                Skip(geometry, "positions did not decode");
                return nullptr;
            }
            const auto& positions = fullScore <= halfScore ? full : half;
            Mesh m{};
            m.id = g_nextID++;
            m.renderer = data;
            m.transform = geometry->world;
            m.bound = geometry->worldBound;
            m.vertices = counts.vertexCount;
            m.triangles = counts.triangleCount;
            m.owner = owner;
            m.seen = now;
            m.pressedUntil = pressedUntil;
            m.capturedAt = now;
            m.lastMoveAt = lastMoveAt;
            m.smallMoves = smallMoves;
            m.faces.reserve(counts.triangleCount);
            m.minX = m.minY = m.minZ = std::numeric_limits<float>::infinity();
            m.maxX = m.maxY = m.maxZ = -std::numeric_limits<float>::infinity();
            auto world = [&](uint16_t index) {
                const auto p = geometry->world * positions[index];
                return AuthoredSnowMath::Point{p.x, p.y, p.z};
            };
            for (uint32_t i = 0; i < counts.triangleCount; ++i) {
                const auto a = indices[i * 3], b = indices[i * 3 + 1], c = indices[i * 3 + 2];
                if (a >= counts.vertexCount || b >= counts.vertexCount || c >= counts.vertexCount) {
                    Skip(geometry, "index out of range");
                    return nullptr;
                }
                const auto face = AuthoredSnowMath::Triangle{world(a), world(b), world(c)};
                for (const auto& p : {face.a, face.b, face.c}) {
                    m.minX = std::min(m.minX, p.x);
                    m.maxX = std::max(m.maxX, p.x);
                    m.minY = std::min(m.minY, p.y);
                    m.maxY = std::max(m.maxY, p.y);
                    m.minZ = std::min(m.minZ, p.z);
                    m.maxZ = std::max(m.maxZ, p.z);
                }
                m.faces.push_back(face);
            }
            m.grid.Build(m.faces, m.minX, m.minY, m.maxX, m.maxY);
            ++g_reportCaptured;
            g_frameTriangles += m.faces.size();
            const char* diffuse = Diffuse(geometry);
            std::string texture = diffuse ? diffuse : "(named NMN_aSnow)";
            std::transform(texture.begin(), texture.end(), texture.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (g_loggedTextures.size() < 64 && g_loggedTextures.insert(texture).second) {
                logger::info("Snow mesh '{}' captured, first with {}: {} triangles, {:.0f} x {:.0f} units, z {:.0f} to {:.0f}, {}",
                             geometry->name.c_str(), texture, m.faces.size(), m.maxX - m.minX, m.maxY - m.minY, m.minZ,
                             m.maxZ, vb.empty() && ib.empty() ? "CPU" : "GPU readback");
            }
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
            if (ms > 2.0 && g_slowLogged < 12) {
                ++g_slowLogged;
                logger::info("Snow mesh '{}' took {:.1f} ms to capture ({} triangles, {} vertices, {})",
                             geometry->name.c_str(), ms, m.faces.size(), counts.vertexCount,
                             vb.empty() && ib.empty() ? "CPU" : "GPU readback");
            }
            return &g_meshes.emplace(geometry, std::move(m)).first->second;
        }
    }

    bool Matches(RE::BSGeometry* geometry) {
        if (!Settings::enableAuthoredMeshDeformation || !geometry) return false;
        auto& runtime = geometry->GetGeometryRuntimeData();
        if (runtime.skinInstance) return false;
        return AuthoredSnowMath::Matches(std::string_view(geometry->name)) || SnowTexture(geometry);
    }

    void BeginGather(float dt) {
        Timed timed;
        std::lock_guard guard(g_lock);
        if (!Settings::enableAuthoredMeshDeformation || !Settings::enableTessellation || !Settings::useClipmap) return;
        if (globals::game::player) g_anchor = globals::game::player->GetPosition();
        const auto now = Clock::now();
        Report(now);
        if (float cx{}, cy{}, half{}; Clipmap::GetWindow(cx, cy, half))
            for (auto& [key, m] : g_meshes)
                if (now < m.pressedUntil && Outside(m, cx, cy, half)) {
                    m.pressedUntil = {};
                    ++g_reportLeft;
                }
        g_scanCountdown -= std::clamp(dt, 0.0f, 0.25f);
        if (g_scanCountdown > 0) return;
        g_scanCountdown = 0.5f;
        std::erase_if(g_meshes, [&](const auto& item) {
            return now - item.second.seen > std::chrono::seconds(30) || (item.second.owner && !item.second.owner.get());
        });
        auto* tes = RE::TES::GetSingleton();
        auto* player = globals::game::player;
        if (!tes || !player) return;
        const auto started = Clock::now();
        size_t visited = 0, captured = 0, references = 0;
        auto visit = [&](auto&& self, RE::NiAVObject* obj, RE::ObjectRefHandle owner, uint32_t depth) -> void {
            if (!obj || depth > 32 || ++visited > 32768) return;
            if (auto* geometry = obj->AsGeometry(); geometry && Matches(geometry)) {
                if (captured < 16 && !g_meshes.contains(geometry)) {
                    auto* data = geometry->GetGeometryRuntimeData().rendererData;
                    if (data && data->rawVertexData && data->rawIndexData) {
                        ++captured;
                        Capture(geometry, false, owner);
                    }
                } else if (g_meshes.contains(geometry))
                    Capture(geometry, false, owner);
            }
            if (auto* node = obj->AsNode())
                for (const auto& child : node->GetChildren()) self(self, child.get(), owner, depth + 1);
        };
        tes->ForEachReferenceInRange(
            player, Clipmap::WorldSizeFor(Clipmap::LevelCount() - 1) * 0.5f, [&](RE::TESObjectREFR* ref) {
                if (visited >= 32768) return RE::BSContainer::ForEachResult::kStop;
                if (ref && !ref->IsDisabled() && !ref->IsMarkedForDeletion() && !ref->As<RE::Actor>()) {
                    ++references;
                    visit(visit, ref->Get3D(), ref->GetHandle(), 0);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        if (ms > 4.0 && g_slowLogged < 12) {
            ++g_slowLogged;
            logger::info("Snow meshes: the scan took {:.1f} ms ({} references, {} nodes, {} new shapes tried, {} held)",
                         ms, references, visited, captured, g_meshes.size());
        }
    }

    bool FindContact(float x, float y, float bottom, float clearance, Contact& result) {
        if (!Settings::enableAuthoredMeshDeformation) return false;
        Timed timed;
        std::lock_guard guard(g_lock);
        float nearest = std::numeric_limits<float>::infinity();
        bool found = false;
        for (const auto& [key, m] : g_meshes) {
            if (m.moving || x < m.minX || x > m.maxX || y < m.minY || y > m.maxY || bottom < m.minZ - clearance ||
                bottom > m.maxZ + clearance)
                continue;
            if (m.owner) {
                auto owner = m.owner.get();
                if (!owner || owner->IsDisabled() || owner->IsMarkedForDeletion() || !owner->Get3D()) continue;
            }
            m.grid.ForEach(x, y, [&](uint32_t index) {
                const auto& face = m.faces[index];
                float z{}, gx{}, gy{};
                if (!AuthoredSnowMath::Height(face, x, y, z, gx, gy) ||
                    !AuthoredSnowMath::Contact(bottom, z, clearance))
                    return;
                const float distance = std::abs(bottom - z);
                if (distance < nearest) {
                    nearest = distance;
                    result = {m.id, z, gx, gy};
                    found = true;
                }
            });
        }
        return found;
    }

    void NotePress(const Clipmap::Stamp& stamp) {
        if (!Settings::enableAuthoredMeshDeformation || stamp.surface == MeshSnowSurface::Kind::kNone) return;
        const float strength = std::max({stamp.depth, stamp.rim, 0.1f});
        float seconds = 3600.0f;
        if (stamp.decay <= 0.0f)
            seconds = 1.0f;
        else if (stamp.decay < 0.9999f)
            seconds = std::clamp(std::log(0.02f / strength) / std::log(stamp.decay), 1.0f, 3600.0f);
        const auto until = Clock::now() + std::chrono::milliseconds(int64_t((seconds + 2.0f) * 1000.0f));
        const float reach = std::max(stamp.radius, stamp.halfWidth) * 3.0f + std::abs(stamp.motionX) +
                            std::abs(stamp.motionY) + 8.0f;
        const float band = Settings::meshSnowHeightBand;
        Timed timed;
        std::lock_guard guard(g_lock);
        for (auto& [key, m] : g_meshes) {
            if (m.moving || stamp.x < m.minX - reach || stamp.x > m.maxX + reach || stamp.y < m.minY - reach ||
                stamp.y > m.maxY + reach || stamp.z < m.minZ - band || stamp.z > m.maxZ + band)
                continue;
            m.pressedUntil = std::max(m.pressedUntil, until);
        }
    }

    void NewFrame() {
        std::lock_guard guard(g_lock);
        g_readbacks = 0;
        g_frameTriangles = 0;
    }

    bool NeedsDraw(RE::BSGeometry* geometry) {
        if (!Clipmap::MeshFieldReady()) return false;
        Timed timed;
        std::lock_guard guard(g_lock);
        auto it = g_meshes.find(geometry);
        if (it == g_meshes.end()) {
            const auto& b = geometry->worldBound;
            if (std::abs(g_anchor.x - b.center.x) > b.radius + 128 ||
                std::abs(g_anchor.y - b.center.y) > b.radius + 128 ||
                std::abs(g_anchor.z - b.center.z) > b.radius + 128)
                return false;
            const auto now = Clock::now();
            if (auto retry = g_retry.find(geometry); retry != g_retry.end() && now < retry->second) return false;
            if (g_retry.size() >= 128) g_retry.clear();
            g_retry[geometry] = now + std::chrono::seconds(1);
            Capture(geometry, true);
            return false;
        }
        auto& m = it->second;
        const auto now = Clock::now();
        m.seen = now;
        if (m.moving) {
            Capture(geometry, false);
            return false;
        }
        if (!SamePlace(m.transform, geometry->world) || m.renderer != geometry->GetGeometryRuntimeData().rendererData) {
            Capture(geometry, false);
            return false;
        }
        if (now >= m.pressedUntil) return false;
        float cx{}, cy{}, half{};
        if (Clipmap::GetWindow(cx, cy, half) && Outside(m, cx, cy, half)) {
            m.pressedUntil = {};
            return false;
        }
        ++g_reportRouted;
        return true;
    }

    void Reset() {
        std::lock_guard guard(g_lock);
        g_meshes.clear();
        g_retry.clear();
        g_scanCountdown = 0;
        g_skipsLogged = 0;
        g_movingLogged = g_againLogged = g_settledLogged = g_slowLogged = 0;
        g_loggedTextures.clear();
        g_nextReport = {};
        g_reportFrames = g_reportRouted = g_reportCaptured = g_reportRefused = g_reportLeft = 0;
        g_textureGeneration.fetch_add(1, std::memory_order_relaxed);
    }

    void Release() { Reset(); }
}
