// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

namespace ShaderRegistry
{
	struct Bytecode
	{
		const void* data{ nullptr };
		size_t      size{ 0 };

		explicit operator bool() const { return data != nullptr && size != 0; }
	};

	bool InstallEarly();

	bool Install(ID3D11Device* a_device);
	bool InstallContextObservers(ID3D11DeviceContext* a_context);

	Bytecode For(ID3D11VertexShader* a_shader);

	size_t Count();

	size_t Bytes();
}
