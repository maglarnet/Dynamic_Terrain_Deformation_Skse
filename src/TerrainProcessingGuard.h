// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

class TerrainProcessingGuard
{
public:
    static bool Mark(ID3D11DeviceChild* shader)
    {
        const UINT marker = 1;
        return shader && SUCCEEDED(shader->SetPrivateData(ownershipGuid_, sizeof(marker), &marker));
    }
    static bool Owns(ID3D11DeviceChild* shader)
    {
        UINT marker{}, size = sizeof(marker);
        return shader && SUCCEEDED(shader->GetPrivateData(ownershipGuid_, &size, &marker)) && size == sizeof(marker) && marker == 1;
    }
    explicit TerrainProcessingGuard(ID3D11DeviceContext* context) : context_(context)
    {
        D3D11_PRIMITIVE_TOPOLOGY topology{}; context->IAGetPrimitiveTopology(&topology);
        if (topology >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST &&
            topology <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST) { return; }
        UINT hullClasses{}, domainClasses{};
        context->HSGetShader(hull_.GetAddressOf(), nullptr, &hullClasses);
        context->DSGetShader(domain_.GetAddressOf(), nullptr, &domainClasses);
        if (hullClasses || domainClasses || !Owns(hull_.Get()) || !Owns(domain_.Get())) { return; }
        context->HSSetShader(nullptr, nullptr, 0); context->DSSetShader(nullptr, nullptr, 0);
        active_ = true;
    }
    ~TerrainProcessingGuard()
    {
        if (!active_) { return; }
        context_->HSSetShader(hull_.Get(), nullptr, 0); context_->DSSetShader(domain_.Get(), nullptr, 0);
    }
    TerrainProcessingGuard(const TerrainProcessingGuard&) = delete;
    TerrainProcessingGuard& operator=(const TerrainProcessingGuard&) = delete;
    bool Active() const { return active_; }
private:
    static inline constexpr GUID ownershipGuid_{0x52bcfa7a,0x9b72,0x48c0,{0x98,0x44,0x31,0xb8,0x76,0x25,0xfa,0xaa}};
    ID3D11DeviceContext* context_{};
    Microsoft::WRL::ComPtr<ID3D11HullShader> hull_;
    Microsoft::WRL::ComPtr<ID3D11DomainShader> domain_;
    bool active_{};
};
