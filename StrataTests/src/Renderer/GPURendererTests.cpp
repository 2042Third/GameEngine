#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Strata/ImGui/ImGuiRenderer.h"

#include <imgui.h>

using namespace Strata;

namespace
{
	nvrhi::TextureHandle CreateRenderTarget(nvrhi::IDevice* device, uint32_t width, uint32_t height, nvrhi::Format format)
	{
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.format = format;
		desc.isRenderTarget = true;
		desc.debugName = "TestRenderTarget";
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		desc.setClearValue(nvrhi::Color(0.0f, 0.0f, 1.0f, 1.0f));
		return device->createTexture(desc);
	}
}

TEST_SUITE("GPU.Renderer")
{
	TEST_CASE("Headless device initializes the renderer")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const GraphicsDeviceInfo& info = gpu.GetDevice().GetInfo();
		CHECK_FALSE(info.AdapterName.empty());
		CHECK_FALSE(gpu.GetDevice().HasSwapchain());
		CHECK(Renderer::GetWhiteTexture() != nullptr);
		CHECK(Renderer::GetShaderLibrary().Get("ImGui.vert") != nullptr);
		CHECK(Renderer::GetShaderLibrary().Get("Missing.vert") == nullptr);

		const GraphicsMemoryBudget budget = gpu.GetDevice().GetMemoryBudget();
		CHECK(budget.Budget > 0);

		// Frame pacing works without a swapchain.
		const uint64_t firstFrame = gpu.GetDevice().GetFrameIndex();
		for (int frame = 0; frame < 4; frame++)
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			gpu.GetDevice().EndFrame();
		}
		CHECK(gpu.GetDevice().GetFrameIndex() == firstFrame + 4);
		CHECK_FALSE(gpu.GetDevice().IsDeviceLost());
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A second graphics device can be created and destroyed")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());

		GraphicsDeviceSpecification specification;
		specification.ApplicationName = "StrataTests.Secondary";
		specification.Headless = true;
		specification.EnableValidation = true;
		specification.MaxFramesInFlight = 3;
		Scope<GraphicsDevice> device = GraphicsDevice::Create(specification);
		REQUIRE(device);
		CHECK(device->GetMaxFramesInFlight() == 3);
		for (int frame = 0; frame < 5; frame++)
		{
			REQUIRE(device->BeginFrame());
			device->EndFrame();
		}
		CHECK(device->GetFrameIndex() == 5);
		CHECK(device->GetErrorCount() == 0);
		device.reset();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Triangle rendering follows the engine's clip-space conventions")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();

		constexpr uint32_t size = 64;
		nvrhi::TextureHandle target = CreateRenderTarget(device, size, size, nvrhi::Format::RGBA8_UNORM);
		nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));

		nvrhi::GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.VS = Renderer::GetShaderLibrary().Get("Debug/ColorTriangle.vert");
		pipelineDesc.PS = Renderer::GetShaderLibrary().Get("Debug/ColorTriangle.frag");
		pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
		pipelineDesc.renderState.rasterState.setCullNone();
		pipelineDesc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
		nvrhi::GraphicsPipelineHandle pipeline = device->createGraphicsPipeline(pipelineDesc, framebuffer->getFramebufferInfo());
		REQUIRE(pipeline);

		nvrhi::CommandListHandle commandList = device->createCommandList();
		commandList->open();
		commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 1.0f, 1.0f));
		nvrhi::GraphicsState state;
		state.pipeline = pipeline;
		state.framebuffer = framebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(size), static_cast<float>(size)));
		commandList->setGraphicsState(state);
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		commandList->close();
		device->executeCommandList(commandList);

		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(target, image));
		REQUIRE(image.Width == size);

		// The triangle spans NDC y in [0, 1]: with +Y up that is the TOP half of the image (row 0 = top).
		const glm::u8vec4 top = Tests::GetPixelRGBA8(image, size / 2, size / 4);
		const glm::u8vec4 bottom = Tests::GetPixelRGBA8(image, size / 2, size * 3 / 4);
		CHECK(top == glm::u8vec4(255, 0, 0, 255));
		CHECK(bottom == glm::u8vec4(0, 0, 255, 255));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("ImGui renders into an offscreen target")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();

		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(128.0f, 128.0f);

		{
			ImGuiRenderer imguiRenderer;
			REQUIRE(imguiRenderer.Init(device, Renderer::GetShaderLibrary()));

			nvrhi::TextureHandle target = CreateRenderTarget(device, 128, 128, nvrhi::Format::RGBA8_UNORM);
			nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
			nvrhi::CommandListHandle commandList = device->createCommandList();

			for (int frame = 0; frame < 2; frame++)
			{
				ImGui::NewFrame();
				ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(64.0f, 64.0f), IM_COL32(0, 255, 0, 255));
				ImGui::GetForegroundDrawList()->AddText(ImVec2(70.0f, 70.0f), IM_COL32(255, 255, 255, 255), "Strata");
				ImGui::Render();

				commandList->open();
				commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
				imguiRenderer.Render(commandList, ImGui::GetDrawData(), framebuffer);
				commandList->close();
				device->executeCommandList(commandList);
			}

			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(target, image));
			CHECK(Tests::GetPixelRGBA8(image, 32, 32) == glm::u8vec4(0, 255, 0, 255));
			CHECK(Tests::GetPixelRGBA8(image, 100, 20) == glm::u8vec4(0, 0, 0, 255));

			// Some text pixels were drawn by the font atlas texture created through the texture protocol.
			bool foundText = false;
			for (uint32_t y = 70; y < 90 && !foundText; y++)
			{
				for (uint32_t x = 70; x < 120 && !foundText; x++)
					foundText = Tests::GetPixelRGBA8(image, x, y).r > 128;
			}
			CHECK(foundText);

			imguiRenderer.Shutdown();
		}
		ImGui::DestroyContext();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("ImGui sampler callbacks switch between linear and nearest filtering")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();

		// A black|white 2x1 texture stretched over 64 pixels: linear filtering blends around the center, nearest
		// filtering keeps a hard edge.
		nvrhi::TextureDesc textureDesc;
		textureDesc.width = 2;
		textureDesc.height = 1;
		textureDesc.format = nvrhi::Format::RGBA8_UNORM;
		textureDesc.debugName = "TestCheckerTexture";
		textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		textureDesc.keepInitialState = true;
		nvrhi::TextureHandle texture = device->createTexture(textureDesc);
		REQUIRE(texture);
		const uint8_t pixels[] = { 0, 0, 0, 255, 255, 255, 255, 255 };
		nvrhi::CommandListHandle commandList = device->createCommandList();
		commandList->open();
		commandList->writeTexture(texture, 0, 0, pixels, sizeof(pixels));
		commandList->close();
		device->executeCommandList(commandList);

		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(128.0f, 64.0f);

		{
			ImGuiRenderer imguiRenderer;
			REQUIRE(imguiRenderer.Init(device, Renderer::GetShaderLibrary()));

			nvrhi::TextureHandle target = CreateRenderTarget(device, 128, 64, nvrhi::Format::RGBA8_UNORM);
			nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));

			ImGui::NewFrame();
			const ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
			const ImTextureID textureID = static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture.Get()));
			ImDrawList* drawList = ImGui::GetForegroundDrawList();
			drawList->AddImage(textureID, ImVec2(0.0f, 0.0f), ImVec2(64.0f, 64.0f));
			drawList->AddCallback(platformIO.DrawCallback_SetSamplerNearest, nullptr);
			drawList->AddImage(textureID, ImVec2(64.0f, 0.0f), ImVec2(128.0f, 64.0f));
			drawList->AddCallback(platformIO.DrawCallback_ResetRenderState, nullptr);
			// Odd index count: the 16-bit index upload is not a multiple of 4 bytes.
			drawList->AddTriangleFilled(ImVec2(0.0f, 63.0f), ImVec2(1.0f, 63.0f), ImVec2(0.0f, 62.0f), IM_COL32(255, 0, 0, 255));
			ImGui::Render();

			commandList->open();
			commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
			imguiRenderer.Render(commandList, ImGui::GetDrawData(), framebuffer);
			commandList->close();
			device->executeCommandList(commandList);

			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(target, image));
			const uint8_t linearCenter = Tests::GetPixelRGBA8(image, 32, 16).r;
			CHECK(linearCenter > 64);
			CHECK(linearCenter < 192);
			CHECK(Tests::GetPixelRGBA8(image, 64 + 30, 16).r == 0);
			CHECK(Tests::GetPixelRGBA8(image, 64 + 34, 16).r == 255);
			CHECK(ImGui::GetPlatformIO().Renderer_RenderState == nullptr);

			imguiRenderer.Shutdown();
		}
		ImGui::DestroyContext();
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
