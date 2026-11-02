// SPDX-License-Identifier: GPL-3.0-only
#include "PCH.h"
#include "OpenShadersTerrain.h"
#include "Settings.h"
#include "VRAPI/CSinterface001.h"

#include <cmath>
#include <cstring>

namespace OpenShadersTerrain
{
	namespace
	{
		CSPluginAPI::ICSInterface001* api{};
		bool required{};
		bool observersReady{};
	}

	void Acquire()
	{
		if (api) { return; }
		const auto* messaging = SKSE::GetMessagingInterface();
		CSPluginAPI::CSMessage request;
		if (messaging && messaging->Dispatch(CSPluginAPI::CSMessage::kMessage_GetInterface,
				&request, sizeof(request), CSPluginAPI::CSPluginName) && request.GetApiFunction) {
			api = static_cast<CSPluginAPI::ICSInterface001*>(
				request.GetApiFunction(CSPluginAPI::CSInterfaceRevision005));
			if (api) { logger::info("Terrain blending: Open Shaders revision 5 API acquired"); }
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* message)
	{
		if (!message || message->type != CSPluginAPI::TerrainDepthSupportMessageType ||
			!message->sender || std::strcmp(message->sender, CSPluginAPI::CSPluginName) != 0 ||
			!message->data || message->dataLen != sizeof(CSPluginAPI::TerrainDepthSupportMessage)) { return; }
		auto& request = *static_cast<CSPluginAPI::TerrainDepthSupportMessage*>(message->data);
		request.supported = 0;
		if (request.revision != CSPluginAPI::CSInterfaceRevision005 || !Settings::terrainBlendingCompatibility || REL::Module::IsVR()) { return; }
		Acquire();
		if (api) {
			required = true;
			request.supported = 1;
			logger::info("Terrain blending: API correction required for this session");
		}
	}

	bool Required() { return required; }
	void SetObserversReady(bool ready) { observersReady = ready; }
	bool CanTessellate() { return !required || observersReady; }

	bool RegisterListener()
	{
		const auto* messaging = SKSE::GetMessagingInterface();
		return messaging && messaging->RegisterListener(nullptr, OnMessage);
	}

	bool Query(ID3D11VertexShader* shader, CSPluginAPI::TerrainDepthOffset& offset)
	{
		offset = {};
		if (!api) { return false; }
		// With revision 5, untagged shaders require zero correction, not fingerprint fallback.
		if (!api->GetTerrainDepthOffset(shader, &offset) ||
			!std::isfinite(offset.standardOffset) || offset.reverseZ > 1) { offset = {}; }
		return true;
	}
}
