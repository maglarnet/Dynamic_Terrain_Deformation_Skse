// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>
namespace AuthoredSnowMath {
    struct Point {
        float x{}, y{}, z{};
    };
    struct Triangle {
        Point a, b, c;
    };
    inline bool Matches(std::string_view name) {
        constexpr std::string_view marker = "nmn_asnow";
        return std::search(name.begin(), name.end(), marker.begin(), marker.end(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == b;
               }) != name.end();
    }
    inline bool MatchesTexture(std::string_view path, std::string_view pieces) {
        const auto fold = [](char c) {
            return c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        };
        while (!pieces.empty()) {
            const auto end = pieces.find(',');
            auto piece = pieces.substr(0, end);
            while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t')) piece.remove_prefix(1);
            while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t')) piece.remove_suffix(1);
            if (!piece.empty() &&
                std::search(path.begin(), path.end(), piece.begin(), piece.end(),
                            [&](char a, char b) { return fold(a) == fold(b); }) != path.end())
                return true;
            if (end == std::string_view::npos) break;
            pieces.remove_prefix(end + 1);
        }
        return false;
    }
    inline bool Height(const Triangle& t, float x, float y, float& z, float& gx, float& gy) {
        const float ux = t.b.x - t.a.x, uy = t.b.y - t.a.y, uz = t.b.z - t.a.z;
        const float vx = t.c.x - t.a.x, vy = t.c.y - t.a.y, vz = t.c.z - t.a.z;
        const float det = ux * vy - uy * vx;
        if (!std::isfinite(det) || std::abs(det) < 1e-5f) return false;
        const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz;
        if (det * det < 0.067f * (nx * nx + ny * ny + det * det)) return false;
        const float dx = x - t.a.x, dy = y - t.a.y;
        const float u = (dx * vy - dy * vx) / det, v = (ux * dy - uy * dx) / det;
        if (u < -1e-4f || v < -1e-4f || u + v > 1.0001f) return false;
        gx = -nx / det;
        gy = -ny / det;
        z = t.a.z + gx * dx + gy * dy;
        return std::isfinite(z) && std::isfinite(gx) && std::isfinite(gy);
    }
    inline bool Contact(float bottom, float height, float clearance) {
        return std::isfinite(bottom) && std::isfinite(height) && std::isfinite(clearance) && clearance >= 0 &&
               std::abs(bottom - height) <= clearance;
    }
    inline float FloorDepth(float depth, float surface, float floor) {
        if (!std::isfinite(depth) || !std::isfinite(surface) || !std::isfinite(floor)) return 0.0f;
        return std::clamp(surface - floor, 0.0f, std::max(depth, 0.0f));
    }
    struct FaceGrid {
        float minX{}, minY{}, cell{1};
        uint32_t width{}, height{};
        std::vector<uint32_t> start, items;
        void Build(const std::vector<Triangle>& faces, float lx, float ly, float hx, float hy) {
            minX = lx;
            minY = ly;
            cell = std::max({32.0f, (hx - lx) / 64.0f, (hy - ly) / 64.0f});
            width = std::clamp(uint32_t(std::ceil((hx - lx) / cell)), 1u, 64u);
            height = std::clamp(uint32_t(std::ceil((hy - ly) / cell)), 1u, 64u);
            start.assign(size_t(width) * height + 1, 0);
            items.clear();
            const auto span = [&](const Triangle& f, uint32_t& x0, uint32_t& y0, uint32_t& x1, uint32_t& y1) {
                x0 = Index(std::min({f.a.x, f.b.x, f.c.x}) - minX, width);
                x1 = Index(std::max({f.a.x, f.b.x, f.c.x}) - minX, width);
                y0 = Index(std::min({f.a.y, f.b.y, f.c.y}) - minY, height);
                y1 = Index(std::max({f.a.y, f.b.y, f.c.y}) - minY, height);
            };
            for (const auto& f : faces) {
                uint32_t x0, y0, x1, y1;
                span(f, x0, y0, x1, y1);
                for (uint32_t y = y0; y <= y1; ++y)
                    for (uint32_t x = x0; x <= x1; ++x) ++start[size_t(y) * width + x + 1];
            }
            for (size_t i = 1; i < start.size(); ++i) start[i] += start[i - 1];
            items.resize(start.back());
            std::vector<uint32_t> fill(start.begin(), start.end() - 1);
            for (uint32_t i = 0; i < faces.size(); ++i) {
                uint32_t x0, y0, x1, y1;
                span(faces[i], x0, y0, x1, y1);
                for (uint32_t y = y0; y <= y1; ++y)
                    for (uint32_t x = x0; x <= x1; ++x) items[fill[size_t(y) * width + x]++] = i;
            }
        }
        uint32_t Index(float offset, uint32_t count) const {
            const float i = std::floor(offset / cell);
            return i <= 0 ? 0u : std::min(uint32_t(i), count - 1);
        }
        template <class Visit>
        void ForEach(float x, float y, Visit&& visit) const {
            if (!width || !height) return;
            const float ox = x - minX, oy = y - minY;
            if (ox < 0 || oy < 0 || ox > float(width) * cell || oy > float(height) * cell) return;
            const size_t c = size_t(Index(oy, height)) * width + Index(ox, width);
            for (uint32_t i = start[c]; i < start[c + 1]; ++i) visit(items[i]);
        }
    };
}
