// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "VRAPI/TerrainDepthOffset.h"

struct ID3D11VertexShader;

namespace OpenShadersTerrain
{
	bool RegisterListener();
	void Acquire();
	bool Required();
	void SetObserversReady(bool ready);
	bool CanTessellate();
	bool Query(ID3D11VertexShader* shader, CSPluginAPI::TerrainDepthOffset& offset);
}
