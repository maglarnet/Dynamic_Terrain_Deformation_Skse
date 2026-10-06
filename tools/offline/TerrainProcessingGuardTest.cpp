// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).
#include "TerrainVertexProbe.h"
#include "TerrainProcessingGuard.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <d3d11sdklayers.h>

using Microsoft::WRL::ComPtr;
static void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* c, UINT count, UINT start, INT base) { c->DrawIndexed(count,start,base); }
int main()
{
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,device.GetAddressOf(),nullptr,context.GetAddressOf()))) { return 1; }
    const char* source="struct O{float4 clip:SV_POSITION;float4 t0:TEXCOORD0;float3 t4:TEXCOORD4;float3 t1:TEXCOORD1;float3 t2:TEXCOORD2;float3 t3:TEXCOORD3;float3 t5:TEXCOORD5;float4 t6:TEXCOORD6;float4 t7:TEXCOORD7;float3 t8:TEXCOORD8;float3 t9:TEXCOORD9;float3 t10:TEXCOORD10;float4 world:POSITION1;float4 previous:POSITION2;float4 c0:COLOR0;float4 c1:COLOR1;};O main(float4 p:POSITION0){O o=(O)0;o.clip=p+float4(0,0,10,0);o.t0=float4(.75,.75,.125,.375);o.world=p;return o;}";
    ComPtr<ID3DBlob> code; ComPtr<ID3D11VertexShader> shader;
    if (FAILED(D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,code.GetAddressOf(),nullptr)) ||
        FAILED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,shader.GetAddressOf()))) { return 2; }
    D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    ComPtr<ID3D11InputLayout> layout;
    if (FAILED(device->CreateInputLayout(&element,1,code->GetBufferPointer(),code->GetBufferSize(),layout.GetAddressOf()))) { return 3; }
    const float vertices[3][4]={{-.5f,-.5f,.1f,1},{.5f,-.5f,.2f,1},{0,.5f,.3f,1}};
    const UINT indices[3]={0,1,2};
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth=sizeof(vertices); desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem=vertices;
    ComPtr<ID3D11Buffer> vb,ib;
    if (FAILED(device->CreateBuffer(&desc,&initial,vb.GetAddressOf()))) { return 4; }
    desc.ByteWidth=sizeof(indices); desc.BindFlags=D3D11_BIND_INDEX_BUFFER; initial.pSysMem=indices;
    if (FAILED(device->CreateBuffer(&desc,&initial,ib.GetAddressOf()))) { return 5; }
    context->IASetInputLayout(layout.Get()); context->VSSetShader(shader.Get(),nullptr,0);
    auto* raw=vb.Get(); const UINT stride=16,offset=0; context->IASetVertexBuffers(0,1,&raw,&stride,&offset);
    context->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    for (const bool world : {false,true}) {
        std::array<TerrainVertexProbe::Vertex,3> captured{};
        if (!TerrainVertexProbe::Capture(context.Get(),Draw,0,0,world,captured)) { return 6; }
        for (unsigned i=0;i<3;++i) { for (unsigned j=0;j<4;++j) {
            if (std::abs(captured[i].clip[j]-(vertices[i][j]+(j==2?10.f:0.f)))>1e-5f ||
                std::abs(captured[i].world[j]-(world?vertices[i][j]:0.f))>1e-5f) { return 7; }
        } }
        D3D11_PRIMITIVE_TOPOLOGY topology{}; context->IAGetPrimitiveTopology(&topology);
        ComPtr<ID3D11VertexShader> afterVS; context->VSGetShader(afterVS.GetAddressOf(),nullptr,nullptr);
        ComPtr<ID3D11InputLayout> afterLayout; context->IAGetInputLayout(afterLayout.GetAddressOf());
        ComPtr<ID3D11Buffer> afterVB; UINT afterStride{},afterOffset{};
        context->IAGetVertexBuffers(0,1,afterVB.GetAddressOf(),&afterStride,&afterOffset);
        ComPtr<ID3D11Buffer> afterSO; context->SOGetTargets(1,afterSO.GetAddressOf());
        if (topology!=D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST || afterVS.Get()!=shader.Get() ||
            afterLayout.Get()!=layout.Get() || afterVB.Get()!=vb.Get() || afterStride!=16 || afterOffset || afterSO) { return 8; }
    }
    const std::string structure(source, std::strstr(source,"O main") - source);
    const std::string constants="struct C{float e[3]:SV_TessFactor;float inside:SV_InsideTessFactor;};";
    const std::string hull=structure+constants+
        "C factors(InputPatch<O,3> p){C c;c.e[0]=c.e[1]=c.e[2]=c.inside=2;return c;}"
        "[domain(\"tri\")][partitioning(\"integer\")][outputtopology(\"triangle_cw\")][outputcontrolpoints(3)][patchconstantfunc(\"factors\")]"
        "O main(InputPatch<O,3> p,uint i:SV_OutputControlPointID){return p[i];}";
    const std::string domain=structure+constants+
        "[domain(\"tri\")]O main(C c,float3 b:SV_DomainLocation,const OutputPatch<O,3> p){O o=(O)0;"
        "o.clip=p[0].clip*b.x+p[1].clip*b.y+p[2].clip*b.z+float4(0,0,7,0);"
        "o.world=p[0].world*b.x+p[1].world*b.y+p[2].world*b.z;return o;}";
    ComPtr<ID3DBlob> hsCode, dsCode;
    ComPtr<ID3D11HullShader> hs; ComPtr<ID3D11DomainShader> ds;
    if (FAILED(D3DCompile(hull.data(),hull.size(),nullptr,nullptr,nullptr,"main","hs_5_0",0,0,hsCode.GetAddressOf(),nullptr)) ||
        FAILED(D3DCompile(domain.data(),domain.size(),nullptr,nullptr,nullptr,"main","ds_5_0",0,0,dsCode.GetAddressOf(),nullptr)) ||
        FAILED(device->CreateHullShader(hsCode->GetBufferPointer(),hsCode->GetBufferSize(),nullptr,hs.GetAddressOf())) ||
        FAILED(device->CreateDomainShader(dsCode->GetBufferPointer(),dsCode->GetBufferSize(),nullptr,ds.GetAddressOf()))) { return 9; }
    context->HSSetShader(hs.Get(),nullptr,0); context->DSSetShader(ds.Get(),nullptr,0);
    std::array<TerrainVertexProbe::Vertex,3> first{}; std::vector<TerrainVertexProbe::Vertex> tessellated;
    const bool capturedDomain = TerrainVertexProbe::Capture(context.Get(),Draw,0,0,true,first,&tessellated);
    if (!capturedDomain || tessellated.size()!=18) { std::printf("Domain capture=%d vertices=%zu\n",capturedDomain,tessellated.size()); return 10; }
    for (const auto& v:tessellated) {
        for (unsigned j=0;j<4;++j) {
            if (std::abs(v.clip[j]-v.world[j]-(j==2?17.f:0.f))>1e-4f) { return 11; }
        }
    }
    ComPtr<ID3D11HullShader> afterHS; ComPtr<ID3D11DomainShader> afterDS;
    context->HSGetShader(afterHS.GetAddressOf(),nullptr,nullptr); context->DSGetShader(afterDS.GetAddressOf(),nullptr,nullptr);
    D3D11_PRIMITIVE_TOPOLOGY afterTopology{}; context->IAGetPrimitiveTopology(&afterTopology);
    if (afterHS.Get()!=hs.Get() || afterDS.Get()!=ds.Get() || afterTopology!=D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST) { return 12; }
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    { TerrainProcessingGuard guard(context.Get()); if (guard.Active()) { return 20; } }
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    if (!TerrainProcessingGuard::Mark(hs.Get()) || !TerrainProcessingGuard::Mark(ds.Get())) { return 19; }
    { TerrainProcessingGuard guard(context.Get()); if (guard.Active()) { return 13; } }
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    {
        TerrainProcessingGuard guard(context.Get());
        if (!guard.Active()) { return 14; }
        afterHS.Reset(); afterDS.Reset();
        context->HSGetShader(afterHS.GetAddressOf(),nullptr,nullptr); context->DSGetShader(afterDS.GetAddressOf(),nullptr,nullptr);
        if (afterHS || afterDS) { return 15; }
        std::array<TerrainVertexProbe::Vertex,3> safe{};
        if (!TerrainVertexProbe::Capture(context.Get(),Draw,0,0,true,safe)) { return 16; }
        for (unsigned i=0;i<3;++i) { for (unsigned j=0;j<4;++j) {
            if (std::abs(safe[i].clip[j]-vertices[i][j]-(j==2?10.f:0.f))>1e-5f) { return 17; }
        } }
    }
    afterHS.Reset(); afterDS.Reset();
    context->HSGetShader(afterHS.GetAddressOf(),nullptr,nullptr); context->DSGetShader(afterDS.GetAddressOf(),nullptr,nullptr);
    context->IAGetPrimitiveTopology(&afterTopology);
    if (afterHS.Get()!=hs.Get() || afterDS.Get()!=ds.Get() || afterTopology!=D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) { return 18; }
    std::puts("PASS WARP processing guard: preserves unowned stages and patch draws, suspends owned stages for triangle draws, restores shaders/topology and captures correct GPU output");
    return 0;
}
