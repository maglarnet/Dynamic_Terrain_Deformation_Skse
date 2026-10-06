// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <vector>

namespace TerrainVertexProbe
{
using Microsoft::WRL::ComPtr;
struct Vertex { float clip[4]; float world[4]; };
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);

inline bool Capture(ID3D11DeviceContext* context, DrawFn draw, UINT start, INT base,
    bool withWorld, std::array<Vertex, 3>& vertices, std::vector<Vertex>* domainVertices = nullptr)
{
    ID3D11Buffer* targets[4]{};
    context->SOGetTargets(4, targets);
    bool occupied{};
    for (auto* target : targets) { if (target) { occupied = true; target->Release(); } }
    if (occupied) { return false; }
    ComPtr<ID3D11HullShader> hs; ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11GeometryShader> gs; ComPtr<ID3D11PixelShader> ps;
    UINT hCount{}, dCount{}, gCount{}, pCount{};
    context->HSGetShader(hs.GetAddressOf(), nullptr, &hCount);
    context->DSGetShader(ds.GetAddressOf(), nullptr, &dCount);
    context->GSGetShader(gs.GetAddressOf(), nullptr, &gCount);
    context->PSGetShader(ps.GetAddressOf(), nullptr, &pCount);
    if (hCount || dCount || gCount || pCount) { return false; }
    D3D11_PRIMITIVE_TOPOLOGY topology{}; context->IAGetPrimitiveTopology(&topology);
    ComPtr<ID3D11Device> device; context->GetDevice(device.GetAddressOf());
    const char* source = withWorld ?
        "struct I{float4 clip:SV_POSITION;float4 t0:TEXCOORD0;float3 t4:TEXCOORD4;float3 t1:TEXCOORD1;float3 t2:TEXCOORD2;float3 t3:TEXCOORD3;float3 t5:TEXCOORD5;float4 t6:TEXCOORD6;float4 t7:TEXCOORD7;float3 t8:TEXCOORD8;float3 t9:TEXCOORD9;float3 t10:TEXCOORD10;float4 world:POSITION1;float4 previous:POSITION2;float4 c0:COLOR0;float4 c1:COLOR1;};struct O{float4 clip:POSITION0;float4 world:POSITION1;};"
        "[maxvertexcount(3)]void main(triangle I v[3],inout TriangleStream<O> s){for(uint i=0;i<3;++i){O o;o.clip=v[i].clip;o.world=v[i].world;s.Append(o);}}" :
        "struct I{float4 clip:SV_POSITION;};struct O{float4 clip:POSITION0;float4 world:POSITION1;};"
        "[maxvertexcount(3)]void main(triangle I v[3],inout TriangleStream<O> s){for(uint i=0;i<3;++i){O o;o.clip=v[i].clip;o.world=0;s.Append(o);}}";
    ComPtr<ID3DBlob> code;
    if (FAILED(D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", "gs_5_0", 0, 0, code.GetAddressOf(), nullptr))) { return false; }
    const D3D11_SO_DECLARATION_ENTRY entries[] = {{0,"POSITION",0,0,4,0},{0,"POSITION",1,0,4,0}};
    const UINT stride = sizeof(Vertex);
    ComPtr<ID3D11GeometryShader> probe;
    if (FAILED(device->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(), entries, 2,
        &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, probe.GetAddressOf()))) { return false; }
    constexpr UINT domainCapacity = 32768;
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth = domainVertices ? domainCapacity * sizeof(Vertex) : sizeof(vertices); desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    ComPtr<ID3D11Buffer> output, staging;
    if (FAILED(device->CreateBuffer(&desc, nullptr, output.GetAddressOf()))) { return false; }
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateBuffer(&desc, nullptr, staging.GetAddressOf()))) { return false; }
    ComPtr<ID3D11Query> statistics;
    if (domainVertices) {
        if (!hs || !ds || topology != D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST) { return false; }
        D3D11_QUERY_DESC query{D3D11_QUERY_SO_STATISTICS, 0};
        if (FAILED(device->CreateQuery(&query, statistics.GetAddressOf()))) { return false; }
    } else {
        context->HSSetShader(nullptr, nullptr, 0); context->DSSetShader(nullptr, nullptr, 0);
    }
    context->GSSetShader(probe.Get(), nullptr, 0); context->PSSetShader(nullptr, nullptr, 0);
    if (!domainVertices) { context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); }
    auto* target = output.Get(); const UINT offset{}; context->SOSetTargets(1, &target, &offset);
    if (statistics) { context->Begin(statistics.Get()); }
    draw(context, 3, start, base);
    if (statistics) { context->End(statistics.Get()); }
    context->SOSetTargets(0, nullptr, nullptr);
    context->HSSetShader(hs.Get(), nullptr, 0); context->DSSetShader(ds.Get(), nullptr, 0);
    context->GSSetShader(gs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
    context->IASetPrimitiveTopology(topology);
    context->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) { return false; }
    bool valid = true;
    if (domainVertices) {
        D3D11_QUERY_DATA_SO_STATISTICS result{};
        valid = context->GetData(statistics.Get(), &result, sizeof(result), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            result.NumPrimitivesWritten > 0 && result.NumPrimitivesWritten == result.PrimitivesStorageNeeded &&
            result.NumPrimitivesWritten <= domainCapacity / 3;
        if (valid) {
            domainVertices->resize(static_cast<size_t>(result.NumPrimitivesWritten) * 3);
            std::memcpy(domainVertices->data(), mapped.pData, domainVertices->size() * sizeof(Vertex));
        }
    }
    if (valid) { std::memcpy(vertices.data(), mapped.pData, sizeof(vertices)); }
    context->Unmap(staging.Get(), 0);
    return valid;
}
}
