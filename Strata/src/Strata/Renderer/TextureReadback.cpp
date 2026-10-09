#include "stpch.h"
#include "Strata/Renderer/TextureReadback.h"

namespace Strata
{

	Scope<TextureReadback> TextureReadback::Create(nvrhi::ITexture* texture, const TextureReadbackRegion& region, std::string* outError)
	{
		auto fail = [outError](std::string message) -> Scope<TextureReadback>
		{
			if (outError)
				*outError = std::move(message);
			return nullptr;
		};

		if (!Renderer::IsInitialized())
			return fail("no renderer is running");
		if (!texture)
			return fail("no texture to read");

		const nvrhi::TextureDesc& desc = texture->getDesc();
		const nvrhi::FormatInfo& formatInfo = nvrhi::getFormatInfo(desc.format);
		if (formatInfo.blockSize != 1 || formatInfo.bytesPerBlock == 0)
			return fail(fmt::format("the format of '{}' cannot be read back", desc.debugName));
		if (desc.dimension == nvrhi::TextureDimension::Texture3D || region.MipLevel >= desc.mipLevels || region.ArraySlice >= desc.arraySize)
			return fail(fmt::format("'{}' has no 2D subresource at mip {}, slice {}", desc.debugName, region.MipLevel, region.ArraySlice));

		const uint32_t mipWidth = std::max(desc.width >> region.MipLevel, 1u);
		const uint32_t mipHeight = std::max(desc.height >> region.MipLevel, 1u);
		if (region.X >= mipWidth || region.Y >= mipHeight)
			return fail(fmt::format("the region at ({}, {}) is outside '{}' ({}x{})", region.X, region.Y, desc.debugName, mipWidth, mipHeight));
		const uint32_t width = region.Width > 0 ? region.Width : mipWidth - region.X;
		const uint32_t height = region.Height > 0 ? region.Height : mipHeight - region.Y;
		// Compared as differences: X + Width could overflow.
		if (width > mipWidth - region.X || height > mipHeight - region.Y)
			return fail(fmt::format("the region {}x{} at ({}, {}) exceeds '{}' ({}x{})", width, height, region.X, region.Y, desc.debugName, mipWidth, mipHeight));

		nvrhi::IDevice* device = Renderer::GetDevice();
		nvrhi::TextureDesc stagingDesc;
		stagingDesc.width = width;
		stagingDesc.height = height;
		stagingDesc.format = desc.format;
		stagingDesc.debugName = "TextureReadback";
		stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
		stagingDesc.keepInitialState = true;
		Scope<TextureReadback> readback(new TextureReadback());
		readback->m_Staging = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
		readback->m_Query = device->createEventQuery();
		nvrhi::CommandListHandle commandList = device->createCommandList();
		if (!readback->m_Staging || !readback->m_Query || !commandList)
			return fail(fmt::format("failed to create the staging resources for reading '{}'", desc.debugName));

		const nvrhi::TextureSlice source = nvrhi::TextureSlice()
			.setOrigin(region.X, region.Y)
			.setSize(width, height, 1)
			.setMipLevel(region.MipLevel)
			.setArraySlice(region.ArraySlice);
		commandList->open();
		commandList->copyTexture(readback->m_Staging, nvrhi::TextureSlice(), texture, source);
		commandList->close();
		device->executeCommandList(commandList);
		// The query completes with the last submitted command list, which is the copy.
		device->setEventQuery(readback->m_Query, nvrhi::CommandQueue::Graphics);

		readback->m_Width = width;
		readback->m_Height = height;
		readback->m_BytesPerPixel = formatInfo.bytesPerBlock;
		readback->m_Format = desc.format;
		return readback;
	}

	bool TextureReadback::IsReady() const
	{
		return Renderer::IsInitialized() && Renderer::GetDevice()->pollEventQuery(m_Query);
	}

	void TextureReadback::Wait()
	{
		if (Renderer::IsInitialized())
			Renderer::GetDevice()->waitEventQuery(m_Query);
	}

	bool TextureReadback::GetResult(ReadbackImage& outImage) const
	{
		if (!IsReady())
			return false;

		nvrhi::IDevice* device = Renderer::GetDevice();
		size_t rowPitch = 0;
		const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(m_Staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
		if (!mapped)
			return false;

		outImage.Width = m_Width;
		outImage.Height = m_Height;
		outImage.Format = m_Format;
		outImage.BytesPerPixel = m_BytesPerPixel;
		const size_t packedRow = static_cast<size_t>(m_Width) * m_BytesPerPixel;
		outImage.Pixels.resize(packedRow * m_Height);
		for (uint32_t row = 0; row < m_Height; row++)
			std::memcpy(outImage.Pixels.data() + row * packedRow, mapped + row * rowPitch, packedRow);
		device->unmapStagingTexture(m_Staging);
		return true;
	}

}
