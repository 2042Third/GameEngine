#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Process.h"
#include "Strata/ImGui/ImGuiRenderer.h"
#include "TestHelpers.h"

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

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
		CHECK(info.MaxTextureDimension2D >= 4096); // Vulkan's guaranteed minimum
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

	TEST_CASE("A validation layer the loader lists but cannot load leaves the device without it")
	{
		// A manifest of the validation layer naming a library that does not exist, the only layer the loader looks for: the
		// loader lists the layer and fails to load it, as with a manifest whose library lies outside its search path.
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("BrokenValidationLayer");
		REQUIRE(FileSystem::WriteText(directory / "VkLayer_khronos_validation.json", R"({
	"file_format_version": "1.2.0",
	"layer": {
		"name": "VK_LAYER_KHRONOS_validation",
		"type": "GLOBAL",
		"library_path": "./Missing/VkLayer_khronos_validation_missing",
		"api_version": "1.3.0",
		"implementation_version": "1",
		"description": "A validation layer whose library is missing"
	}
})"));

		// The child's environment only (a helper sets it), never this process's, whose device uses the real layer.
		ProcessSpecification specification;
		specification.Executable = Tests::GetTestExecutablePath();
		specification.Arguments = { "--strata-test-helper=run-with-environment", "VK_LAYER_PATH=" + FileSystem::ToUTF8(directory), "VK_ADD_LAYER_PATH=",
			"--", FileSystem::ToUTF8(Tests::GetTestExecutablePath()), "--strata-test-helper=create-graphics-device", "--validation" };
		specification.Output = ProcessOutputMode::CaptureSeparate;
		const Process::RunResult result = Process::Run(specification, std::chrono::seconds(60));
		REQUIRE_MESSAGE(result.Started, result.Error);
		REQUIRE_FALSE(result.TimedOut);
		INFO(result.Output);
		INFO(result.ErrorOutput);
		CHECK(result.ExitCode == 0);
		// The loader's reports of the layer it could not load are no errors of the device.
		CHECK(result.Output.find("Graphics device created (errors: 0)") != std::string::npos);
		const std::string log = result.Output + result.ErrorOutput;
		CHECK(log.find("VK_LAYER_KHRONOS_validation could not be loaded") != std::string::npos);
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

	TEST_CASE("ImGui frames the GPU has not drawn yet keep their own vertices")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();

		ImGui::CreateContext();
		ImGui::GetIO().IniFilename = nullptr;
		{
			ImGuiRenderer imguiRenderer;
			REQUIRE(imguiRenderer.Init(device, Renderer::GetShaderLibrary()));
			nvrhi::CommandListHandle commandList = device->createCommandList();
			const auto drawFrame = [&](nvrhi::IFramebuffer* framebuffer, nvrhi::ITexture* target, const ImVec2& size, uint32_t rects, ImU32 color)
			{
				ImGui::GetIO().DisplaySize = size;
				ImGui::NewFrame();
				for (uint32_t rect = 0; rect < rects; rect++)
					ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), size, color);
				ImGui::Render();
				commandList->open();
				commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
				imguiRenderer.Render(commandList, ImGui::GetDrawData(), framebuffer);
				commandList->close();
				device->executeCommandList(commandList);
			};

			// A frame that keeps the GPU busy (thousands of blended layers over four million pixels), then frames recorded
			// and submitted at once, each in a color and a target of its own: the GPU draws them long after the CPU wrote
			// the next frames' vertices, which must not have replaced theirs.
			nvrhi::TextureHandle busyTarget = CreateRenderTarget(device, 2048, 2048, nvrhi::Format::RGBA8_UNORM);
			nvrhi::FramebufferHandle busyFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(busyTarget));
			drawFrame(busyFramebuffer, busyTarget, ImVec2(2048.0f, 2048.0f), 1500, IM_COL32(255, 255, 255, 1));

			const std::vector<glm::u8vec4> colors = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 0, 255 }, { 0, 255, 255, 255 } };
			std::vector<nvrhi::TextureHandle> targets;
			std::vector<nvrhi::FramebufferHandle> framebuffers;
			for (size_t frame = 0; frame < colors.size(); frame++)
			{
				targets.push_back(CreateRenderTarget(device, 64, 64, nvrhi::Format::RGBA8_UNORM));
				framebuffers.push_back(device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.back())));
				const glm::u8vec4& color = colors[frame];
				// More rectangles each frame: the buffers also grow while earlier frames are in flight.
				drawFrame(framebuffers.back(), targets.back(), ImVec2(64.0f, 64.0f), 1 + static_cast<uint32_t>(frame) * 2000,
					IM_COL32(color.r, color.g, color.b, color.a));
			}

			for (size_t frame = 0; frame < colors.size(); frame++)
			{
				CAPTURE(frame);
				ReadbackImage image;
				REQUIRE(Renderer::ReadTexture(targets[frame], image));
				CHECK(Tests::GetPixelRGBA8(image, 32, 32) == colors[frame]);
			}
			// Frames in flight drew from sets of their own; the bounded number of sets was enough.
			CHECK(imguiRenderer.GetFrameBufferSetCount() >= 2);
			CHECK(imguiRenderer.GetFrameBufferSetCount() <= 8);
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

	TEST_CASE("ImGui texture displays show sRGB, grayscale and two-channel normal map textures as their content")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		nvrhi::CommandListHandle commandList = device->createCommandList();

		// One texel each: sRGB orange, a one-channel gray, and a normal map's X and Y (Z left to reconstruct).
		const auto createTexel = [&](nvrhi::Format format, std::vector<uint8_t> texel)
		{
			nvrhi::TextureDesc desc;
			desc.width = 1;
			desc.height = 1;
			desc.format = format;
			desc.debugName = "TestDisplayTexel";
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			nvrhi::TextureHandle texture = device->createTexture(desc);
			REQUIRE(texture);
			commandList->open();
			commandList->writeTexture(texture, 0, 0, texel.data(), texel.size());
			commandList->close();
			device->executeCommandList(commandList);
			return texture;
		};
		const nvrhi::TextureHandle srgb = createTexel(nvrhi::Format::SRGBA8_UNORM, { 230, 120, 30, 255 });
		const nvrhi::TextureHandle gray = createTexel(nvrhi::Format::R8_UNORM, { 165 });
		const nvrhi::TextureHandle normal = createTexel(nvrhi::Format::RG8_UNORM, { 191, 128 });

		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(128.0f, 32.0f);
		{
			ImGuiRenderer imguiRenderer;
			REQUIRE(imguiRenderer.Init(device, Renderer::GetShaderLibrary()));
			nvrhi::TextureHandle target = CreateRenderTarget(device, 128, 32, nvrhi::Format::RGBA8_UNORM);
			nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));

			ImGui::NewFrame();
			ImDrawList* drawList = ImGui::GetForegroundDrawList();
			const auto image = [drawList](nvrhi::ITexture* texture, float x)
			{
				drawList->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)), ImVec2(x, 0.0f), ImVec2(x + 32.0f, 32.0f));
			};
			image(srgb, 0.0f); // As sampled: the linear values
			AddImGuiTextureDisplay(drawList, GetImGuiTextureDisplay(TextureFormat::RGBA8SRGB, false));
			image(srgb, 32.0f);
			AddImGuiTextureDisplay(drawList, GetImGuiTextureDisplay(TextureFormat::R8, true));
			image(gray, 64.0f);
			AddImGuiTextureDisplay(drawList, GetImGuiTextureDisplay(TextureFormat::RG8, false));
			image(normal, 96.0f);
			ImGui::Render();

			commandList->open();
			commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
			imguiRenderer.Render(commandList, ImGui::GetDrawData(), framebuffer);
			commandList->close();
			device->executeCommandList(commandList);

			ReadbackImage pixels;
			REQUIRE(Renderer::ReadTexture(target, pixels));
			const auto describe = [](const glm::u8vec4& pixel)
			{
				return std::to_string(pixel.r) + ", " + std::to_string(pixel.g) + ", " + std::to_string(pixel.b) + ", " + std::to_string(pixel.a);
			};
			const auto isClose = [](const glm::u8vec4& pixel, const glm::ivec3& expected)
			{
				return glm::all(glm::lessThanEqual(glm::abs(glm::ivec3(pixel) - expected), glm::ivec3(1)));
			};
			const glm::u8vec4 direct = Tests::GetPixelRGBA8(pixels, 16, 16);
			CHECK_MESSAGE(isClose(direct, glm::ivec3(202, 48, 3)), describe(direct));
			const glm::u8vec4 encoded = Tests::GetPixelRGBA8(pixels, 48, 16);
			CHECK_MESSAGE(isClose(encoded, glm::ivec3(230, 120, 30)), describe(encoded));
			const glm::u8vec4 grayscale = Tests::GetPixelRGBA8(pixels, 80, 16);
			CHECK_MESSAGE(isClose(grayscale, glm::ivec3(165, 165, 165)), describe(grayscale));
			// X 0.498, Y 0.004, so Z 0.867: shown as (X, Y, Z) * 0.5 + 0.5.
			const glm::u8vec4 normalMap = Tests::GetPixelRGBA8(pixels, 112, 16);
			CHECK_MESSAGE(isClose(normalMap, glm::ivec3(191, 128, 238)), describe(normalMap));
			CHECK(normalMap.a == 255);

			imguiRenderer.Shutdown();
		}
		ImGui::DestroyContext();
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
