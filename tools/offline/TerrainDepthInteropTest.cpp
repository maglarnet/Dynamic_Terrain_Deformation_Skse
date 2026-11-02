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
#include <stdexcept>
#include <string>
#include <vector>
#include "TerrainDepthCorrection.h"

using Microsoft::WRL::ComPtr;

static void Check(HRESULT result)
{
	if (FAILED(result)) { throw std::runtime_error("D3D operation failed: " + std::to_string(result)); }
}

int main()
{
	try {
		struct Input { float clip[4]; float params[4]; };
		std::vector<Input> inputs;
		for (float offset : { 0.0f, 5.0f, 10.0f, -5.0f }) {
			for (bool reverseZ : { false, true }) {
				for (bool reversedProjection : { false, true }) {
					for (float nearPlane : { 5.0f, 10.0f, 15.0f }) {
						const float farPlane = 10000.0f;
						const float a = reversedProjection ? -nearPlane / (farPlane - nearPlane) : farPlane / (farPlane - nearPlane);
						const float b = reversedProjection ? nearPlane * farPlane / (farPlane - nearPlane) : -nearPlane * a;
						for (float z : { 7.0f, 12.0f, 30.0f, 300.0f, 3000.0f }) {
							const float applied = reverseZ && reversedProjection ? -offset : offset;
							inputs.push_back({ { 123.0f, 45.0f, a * z + b + applied, z }, { offset, reverseZ ? 1.0f : 0.0f, a, b } });
						}
					}
				}
			}
		}

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
			D3D11_SDK_VERSION, &device, nullptr, &context));
		const std::string source = std::string(TerrainDepthCorrection::source) + R"hlsl(
struct Input { float4 clip; float4 params; };
StructuredBuffer<Input> inputs : register(t0);
RWStructuredBuffer<float4> results : register(u0);
RWStructuredBuffer<float> nearClips : register(u1);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint count, stride;
    inputs.GetDimensions(count, stride);
    if (id.x >= count) return;
    Input value = inputs[id.x];
    float4x4 projection = (float4x4)0;
    projection[2][2] = value.params.z;
    projection[3][2] = 1;
    float4 corrected = RemoveTerrainDepthOffset(value.clip, projection, value.params.xy);
    float homogeneousW = (corrected.z - value.params.z * corrected.w) / value.params.w;
    results[id.x] = float4(corrected.xy / homogeneousW, corrected.w / homogeneousW, corrected.w);
    nearClips[id.x] = TerrainNearClipDistance(corrected, projection);
}
)hlsl";
		ComPtr<ID3DBlob> bytecode, errors;
		const auto compiled = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
			"main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
		if (errors) { std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr); }
		Check(compiled);
		ComPtr<ID3D11ComputeShader> shader;
		Check(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &shader));
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = static_cast<UINT>(inputs.size() * sizeof(Input));
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = sizeof(Input);
		D3D11_SUBRESOURCE_DATA initial{ inputs.data(), 0, 0 };
		ComPtr<ID3D11Buffer> input, output, clipOutput, staging, clipStaging;
		Check(device->CreateBuffer(&desc, &initial, &input));
		ComPtr<ID3D11ShaderResourceView> srv;
		Check(device->CreateShaderResourceView(input.Get(), nullptr, &srv));
		desc.ByteWidth = static_cast<UINT>(inputs.size() * sizeof(float) * 4);
		desc.StructureByteStride = sizeof(float) * 4;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		Check(device->CreateBuffer(&desc, nullptr, &output));
		D3D11_BUFFER_DESC clipDesc = desc;
		clipDesc.ByteWidth = static_cast<UINT>(inputs.size() * sizeof(float));
		clipDesc.StructureByteStride = sizeof(float);
		Check(device->CreateBuffer(&clipDesc, nullptr, &clipOutput));
		ComPtr<ID3D11UnorderedAccessView> uav[2];
		Check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav[0]));
		Check(device->CreateUnorderedAccessView(clipOutput.Get(), nullptr, &uav[1]));
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		Check(device->CreateBuffer(&desc, nullptr, &staging));
		desc.ByteWidth = clipDesc.ByteWidth;
		Check(device->CreateBuffer(&desc, nullptr, &clipStaging));
		context->CSSetShader(shader.Get(), nullptr, 0);
		context->CSSetShaderResources(0, 1, srv.GetAddressOf());
		ID3D11UnorderedAccessView* views[2]{ uav[0].Get(), uav[1].Get() };
		context->CSSetUnorderedAccessViews(0, 2, views, nullptr);
		context->Dispatch(static_cast<UINT>((inputs.size() + 63) / 64), 1, 1);
		context->CopyResource(staging.Get(), output.Get());
		context->CopyResource(clipStaging.Get(), clipOutput.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
		const auto* values = static_cast<const std::array<float, 4>*>(mapped.pData);
		bool passed = true;
		for (size_t i = 0; i < inputs.size(); ++i) {
			const std::array<float, 4> expected{ 123.0f, 45.0f, inputs[i].clip[3], inputs[i].clip[3] };
			for (size_t component = 0; component < 4; ++component) {
				if (!std::isfinite(values[i][component]) || std::abs(values[i][component] - expected[component]) > 0.25f) {
					std::printf("FAIL case %zu component %zu: got %f expected %f\n", i, component, values[i][component], expected[component]);
					passed = false;
				}
			}
		}
		context->Unmap(staging.Get(), 0);
		Check(context->Map(clipStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
		const auto* clips = static_cast<const float*>(mapped.pData);
		for (size_t i = 0; i < inputs.size(); ++i) {
			const float z = inputs[i].clip[3];
			const float clipZ = inputs[i].params[2] * z + inputs[i].params[3];
			const float expected = inputs[i].params[2] < 0.0f ? z - clipZ : clipZ;
			if (!std::isfinite(clips[i]) || std::abs(clips[i] - expected) > 0.01f || (clips[i] < 0.0f) != (expected < 0.0f)) {
				std::printf("FAIL case %zu near clip: got %f expected %f\n", i, clips[i], expected);
				passed = false;
			}
		}
		context->Unmap(clipStaging.Get(), 0);
		if (!passed) { return 1; }
		std::printf("PASS %zu GPU terrain reconstruction cases: offset, reverse-Z, projection, near plane, distance, near clip\n", inputs.size());
		return 0;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
