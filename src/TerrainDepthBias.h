// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <limits>
#include <string>
#include <string_view>
namespace TerrainDepthBias
{

inline constexpr std::string_view capturedFingerprint = "98fa4a016ed7c8ec47e0a1f8a1268fcb122d9eef0b9ea4034926502e7adfc5c4";
inline constexpr std::string_view cs191Fingerprint = "912728acae006bf1d05e51e508c6eb52b57e326ea0a87977eb73712623524561";
inline std::string Fingerprint(std::string_view text)
{
    if (text.size() > (std::numeric_limits<ULONG>::max)()) return {};
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::array<UCHAR, 32> digest{};
    BCRYPT_HASH_HANDLE hash{};
    auto status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status >= 0) {
        status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
            static_cast<ULONG>(text.size()), 0);
        if (status >= 0) status = BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 15]);
    }
    return result;
}
inline std::string Instructions(std::string_view text)
{
    const auto start = text.find("vs_5_0");
    if (start == text.npos) return {};
    text.remove_prefix(start);
    std::string out;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (line.starts_with("//") || (!line.empty() && line.front() == '\0')) break;
        if (!line.empty()) { out.append(line); out.push_back('\n'); }
        if (end == text.npos) break;
        text.remove_prefix(end + 1);
    }
    return out;
}
inline float Recognize(std::string_view assembly)
{
    const auto fingerprint = Fingerprint(Instructions(assembly));
    if (fingerprint == capturedFingerprint) { return 10.0f; }
    if (fingerprint == cs191Fingerprint) { return 5.0f; }
    return 0.0f;
}
}
