// SPDX-License-Identifier: GPL-3.0-only
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>
#include "TerrainDepthCorrection.h"

using Microsoft::WRL::ComPtr;
using Matrix = std::array<std::array<double, 4>, 4>;
using Vector = std::array<double, 4>;

static void Check(HRESULT result)
{
	if (FAILED(result)) { throw std::runtime_error("D3D operation failed: " + std::to_string(result)); }
}

static Matrix Multiply(const Matrix& a, const Matrix& b)
{
	Matrix out{};
	for (int row = 0; row < 4; ++row) {
		for (int column = 0; column < 4; ++column) {
			for (int i = 0; i < 4; ++i) { out[row][column] += a[row][i] * b[i][column]; }
		}
	}
	return out;
}

static Vector Transform(const Matrix& m, const Vector& v)
{
	Vector out{};
	for (int row = 0; row < 4; ++row) {
		for (int i = 0; i < 4; ++i) { out[row] += m[row][i] * v[i]; }
	}
	return out;
}

static Matrix Perspective(bool reverseZ, double jitter)
{
	const double n = 15.0, f = 353800.0;
	const double a = reverseZ ? -n / (f - n) : f / (f - n), b = reverseZ ? n * f / (f - n) : -n * a;
	return { { { 1.0, 0.0, jitter, 0.0 }, { 0.0, 16.0 / 9.0, -jitter, 0.0 }, { 0.0, 0.0, a, b }, { 0.0, 0.0, 1.0, 0.0 } } };
}

static Matrix Orthographic()
{
	const double n = 1.0, f = 20000.0;
	return { { { 2.0 / 4096.0, 0.0, 0.0, 0.0 }, { 0.0, 2.0 / 4096.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0 / (f - n), -n / (f - n) }, { 0.0, 0.0, 0.0, 1.0 } } };
}

// Rows: right, up, forward (z up, yaw from +y); the eye need not be at the camera-relative origin.
static Matrix View(double yaw, double pitch, const std::array<double, 3>& eye)
{
	const std::array<double, 3> f{ std::cos(pitch) * std::sin(yaw), std::cos(pitch) * std::cos(yaw), std::sin(pitch) };
	const std::array<double, 3> r{ std::cos(yaw), -std::sin(yaw), 0.0 };
	const std::array<double, 3> u{ r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
	Matrix m{};
	for (int i = 0; i < 3; ++i) { m[0][i] = r[i]; m[1][i] = u[i]; m[2][i] = f[i]; }
	for (int row = 0; row < 3; ++row) { m[row][3] = -(m[row][0] * eye[0] + m[row][1] * eye[1] + m[row][2] * eye[2]); }
	m[3][3] = 1.0;
	return m;
}

int main()
{
	try {
		struct Case { float viewProj[16]; float projection[16]; float clip[4]; float rise[4]; };
		struct Expected { double position[3]; double nearClip; };
		std::vector<Case> cases;
		std::vector<Expected> expected;
		for (bool perspective : { true, false }) {
			for (bool reverseZ : { false, true }) {
				for (double jitter : { 0.0, 0.0004 }) {
					if (!perspective && (reverseZ || jitter != 0.0)) { continue; }
					const Matrix projection = perspective ? Perspective(reverseZ, jitter) : Orthographic();
					for (double yaw : { 0.0, 2.1 }) {
						for (double pitch : { 0.0, -0.6, -1.35 }) {
							for (const auto& eye : { std::array<double, 3>{ 0.0, 0.0, 0.0 }, std::array<double, 3>{ 12.5, -7.25, 3.0 } }) {
								const Matrix view = View(yaw, pitch, eye);
								const Matrix viewProj = Multiply(projection, view);
								for (double depth : { 7.0, 12.0, 16.0, 30.0, 300.0, 3000.0, 30000.0 }) {
									for (const auto& screen : { std::array<double, 2>{ -0.8, -0.6 }, std::array<double, 2>{ 0.1, 0.05 }, std::array<double, 2>{ 0.7, -0.9 } }) {
										const double vx = screen[0] * (perspective ? depth : 2048.0);
										const double vy = screen[1] * (perspective ? depth * 9.0 / 16.0 : 2048.0);
										Vector point{ eye[0], eye[1], eye[2], 1.0 };
										for (int i = 0; i < 3; ++i) { point[i] += vx * view[0][i] + vy * view[1][i] + depth * view[2][i]; }
										for (double offset : perspective ? std::initializer_list<double>{ 0.0, 5.0, -5.0, 10.0, 37.5 } : std::initializer_list<double>{ 0.0 }) {
											for (double rise : { 0.0, 24.0 }) {
												Case value{};
												for (int i = 0; i < 16; ++i) {
													value.viewProj[i] = static_cast<float>(viewProj[i / 4][i % 4]);
													value.projection[i] = static_cast<float>(projection[i / 4][i % 4]);
												}
												const Vector clip = Transform(viewProj, point);
												for (int i = 0; i < 4; ++i) { value.clip[i] = static_cast<float>(clip[i]); }
												value.clip[2] += static_cast<float>(reverseZ ? -offset : offset);
												value.rise[0] = static_cast<float>(rise);
												const Vector displaced = Transform(viewProj, { point[0], point[1], point[2] + rise, 1.0 });
												cases.push_back(value);
												expected.push_back({ { point[0], point[1], point[2] },
													reverseZ ? displaced[3] - displaced[2] : displaced[2] });
											}
										}
									}
								}
							}
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
struct Case { float4 viewProj[4]; float4 projection[4]; float4 clip; float4 rise; };
StructuredBuffer<Case> cases : register(t0);
RWStructuredBuffer<float4> results : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint count, stride;
    cases.GetDimensions(count, stride);
    if (id.x >= count) return;
    const Case value = cases[id.x];
    const float4x4 viewProj = float4x4(value.viewProj[0], value.viewProj[1], value.viewProj[2], value.viewProj[3]);
    const float4x4 projection = float4x4(value.projection[0], value.projection[1], value.projection[2], value.projection[3]);
    const float3 position = CameraRelativeFromClip(value.clip, viewProj);
    const float4 displaced = value.clip + mul(viewProj, float4(0.0f, 0.0f, value.rise.x, 0.0f));
    results[id.x] = float4(position,
        TerrainNearClipDistance(displaced, position + float3(0.0f, 0.0f, value.rise.x), viewProj, projection));
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
		desc.ByteWidth = static_cast<UINT>(cases.size() * sizeof(Case));
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		desc.StructureByteStride = sizeof(Case);
		D3D11_SUBRESOURCE_DATA initial{ cases.data(), 0, 0 };
		ComPtr<ID3D11Buffer> input, output, staging;
		Check(device->CreateBuffer(&desc, &initial, &input));
		ComPtr<ID3D11ShaderResourceView> srv;
		Check(device->CreateShaderResourceView(input.Get(), nullptr, &srv));
		desc.ByteWidth = static_cast<UINT>(cases.size() * sizeof(float) * 4);
		desc.StructureByteStride = sizeof(float) * 4;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		Check(device->CreateBuffer(&desc, nullptr, &output));
		ComPtr<ID3D11UnorderedAccessView> uav;
		Check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		Check(device->CreateBuffer(&desc, nullptr, &staging));
		context->CSSetShader(shader.Get(), nullptr, 0);
		context->CSSetShaderResources(0, 1, srv.GetAddressOf());
		context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
		context->Dispatch(static_cast<UINT>((cases.size() + 63) / 64), 1, 1);
		context->CopyResource(staging.Get(), output.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
		const auto* values = static_cast<const std::array<float, 4>*>(mapped.pData);
		bool passed = true;
		double worst = 0.0;
		for (size_t i = 0; i < cases.size(); ++i) {
			const auto& want = expected[i];
			const double distance = std::hypot(want.position[0], want.position[1], want.position[2]);
			const double error = std::hypot(values[i][0] - want.position[0], values[i][1] - want.position[1], values[i][2] - want.position[2]);
			worst = std::max(worst, error);
			if (!std::isfinite(error) || error > 1e-5 * distance + 1e-3) {
				std::printf("FAIL case %zu position: error %f at distance %f\n", i, error, distance);
				passed = false;
			}
			const double tolerance = 1e-5 * std::abs(cases[i].clip[3]) + 1e-3;
			if (!std::isfinite(values[i][3]) || std::abs(values[i][3] - want.nearClip) > tolerance ||
				(std::abs(want.nearClip) > tolerance && (values[i][3] < 0.0f) != (want.nearClip < 0.0))) {
				std::printf("FAIL case %zu near clip: got %f expected %f\n", i, values[i][3], want.nearClip);
				passed = false;
			}
		}
		context->Unmap(staging.Get(), 0);
		if (!passed) { return 1; }
		std::printf("PASS %zu GPU terrain reconstruction cases: unknown depth offset, reverse-Z, jitter, orthographic, near clip; max error %.5f\n",
			cases.size(), worst);
		return 0;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
