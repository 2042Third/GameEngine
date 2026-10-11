#include "Editor/DisplayImage.h"

#include <Strata/Renderer/Renderer.h>

namespace Strata
{

	namespace DisplayImage
	{

		nvrhi::TextureHandle Upload(nvrhi::ICommandList* commandList, uint32_t width, uint32_t height, std::span<const uint8_t> rgba, const char* debugName)
		{
			if (!Renderer::IsInitialized() || !commandList || width == 0 || height == 0 || rgba.size() < static_cast<size_t>(width) * height * 4)
				return nullptr;
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.debugName = debugName;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			nvrhi::TextureHandle texture = Renderer::GetDevice()->createTexture(desc);
			if (!texture)
				return nullptr;
			commandList->writeTexture(texture, 0, 0, rgba.data(), static_cast<size_t>(width) * 4);
			return texture;
		}

	}

}
