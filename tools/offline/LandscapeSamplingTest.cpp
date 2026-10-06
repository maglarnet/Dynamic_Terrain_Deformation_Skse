// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
void Check(HRESULT hr) { if (FAILED(hr)) { throw std::runtime_error("D3D11 operation failed"); } }
constexpr UINT Side = 256, Count = Side * Side;
using Result = std::array<float, 4>;

std::vector<Result> Run(ID3D11Device* device, ID3D11DeviceContext* context, const char* path)
{
    std::ifstream file(path);
    if (!file) { throw std::runtime_error("Cannot open generated HLSL"); }
    std::string source{std::istreambuf_iterator<char>(file), {}};
    const auto end = source.find("struct PatchConstants");
    if (end == std::string::npos) { throw std::runtime_error("Missing generated prologue"); }
    source.resize(end);
    source += R"(
RWStructuredBuffer<float4> Results : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint x = id.x % 256, y = id.x / 256;
    float2 xy = (float2(x, y) / 255.0f - 0.5f) * 10000.0f;
    if (y < 16) {
        float boundaries[4] = { Window.z, Window.w, Window1.z, Window1.w };
        xy.x = boundaries[y % 4] + (int(x % 5) - 2) * 0.001f;
        xy.y = (int(x / 5) - 25) * 20.0f;
        if (y >= 4 && y < 8) xy.x = -xy.x;
        if (y >= 8) xy = xy.yx;
    }
    float3 p = float3(xy - CameraPosAdjust.xy, 0.0f);
    float h = SurfaceOffset(p);
    float2 gradient = (float2(SurfaceOffset(p + float3(kGradEps, 0, 0)),
        SurfaceOffset(p + float3(0, kGradEps, 0))) - h) / kGradEps;
    Results[id.x] = float4(h, gradient, length(gradient));
}
)";
    ComPtr<ID3DBlob> code, errors;
    const HRESULT compiled = D3DCompile(source.data(), source.size(), path, nullptr, nullptr,
        "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(compiled) && errors) { std::fprintf(stderr, "%s\n", static_cast<char*>(errors->GetBufferPointer())); }
    Check(compiled);
    ComPtr<ID3D11ComputeShader> shader;
    Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));

    std::array<ComPtr<ID3D11ShaderResourceView>, 4> views;
    for (UINT slot = 0; slot < 4; ++slot) {
        std::array<float, 64 * 64> pixels;
        for (UINT i = 0; i < pixels.size(); ++i) {
            const float wave = std::sin(i * 0.73f + slot * 1.9f);
            pixels[i] = slot == 0 || slot == 2 ? wave * 24.0f : (wave + 1.0f) * 0.5f;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 64; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels.data(), 64 * sizeof(float), 0};
        ComPtr<ID3D11Texture2D> texture;
        Check(device->CreateTexture2D(&desc, &data, &texture));
        Check(device->CreateShaderResourceView(texture.Get(), nullptr, &views[slot]));
    }
    ID3D11ShaderResourceView* resources[]{ views[0].Get(), views[1].Get(), views[2].Get(), views[3].Get() };
    context->CSSetShaderResources(0, 4, resources);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    Check(device->CreateSamplerState(&sd, &sampler));
    ID3D11SamplerState* samplers[]{sampler.Get(), sampler.Get(), sampler.Get()};
    context->CSSetSamplers(0, 3, samplers);

    std::array<float, 164> camera{};
    camera[160] = 12345.0f; camera[161] = -4321.0f;
    std::array<float, 12> window{0, 0, 600, 768, 0.7f, 0, 0, 0, 0, 0, 2400, 3072};
    std::array<ComPtr<ID3D11Buffer>, 2> constants;
    for (UINT i = 0; i < 2; ++i) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = i == 0 ? sizeof(camera) : sizeof(window);
        bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA data{i == 0 ? camera.data() : window.data(), 0, 0};
        Check(device->CreateBuffer(&bd, &data, &constants[i]));
    }
    ID3D11Buffer* buffers[]{constants[0].Get(), constants[1].Get()};
    context->CSSetConstantBuffers(12, 2, buffers);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = Count * sizeof(Result); bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(Result);
    ComPtr<ID3D11Buffer> output, staging;
    Check(device->CreateBuffer(&bd, nullptr, &output));
    ComPtr<ID3D11UnorderedAccessView> uav;
    Check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
    bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = bd.MiscFlags = bd.StructureByteStride = 0;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Check(device->CreateBuffer(&bd, nullptr, &staging));
    ID3D11UnorderedAccessView* targets[]{uav.Get()};
    context->CSSetUnorderedAccessViews(0, 1, targets, nullptr);
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->Dispatch(Count / 64, 1, 1);
    context->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    const auto first = static_cast<const Result*>(mapped.pData);
    std::vector<Result> result(first, first + Count);
    context->Unmap(staging.Get(), 0);
    context->ClearState();
    return result;
}

int main(int argc, char** argv)
{
    try {
        if (argc != 3) { throw std::runtime_error("Usage: LandscapeSamplingTest before.hlsl after.hlsl"); }
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
            D3D11_SDK_VERSION, &device, nullptr, &context));
        const auto before = Run(device.Get(), context.Get(), argv[1]);
        const auto after = Run(device.Get(), context.Get(), argv[2]);
        float maxHeightError = 0, maxGradientError = 0;
        for (UINT i = 0; i < Count; ++i) {
            for (UINT j = 0; j < 4; ++j) {
                if (!std::isfinite(before[i][j]) || !std::isfinite(after[i][j])) {
                    throw std::runtime_error("Non-finite displacement/gradient");
                }
                const float error = std::abs(before[i][j] - after[i][j]);
                auto& worst = j == 0 ? maxHeightError : maxGradientError;
                worst = (std::max)(worst, error);
                if (error > 0.0001f) { throw std::runtime_error("Displacement/gradient mismatch"); }
            }
        }
        std::printf("PASS %u positions: maximum height error %.9g, gradient error %.9g (WARP correctness only)\n",
            Count, maxHeightError, maxGradientError);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what()); return 1;
    }
}
