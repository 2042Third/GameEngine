#include "stpch.h"
#include "Strata/Renderer/Renderer.h"

namespace Strata
{

	namespace
	{

		struct RendererData
		{
			GraphicsDevice* Device = nullptr;
			Scope<ShaderLibrary> Shaders;
			Scope<BindlessTextureTable> BindlessTextures;

			nvrhi::SamplerHandle LinearClamp;
			nvrhi::SamplerHandle LinearWrap;
			nvrhi::SamplerHandle PointClamp;
			nvrhi::SamplerHandle AnisotropicWrap;

			nvrhi::CommandListHandle FrameCommandList;

			nvrhi::TextureHandle WhiteTexture;
			nvrhi::TextureHandle BlackTexture;
			nvrhi::TextureHandle FlatNormalTexture;
			nvrhi::TextureHandle BlackCubeTexture;
		};

		Scope<RendererData> s_Data;

		nvrhi::TextureHandle CreateSolidTexture(nvrhi::ICommandList* commandList, uint32_t rgba, const char* name, bool cube)
		{
			nvrhi::TextureDesc desc;
			desc.width = 1;
			desc.height = 1;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.dimension = cube ? nvrhi::TextureDimension::TextureCube : nvrhi::TextureDimension::Texture2D;
			desc.arraySize = cube ? 6 : 1;
			desc.debugName = name;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;

			nvrhi::TextureHandle texture = s_Data->Device->GetDevice()->createTexture(desc);
			if (!texture)
				return nullptr;
			for (uint32_t slice = 0; slice < desc.arraySize; slice++)
				commandList->writeTexture(texture, slice, 0, &rgba, sizeof(rgba));
			return texture;
		}

		uint32_t GetBytesPerPixel(nvrhi::Format format)
		{
			const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(format);
			return info.blockSize == 1 ? info.bytesPerBlock : 0; // Block-compressed formats are not readable
		}

	}

	bool Renderer::Init(GraphicsDevice& device)
	{
		ST_CORE_VERIFY(!s_Data, "Renderer initialized twice");
		s_Data = CreateScope<RendererData>();
		s_Data->Device = &device;
		nvrhi::IDevice* nvrhiDevice = device.GetDevice();
		s_Data->Shaders = CreateScope<ShaderLibrary>(nvrhiDevice);

		nvrhi::SamplerDesc samplerDesc;
		samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		s_Data->LinearClamp = nvrhiDevice->createSampler(samplerDesc);
		samplerDesc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
		s_Data->LinearWrap = nvrhiDevice->createSampler(samplerDesc);
		samplerDesc.setMaxAnisotropy(16.0f);
		s_Data->AnisotropicWrap = nvrhiDevice->createSampler(samplerDesc);
		nvrhi::SamplerDesc pointDesc;
		pointDesc.setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		s_Data->PointClamp = nvrhiDevice->createSampler(pointDesc);

		nvrhi::CommandListHandle commandList = nvrhiDevice->createCommandList();
		commandList->open();
		s_Data->WhiteTexture = CreateSolidTexture(commandList, 0xFFFFFFFFu, "WhiteTexture", false);
		s_Data->BlackTexture = CreateSolidTexture(commandList, 0xFF000000u, "BlackTexture", false);
		s_Data->FlatNormalTexture = CreateSolidTexture(commandList, 0xFFFF8080u, "FlatNormalTexture", false); // (0.5, 0.5, 1.0)
		s_Data->BlackCubeTexture = CreateSolidTexture(commandList, 0xFF000000u, "BlackCubeTexture", true);
		commandList->close();
		nvrhiDevice->executeCommandList(commandList);

		s_Data->FrameCommandList = nvrhiDevice->createCommandList();
		if (!s_Data->LinearClamp || !s_Data->LinearWrap || !s_Data->AnisotropicWrap || !s_Data->PointClamp || !s_Data->WhiteTexture
			|| !s_Data->BlackTexture || !s_Data->FlatNormalTexture || !s_Data->BlackCubeTexture || !s_Data->FrameCommandList)
		{
			ST_CORE_ERROR("Renderer: failed to create shared GPU resources");
			Shutdown();
			return false;
		}

		constexpr uint32_t maxBindlessTextures = 32768;
		s_Data->BindlessTextures = CreateScope<BindlessTextureTable>(nvrhiDevice, maxBindlessTextures, device.GetMaxFramesInFlight());
		s_Data->BindlessTextures->SetReservedTexture(BindlessTextureTable::c_WhiteSlot, s_Data->WhiteTexture);
		s_Data->BindlessTextures->SetReservedTexture(BindlessTextureTable::c_BlackSlot, s_Data->BlackTexture);
		s_Data->BindlessTextures->SetReservedTexture(BindlessTextureTable::c_FlatNormalSlot, s_Data->FlatNormalTexture);
		return true;
	}

	void Renderer::Shutdown()
	{
		if (!s_Data)
			return;

		s_Data->Device->WaitForIdle();
		s_Data.reset();
	}

	bool Renderer::IsInitialized()
	{
		return s_Data != nullptr;
	}

	GraphicsDevice& Renderer::GetGraphicsDevice()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->Device;
	}

	nvrhi::IDevice* Renderer::GetDevice()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return s_Data->Device->GetDevice();
	}

	ShaderLibrary& Renderer::GetShaderLibrary()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->Shaders;
	}

	BindlessTextureTable& Renderer::GetBindlessTextures()
	{
		ST_CORE_ASSERT(s_Data, "Renderer is not initialized");
		return *s_Data->BindlessTextures;
	}

	void Renderer::BeginFrame()
	{
		if (!s_Data)
			return;

		s_Data->BindlessTextures->BeginFrame(s_Data->Device->GetFrameIndex());
		if (nvrhi::ITexture* backBuffer = s_Data->Device->GetBackBuffer())
		{
			s_Data->FrameCommandList->open();
			s_Data->FrameCommandList->clearTextureFloat(backBuffer, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
			s_Data->FrameCommandList->close();
			s_Data->Device->GetDevice()->executeCommandList(s_Data->FrameCommandList);
		}
	}

	nvrhi::ISampler* Renderer::GetLinearClampSampler() { return s_Data->LinearClamp; }
	nvrhi::ISampler* Renderer::GetLinearWrapSampler() { return s_Data->LinearWrap; }
	nvrhi::ISampler* Renderer::GetPointClampSampler() { return s_Data->PointClamp; }
	nvrhi::ISampler* Renderer::GetAnisotropicWrapSampler() { return s_Data->AnisotropicWrap; }
	nvrhi::ITexture* Renderer::GetWhiteTexture() { return s_Data->WhiteTexture; }
	nvrhi::ITexture* Renderer::GetBlackTexture() { return s_Data->BlackTexture; }
	nvrhi::ITexture* Renderer::GetFlatNormalTexture() { return s_Data->FlatNormalTexture; }
	nvrhi::ITexture* Renderer::GetBlackCubeTexture() { return s_Data->BlackCubeTexture; }

	bool Renderer::ReadTexture(nvrhi::ITexture* texture, ReadbackImage& outImage)
	{
		if (!s_Data || !texture)
			return false;

		const nvrhi::TextureDesc& sourceDesc = texture->getDesc();
		const uint32_t bytesPerPixel = GetBytesPerPixel(sourceDesc.format);
		if (bytesPerPixel == 0)
		{
			ST_CORE_ERROR("Renderer::ReadTexture: format of '{}' cannot be read back", sourceDesc.debugName);
			return false;
		}

		nvrhi::IDevice* device = GetDevice();
		nvrhi::TextureDesc stagingDesc;
		stagingDesc.width = sourceDesc.width;
		stagingDesc.height = sourceDesc.height;
		stagingDesc.format = sourceDesc.format;
		stagingDesc.debugName = "ReadbackStaging";
		stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
		stagingDesc.keepInitialState = true;
		nvrhi::StagingTextureHandle staging = device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
		if (!staging)
			return false;

		nvrhi::CommandListHandle commandList = device->createCommandList();
		commandList->open();
		commandList->copyTexture(staging, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());
		commandList->close();
		device->executeCommandList(commandList);
		device->waitForIdle();

		size_t rowPitch = 0;
		const auto* mapped = static_cast<const uint8_t*>(device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
		if (!mapped)
			return false;

		outImage.Width = sourceDesc.width;
		outImage.Height = sourceDesc.height;
		outImage.Format = sourceDesc.format;
		outImage.BytesPerPixel = bytesPerPixel;
		const size_t packedRow = static_cast<size_t>(sourceDesc.width) * bytesPerPixel;
		outImage.Pixels.resize(packedRow * sourceDesc.height);
		for (uint32_t row = 0; row < sourceDesc.height; row++)
			std::memcpy(outImage.Pixels.data() + row * packedRow, mapped + row * rowPitch, packedRow);

		device->unmapStagingTexture(staging);
		return true;
	}

}
