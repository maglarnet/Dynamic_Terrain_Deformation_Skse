// SPDX-License-Identifier: GPL-3.0-only
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "AuthoredSnowMath.h"
#include "MeshSnowSurface.h"
#include "SnowMarkDepth.h"
using Microsoft::WRL::ComPtr;
void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

namespace {
    constexpr int kTexels = 8;
    constexpr float kBand = 64.0f;

    float Height(int x, int y) { return 1.0f + float(x * x) + 3.0f * float(y * y); }

    float Code(int x, int y) {
        using namespace MeshSnowSurface;
        if (x == 5 && y == 5) return Encode(Kind::kMesh, 300.0f);
        if (x == 1 && y == 6) return 0.0f;
        return x < 4 ? Encode(Kind::kGround, 100.0f) : Encode(Kind::kMesh, 100.0f);
    }

    float Expected(float u, float v, float z, bool land) {
        const float tx = u * kTexels - 0.5f, ty = v * kTexels - 0.5f;
        const int i = int(std::floor(tx)), j = int(std::floor(ty));
        const float fx = tx - float(i), fy = ty - float(j);
        const auto at = [&](int x, int y) {
            x &= kTexels - 1;
            y &= kTexels - 1;
            return Height(x, y) * MeshSnowSurface::Weight(Code(x, y), z, kBand, land);
        };
        const float top = at(i, j) + (at(i + 1, j) - at(i, j)) * fx;
        const float bottom = at(i, j + 1) + (at(i + 1, j + 1) - at(i, j + 1)) * fx;
        return top + (bottom - top) * fy;
    }
}

int main() {
    try {
        using namespace AuthoredSnowMath;
        Require(std::abs(SnowMarkDepth::Resolve(4.7f, 35.0f, 0.5f) - 17.5f) < 1e-4f,
                "mesh and full terrain snow use identical trench depth");
        Require(SnowMarkDepth::Resolve(20.0f, 35.0f, 0.5f) == 20.0f &&
                    SnowMarkDepth::Resolve(4.7f, 0.0f, 0.5f) == 4.7f &&
                    SnowMarkDepth::Resolve(4.7f, 35.0f, 0.5f, 0.0f) == 4.7f,
                "profile depth, disabled blanket, and sink limits");
        std::puts("PASS shared terrain/mesh snow trench depth and ordinary-depth fallback");

        Require(Matches("Roof_NMN_aSnow_01") && Matches("nmn_asnow") && !Matches("SnowRoof"), "shape recognition");
        Require(MatchesTexture("textures\\landscape\\Snow01.dds", "landscape\\snow"), "snow diffuse");
        Require(MatchesTexture("Textures/Landscape/SNOW02.dds", "landscape\\snow"), "forward slashes and case");
        Require(MatchesTexture("textures\\mymod\\drift.dds", "landscape\\snow, mymod\\drift"), "second piece");
        Require(!MatchesTexture("textures\\dungeons\\RidgedStone05.dds", "landscape\\snow"), "stone is not snow");
        Require(!MatchesTexture("textures\\landscape\\Snow01.dds", ""), "an empty list matches nothing");
        Require(!MatchesTexture("textures\\landscape\\Snow01.dds", " , "), "blank pieces match nothing");
        Require(MatchesTexture("textures\\landscape\\Snow01.dds", "landscape\\snow0") &&
                    MatchesTexture("textures\\landscape\\snow02.dds", "landscape\\snow0") &&
                    !MatchesTexture("textures\\landscape\\SnowRocks01.dds", "landscape\\snow0"),
                "the default takes snow and leaves out snow rocks");
        std::puts("PASS snow mesh names and diffuse texture pieces");

        Triangle roof{{0, 0, 100}, {10, 0, 120}, {0, 10, 100}};
        float z{}, gx{}, gy{};
        Require(Height(roof, 2, 2, z, gx, gy) && std::abs(z - 104) < 1e-4f && std::abs(gx - 2) < 1e-4f &&
                    std::abs(gy) < 1e-4f,
                "sloping triangle plane");
        Require(!Height(roof, 9, 9, z, gx, gy), "outside triangle");
        Require(!Height({{0, 0, 0}, {0, 0, 10}, {0, 10, 0}}, 0, 0, z, gx, gy), "vertical wall");
        Require(!Height({{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}, 0, 0, z, gx, gy), "degenerate triangle");
        Require(Contact(104, 100, 8) && !Contact(120, 100, 8) && !Contact(NAN, 100, 8),
                "contact clearance/airborne/invalid");
        std::puts("PASS finite triangle contacts, slope, walls, and airborne rejection");

        Require(std::abs(FloorDepth(12.25f, 104, 100) - 4) < 1e-5f, "print stops at the walkway");
        Require(std::abs(FloorDepth(2.1f, 104, 100) - 2.1f) < 1e-5f, "deep snow keeps the profile depth");
        Require(FloorDepth(12.25f, 100.2f, 100.2f) == 0.0f, "drift collision: rim only");
        Require(FloorDepth(12.25f, 98, 100) == 0.0f && FloorDepth(-3, 104, 100) == 0.0f, "never a raise");
        Require(FloorDepth(NAN, 104, 100) == 0.0f && FloorDepth(4, 104, INFINITY) == 0.0f, "invalid input");
        std::puts("PASS mesh prints stop at the floor the actor stands on");

        std::vector<Triangle> faces;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x) {
                const float x0 = x * 50.0f, y0 = y * 50.0f, x1 = x0 + 50, y1 = y0 + 50;
                faces.push_back({{x0, y0, 10}, {x1, y0, 10}, {x0, y1, 10}});
                faces.push_back({{x1, y0, 10}, {x1, y1, 10}, {x0, y1, 10}});
            }
        FaceGrid grid;
        grid.Build(faces, 0, 0, 2000, 2000);
        Require(grid.width <= 64 && grid.height <= 64 && grid.cell >= 32, "grid bounds");
        size_t tested = 0, most = 0;
        for (float y = 1; y < 2000; y += 37)
            for (float x = 1; x < 2000; x += 41) {
                bool hit = false;
                size_t count = 0;
                grid.ForEach(x, y, [&](uint32_t i) {
                    ++count;
                    hit |= Height(faces[i], x, y, z, gx, gy);
                });
                Require(hit, "a point on the plane found no face");
                most = std::max(most, count);
                ++tested;
            }
        Require(most < 64, "a contact still tests a large part of the mesh");
        bool outside = false;
        grid.ForEach(-50, 100, [&](uint32_t) { outside = true; });
        grid.ForEach(100, 2100, [&](uint32_t) { outside = true; });
        Require(!outside, "a point off the mesh visited faces");
        std::vector<Triangle> huge{{{0, 0, 0}, {7500, 0, 0}, {0, 7500, 0}}, {{7500, 0, 0}, {7500, 7500, 0}, {0, 7500, 0}}};
        FaceGrid wide;
        wide.Build(huge, 0, 0, 7500, 7500);
        bool found = false;
        wide.ForEach(7400, 7400, [&](uint32_t i) { found |= Height(huge[i], 7400, 7400, z, gx, gy); });
        Require(found, "a face spanning the whole grid was lost");
        std::printf("PASS face grid: %zu points on 3,200 faces, at most %zu tested each; spanning faces kept\n", tested,
                    most);

        {
            using namespace MeshSnowSurface;
            Require(KindOf(0.0f) == Kind::kNone && KindOf(Encode(Kind::kGround, -30000.0f)) == Kind::kGround &&
                        KindOf(Encode(Kind::kGround, 30000.0f)) == Kind::kGround &&
                        KindOf(Encode(Kind::kMesh, -30000.0f)) == Kind::kMesh &&
                        KindOf(Encode(Kind::kMesh, 30000.0f)) == Kind::kMesh,
                    "surface kinds over the world's height range");
            Require(std::abs(HeightOf(Encode(Kind::kMesh, 1234.5f)) - 1234.5f) < 0.26f &&
                        std::abs(HeightOf(Encode(Kind::kGround, -987.25f)) + 987.25f) < 0.13f,
                    "surface heights survive the encoding");
            const float ground = Encode(Kind::kGround, 100), mesh = Encode(Kind::kMesh, 100);
            Require(!Differ(ground, Encode(Kind::kGround, 900), kBand), "two land dents never differ");
            Require(!Differ(ground, mesh, kBand) && Differ(mesh, Encode(Kind::kMesh, 300), kBand) &&
                        Differ(mesh, 0.0f, kBand) && !Differ(0.0f, ground, kBand),
                    "surfaces differ by height only when a mesh is involved");
            Require(Weight(0.0f, 100, kBand, true) == 1 && Weight(0.0f, 100, kBand, false) == 0 &&
                        Weight(ground, 900, kBand, true) == 1 && Weight(Encode(Kind::kMesh, 300), 100, kBand, true) == 0 &&
                        Weight(mesh, 140, kBand, false) == 1 && Weight(mesh, 170, kBand, false) == 0 &&
                        Weight(mesh, 156, kBand, false) > 0 && Weight(mesh, 156, kBand, false) < 1,
                    "surface weights for the land and for meshes");
            std::puts("PASS surface codes: kinds, heights, which surfaces differ, and weights");
        }

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> ctx;
        Require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                            &device, nullptr, &ctx)),
                "WARP device");
        std::string source = MeshSnowSurface::hlsl;
        source += R"(
Texture2D<float> Heights : register(t0);
Texture2D<float> Surfaces : register(t1);
SamplerState Linear : register(s0);
StructuredBuffer<float4> Queries : register(t2);
RWStructuredBuffer<float2> Output : register(u0);
[numthreads(1,1,1)] void main(uint3 id : SV_DispatchThreadID) {
 const float4 q = Queries[id.x];
 Output[id.x] = float2(GatedField(Heights, Surfaces, Linear, q.xy, 8.0f, q.z, 64.0f, q.w > 0.5f),
  Heights.SampleLevel(Linear, q.xy, 0));
}
)";
        ComPtr<ID3DBlob> code, errors;
        const auto hr =
            D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &errors);
        if (FAILED(hr)) {
            if (errors) std::printf("%s", static_cast<const char*>(errors->GetBufferPointer()));
            throw std::runtime_error("gated field shader compile");
        }
        ComPtr<ID3D11ComputeShader> shader;
        Require(SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader)),
                "shader creation");

        std::array<float, kTexels * kTexels> heights{}, codes{};
        for (int y = 0; y < kTexels; ++y)
            for (int x = 0; x < kTexels; ++x) {
                heights[y * kTexels + x] = Height(x, y);
                codes[y * kTexels + x] = Code(x, y);
            }
        const auto texture = [&](const float* data, ComPtr<ID3D11ShaderResourceView>& view) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = desc.Height = kTexels;
            desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA initial{data, kTexels * sizeof(float), 0};
            ComPtr<ID3D11Texture2D> t;
            Require(SUCCEEDED(device->CreateTexture2D(&desc, &initial, &t)) &&
                        SUCCEEDED(device->CreateShaderResourceView(t.Get(), nullptr, &view)),
                    "test texture");
        };
        ComPtr<ID3D11ShaderResourceView> heightView, codeView;
        texture(heights.data(), heightView);
        texture(codes.data(), codeView);

        struct Query {
            float u, v, z, land;
        };
        std::vector<Query> queries;
        for (const float land : {1.0f, 0.0f})
            for (const float z : {100.0f, 150.0f, 300.0f})
                for (const auto& [u, v] : std::initializer_list<std::pair<float, float>>{
                         {0.23f, 0.31f}, {0.47f, 0.52f}, {0.61f, 0.66f}, {0.70f, 0.72f}, {0.19f, 0.83f},
                         {0.97f, 0.05f}, {0.53f, 0.13f}})
                    queries.push_back({u, v, z, land});
        D3D11_BUFFER_DESC qd{};
        qd.ByteWidth = UINT(queries.size() * sizeof(Query));
        qd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        qd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        qd.StructureByteStride = sizeof(Query);
        D3D11_SUBRESOURCE_DATA qi{queries.data(), 0, 0};
        ComPtr<ID3D11Buffer> queryBuffer, outBuffer, readBuffer;
        Require(SUCCEEDED(device->CreateBuffer(&qd, &qi, &queryBuffer)), "query buffer");
        ComPtr<ID3D11ShaderResourceView> queryView;
        Require(SUCCEEDED(device->CreateShaderResourceView(queryBuffer.Get(), nullptr, &queryView)), "query view");
        D3D11_BUFFER_DESC od{};
        od.ByteWidth = UINT(queries.size() * 2 * sizeof(float));
        od.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        od.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        od.StructureByteStride = 2 * sizeof(float);
        Require(SUCCEEDED(device->CreateBuffer(&od, nullptr, &outBuffer)), "output buffer");
        ComPtr<ID3D11UnorderedAccessView> outView;
        Require(SUCCEEDED(device->CreateUnorderedAccessView(outBuffer.Get(), nullptr, &outView)), "output view");
        od.BindFlags = 0;
        od.MiscFlags = 0;
        od.Usage = D3D11_USAGE_STAGING;
        od.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Require(SUCCEEDED(device->CreateBuffer(&od, nullptr, &readBuffer)), "readback buffer");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        Require(SUCCEEDED(device->CreateSamplerState(&sd, &sampler)), "sampler");

        ID3D11ShaderResourceView* views[]{heightView.Get(), codeView.Get(), queryView.Get()};
        ctx->CSSetShader(shader.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 3, views);
        ctx->CSSetSamplers(0, 1, sampler.GetAddressOf());
        ctx->CSSetUnorderedAccessViews(0, 1, outView.GetAddressOf(), nullptr);
        ctx->Dispatch(UINT(queries.size()), 1, 1);
        ctx->CopyResource(readBuffer.Get(), outBuffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Require(SUCCEEDED(ctx->Map(readBuffer.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map output");
        const auto* out = static_cast<const float*>(mapped.pData);
        float worst = 0;
        for (size_t i = 0; i < queries.size(); ++i) {
            const auto& q = queries[i];
            const float expected = Expected(q.u, q.v, q.z, q.land > 0.5f);
            const float gated = out[i * 2], plain = out[i * 2 + 1];
            const float tolerance = (q.land > 0.5f && gated == plain) ? 0.02f * std::max(1.0f, std::abs(expected)) : 1e-3f;
            if (std::abs(gated - expected) > tolerance) {
                std::printf("query %zu uv (%.2f, %.2f) z %.0f land %.0f: gated %.4f expected %.4f plain %.4f\n", i,
                            q.u, q.v, q.z, q.land, gated, expected, plain);
                ctx->Unmap(readBuffer.Get(), 0);
                throw std::runtime_error("gated field differs from the CPU");
            }
            worst = std::max(worst, std::abs(gated - expected));
        }
        ctx->Unmap(readBuffer.Get(), 0);
        std::printf("PASS gated field on the GPU: %zu samples, land and mesh, gather order and weights (worst %.5f)\n",
                    queries.size(), worst);
        std::puts("ALL PASS");
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
