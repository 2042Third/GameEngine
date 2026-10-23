#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorViewport.h"
#include "Editor/ProjectThumbnail.h"
#include "Editor/ViewportRenderer.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Panels/GameViewPanel.h"
#include "Renderer/GPUTestUtils.h"
#include "Renderer/MeshTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/Base64.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/RenderScene.h>
#include <Strata/Renderer/Renderer.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Scene/Components.h>

#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Strata;

namespace
{

	struct DecodedImage
	{
		int Width = 0;
		int Height = 0;
		std::vector<uint8_t> Pixels; // RGBA8

		glm::ivec4 At(int x, int y) const
		{
			const uint8_t* pixel = Pixels.data() + (static_cast<size_t>(y) * Width + x) * 4;
			return glm::ivec4(pixel[0], pixel[1], pixel[2], pixel[3]);
		}
	};

	DecodedImage DecodePNG(const std::vector<uint8_t>& png)
	{
		DecodedImage image;
		int channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &image.Width, &image.Height, &channels, 4);
		REQUIRE(pixels);
		image.Pixels.assign(pixels, pixels + static_cast<size_t>(image.Width) * image.Height * 4);
		stbi_image_free(pixels);
		return image;
	}

	struct LumaStats
	{
		double Mean = 0.0;
		double StandardDeviation = 0.0;
	};

	// Luma of display values (Rec. 709 weights on the sRGB-encoded channels, 0..255).
	double GetLuma(const glm::ivec4& pixel)
	{
		return 0.2126 * pixel.r + 0.7152 * pixel.g + 0.0722 * pixel.b;
	}

	LumaStats MeasureLuma(const DecodedImage& image)
	{
		double sum = 0.0;
		double squares = 0.0;
		for (int y = 0; y < image.Height; y++)
		{
			for (int x = 0; x < image.Width; x++)
			{
				const double luma = GetLuma(image.At(x, y));
				sum += luma;
				squares += luma * luma;
			}
		}
		const double count = static_cast<double>(image.Width) * static_cast<double>(image.Height);
		LumaStats stats;
		stats.Mean = sum / count;
		stats.StandardDeviation = std::sqrt(std::max(squares / count - stats.Mean * stats.Mean, 0.0));
		return stats;
	}

	// Mean luma of the 7x7 pixels around where a world point appears.
	double MeanLumaAround(const DecodedImage& image, const SceneCamera& camera, const glm::vec3& point)
	{
		const glm::vec4 clip = camera.Projection * camera.View * glm::vec4(point, 1.0f);
		REQUIRE(clip.w > 0.0f);
		const glm::vec2 ndc = glm::vec2(clip) / clip.w;
		const int centerX = static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(image.Width));
		const int centerY = static_cast<int>((0.5f - ndc.y * 0.5f) * static_cast<float>(image.Height));
		REQUIRE(centerX >= 3);
		REQUIRE(centerY >= 3);
		REQUIRE(centerX + 3 < image.Width);
		REQUIRE(centerY + 3 < image.Height);
		double sum = 0.0;
		for (int y = centerY - 3; y <= centerY + 3; y++)
		{
			for (int x = centerX - 3; x <= centerX + 3; x++)
				sum += GetLuma(image.At(x, y));
		}
		return sum / 49.0;
	}

	// The active scene rendered by a scene renderer of its own with the default options: what the scene shows without
	// any editor view settings.
	DecodedImage RenderDirectly(EditorContext& context, const SceneCamera& camera, int width, int height)
	{
		SceneRenderer renderer;
		renderer.SetViewportSize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
		REQUIRE(renderer.Render(*context.GetActiveScene(), camera));
		ReadbackImage readback;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), readback));
		REQUIRE(readback.BytesPerPixel == 4);
		DecodedImage image;
		image.Width = static_cast<int>(readback.Width);
		image.Height = static_cast<int>(readback.Height);
		image.Pixels = std::move(readback.Pixels); // Tightly packed RGBA8 rows, like a decoded PNG
		// Captures are encoded opaque (ImageWriter::EncodePNG).
		for (size_t index = 3; index < image.Pixels.size(); index += 4)
			image.Pixels[index] = 255;
		return image;
	}

	struct ViewportGPUHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;
		// When set, RunFrames runs device frames around the editor's frames, as the application does: the GPU's work of each
		// frame is retired before frames in flight run out (a capture that renders on many frames needs it).
		Tests::GPUContext* DeviceFrames = nullptr;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			const EditorCommandResult result = Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		// Runs a command through the runner, one editor frame at a time (the context's update, then the runner's, as in the
		// editor), until it completes.
		EditorCommandResult RunFrames(std::string_view name, const nlohmann::json& parameters, int* outFrames = nullptr)
		{
			// Shared with the completion, which may outlive this call if the command never finishes.
			auto completed = std::make_shared<std::optional<EditorCommandResult>>();
			const bool pending = Runner.Run(Context, Commands, name, parameters, [completed](const EditorCommandResult& result) { *completed = result; });
			int frames = 0;
			for (; !*completed && frames < 20000; frames++)
			{
				if (DeviceFrames)
				{
					REQUIRE(DeviceFrames->GetDevice().BeginFrame());
					Renderer::BeginFrame();
				}
				Context.Update(Timestep(1.0f / 60.0f));
				Runner.Update(Context);
				if (DeviceFrames)
					DeviceFrames->GetDevice().EndFrame();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			REQUIRE(*completed);
			if (outFrames)
				*outFrames = pending ? frames : 0;
			return **completed;
		}

		DecodedImage Capture(const nlohmann::json& parameters, nlohmann::json* outResult = nullptr)
		{
			int frames = 0;
			const EditorCommandResult result = RunFrames("viewport.capture", parameters, &frames);
			INFO(result.Error);
			REQUIRE(result.Success);
			CHECK(frames >= 1); // Rendered on a later frame, never inside the command
			const nlohmann::json& image = result.Value["Image"];
			CHECK(image["MimeType"] == "image/png");
			const std::optional<std::vector<uint8_t>> png = Base64::Decode(image["Data"].get<std::string>());
			REQUIRE(png);
			if (outResult)
				*outResult = result.Value;
			DecodedImage decoded = DecodePNG(*png);
			CHECK(result.Value["width"] == decoded.Width);
			CHECK(result.Value["height"] == decoded.Height);
			return decoded;
		}

		// Captures until no asset is loading any more: assets start loading when a render first needs them and finish in the
		// context's update, as in the editor's frames (a project's materials, e.g. a template's ground).
		DecodedImage CaptureLoaded(const nlohmann::json& parameters, nlohmann::json* outResult = nullptr)
		{
			nlohmann::json result;
			DecodedImage image = Capture(parameters, &result);
			for (int attempt = 0; attempt < 100 && result["pendingAssets"] != 0; attempt++)
			{
				Context.Update(Timestep(1.0f / 60.0f));
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				image = Capture(parameters, &result);
			}
			CHECK(result["pendingAssets"] == 0);
			if (outResult)
				*outResult = result;
			return image;
		}

		void AddScene()
		{
			// Plain output, so colors can be compared: no automatic exposure, tone curve, vignette or anti-aliasing.
			Run("entity.create", { { "name", "PostProcess" }, { "components", { { "PostProcess", { { "Tonemapper", "None" }, { "AutoExposure", false },
				{ "Vignette", 0.0 }, { "Bloom", false }, { "AmbientOcclusion", false }, { "AntiAliasing", false } } } } } });
			Run("entity.create", { { "name", "Sun" }, { "components", { { "DirectionalLight", { { "Intensity", 3.0 } } } } } });
			Run("entity.create", { { "name", "Cube" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } });
			Run("camera.set", { { "position", { 0, 0, 4 } }, { "target", { 0, 0, 0 } } });
		}

		int CountDifferences(const DecodedImage& a, const DecodedImage& b)
		{
			REQUIRE(a.Width == b.Width);
			REQUIRE(a.Height == b.Height);
			int differences = 0;
			for (int y = 0; y < a.Height; y++)
			{
				for (int x = 0; x < a.Width; x++)
					differences += glm::any(glm::greaterThan(glm::abs(a.At(x, y) - b.At(x, y)), glm::ivec4(8))) ? 1 : 0;
			}
			return differences;
		}
	};

}

TEST_SUITE("GPU.Editor.Viewport")
{
	TEST_CASE("viewport.capture renders on a later frame and returns and saves a PNG")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
		const std::filesystem::path path = Tests::CreateTemporaryDirectory("ViewportCapture") / "Shots" / "Capture.png";

		nlohmann::json result;
		const DecodedImage image = harness.Capture({ { "width", 64 }, { "height", 48 }, { "path", FileSystem::ToUTF8(path) } }, &result);
		CHECK(image.Width == 64);
		CHECK(image.Height == 48);
		CHECK(result["camera"] == "editor");
		CHECK(result["overlays"] == true);
		CHECK(result["pendingAssets"] == 0);
		CHECK(result["pendingTextGlyphs"] == 0);
		CHECK_FALSE(result.contains("notice"));
		CHECK(FileSystem::FromUTF8(result["path"].get<std::string>()) == path);

		// The lit cube fills the center; the corners show the dark clear color.
		const glm::ivec4 center = image.At(32, 24);
		const glm::ivec4 corner = image.At(1, 1);
		CHECK(center.r > 100);
		CHECK(corner.r < 40);
		CHECK(center.a == 255);

		// The saved file holds the same image.
		const std::optional<std::vector<uint8_t>> saved = FileSystem::ReadBytes(path);
		REQUIRE(saved);
		CHECK(*Base64::Decode(result["Image"]["Data"].get<std::string>()) == *saved);

		// Captures do not keep their render targets.
		ViewportRenderer* captureRenderer = harness.Context.GetViewport().GetCaptureRenderer();
		REQUIRE(captureRenderer);
		CHECK(captureRenderer->GetSize() == glm::uvec2(0, 0));
		CHECK(captureRenderer->GetOutputTexture() == nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A capture reports what its frame drew, with the GPU's time per stage, and editor.perf the viewport's frame")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.DeviceFrames = &gpu; // The renderer's frames resolve the GPU's times
		harness.AddScene();

		nlohmann::json result;
		harness.CaptureLoaded({ { "width", 64 }, { "height", 48 } }, &result);
		REQUIRE(result.contains("renderStats"));
		const nlohmann::json& stats = result["renderStats"];
		INFO(stats.dump(1));
		for (const char* key : { "gpuDriven", "drawCommands", "draws", "opaqueDraws", "shadowDraws", "instances", "shadowCasters", "triangles", "visibleProxies",
			"occludedProxies", "occludedCasters", "cachedCascades", "culledProxies", "instancesPerLod", "lights", "droppedLights", "materials", "pendingAssets", "extractMs", "sortMs", "proxyUpdates",
			"instanceBytesUploaded", "deferredInstanceBytes", "geometryPool", "gpuMs", "gpuStatsFrame" })
		{
			CAPTURE(key);
			CHECK(stats.contains(key));
		}
		// The cube, drawn in the prepass and the forward pass, and into the sun's shadow cascades.
		CHECK(stats["instances"] == 1);
		CHECK(stats["opaqueDraws"] == 1);
		CHECK(stats["drawCommands"].get<uint32_t>() >= 2);
		CHECK(stats["triangles"] == 12);
		CHECK(stats["lights"] == 1);
		CHECK(stats["instancesPerLod"].size() == SceneRendererStats::c_LODStatCount);
		// The meshes' geometry lives in the renderer's pool.
		REQUIRE(stats["geometryPool"].is_object());
		CHECK(stats["geometryPool"]["usedMB"].get<double>() > 0.0);
		CHECK(stats["geometryPool"]["capacityMB"].get<double>() >= stats["geometryPool"]["usedMB"].get<double>());
		CHECK(stats["geometryPool"]["allocations"].get<uint32_t>() >= 1);
		if (Renderer::GetGPUProfiler().IsEnabled())
		{
			// The captured frame's own GPU time.
			CHECK(stats["gpuStatsFrame"].is_number_unsigned());
			const nlohmann::json& gpuMs = stats["gpuMs"];
			REQUIRE(gpuMs.contains("GPU.Opaque"));
			CHECK(gpuMs["GPU.Opaque"].get<float>() > 0.0f);
			CHECK(gpuMs["GPU.Prepass"].get<float>() > 0.0f);
			CHECK(gpuMs.contains("GPU.Overlays")); // The editor's overlays and the copy
		}

		// editor.perf: the viewport panel's frame, once it rendered one.
		CHECK(harness.Run("editor.perf")["renderStats"].is_null());
		ViewportRenderer* viewport = harness.Context.GetViewport().GetRenderer();
		REQUIRE(viewport);
		const std::optional<ViewportView> view = ResolveViewportView(harness.Context, ViewportCameraSource::Editor, 4.0f / 3.0f);
		REQUIRE(view);
		REQUIRE(viewport->Render(harness.Context, glm::uvec2(64, 48), *view, harness.Context.GetViewport().GetSettings(), true));
		// The GPU-driven path's counts are known once the GPU ran the frame's culling.
		viewport->GetSceneRenderer().ResolveGPUStats(true);
		const nlohmann::json perf = harness.Run("editor.perf");
		REQUIRE(perf["renderStats"].is_object());
		CHECK(perf["renderStats"]["instances"] == 1);
		CHECK(perf.contains("gpu"));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A capture waits until its text is complete")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
		// Screen-space text is the game's UI, which editor views draw only with gameUI on.
		harness.Run("viewport.setSettings", { { "gameUI", true } });
		// More distinct glyphs than a render may rasterize (TextRenderer::c_FrameRasterBudget), new to the capture renderer.
		harness.Run("entity.create", { { "name", "HUD" }, { "components", { { "Text", { { "Text", "ABCDEFGHIJKLMNOPQRSTUVWXYZ\nabcdefghijklmnopqrstuvwxyz\n0123456789" },
			{ "ScreenSpace", true }, { "ScreenAnchor", { 0.0, 0.0 } }, { "FontSize", 20.0 }, { "Alignment", "Left" } } } } } });

		nlohmann::json result;
		int frames = 0;
		const EditorCommandResult first = harness.RunFrames("viewport.capture", { { "width", 320 }, { "height", 96 } }, &frames);
		REQUIRE(first.Success);
		CHECK(first.Value.value("pendingTextGlyphs", -1) == 0);
		CHECK(frames > 2); // It rendered again while glyphs were missing
		const DecodedImage complete = harness.Capture({ { "width", 320 }, { "height", 96 } }, &result);
		CHECK(result["pendingTextGlyphs"] == 0);
		const std::optional<std::vector<uint8_t>> png = Base64::Decode(first.Value["Image"]["Data"].get<std::string>());
		REQUIRE(png);
		CHECK(harness.CountDifferences(DecodePNG(*png), complete) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A capture waits until mesh renderers new to the scene reached the GPU")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.DeviceFrames = &gpu; // Each render is a frame of its own, with a new upload budget
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(Tests::CreateTemporaryDirectory("ViewportCaptureBacklog") / "Game") }, { "name", "Game" } });
		harness.Run("entity.create", { { "name", "PostProcess" }, { "components", { { "PostProcess", { { "Tonemapper", "None" }, { "AutoExposure", false },
			{ "Vignette", 0.0 }, { "Bloom", false }, { "AmbientOcclusion", false }, { "AntiAliasing", false } } } } } });
		harness.Run("entity.create", { { "name", "Sun" }, { "components", { { "DirectionalLight", { { "Intensity", 3.0 } } } } } });
		// A scene that just gained a cube in front of the camera and, behind it, more mesh renderers than its GPU copy takes in
		// two frames, which it takes first (the newest entities are the first the render scene meets).
		harness.Run("entity.create", { { "name", "Cube" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } });
		constexpr uint32_t c_Parts = 4096;
		AssetMetadata metadata;
		metadata.Type = AssetType::Mesh;
		metadata.Name = "Parts";
		const AssetHandle parts = harness.Context.GetAssetManager()->AddMemoryAsset(Tests::CreateCubeOfParts(1.0f, c_Parts, BuiltinAssets::DefaultMaterial), metadata);
		Tests::AddUploadBacklog(*harness.Context.GetActiveScene(), parts, c_Parts, glm::vec3(0.0f, 0.0f, 40.0f));
		harness.Run("camera.set", { { "position", { 0, 0, 4 } }, { "target", { 0, 0, 0 } } });

		nlohmann::json result;
		const DecodedImage image = harness.Capture({ { "width", 64 }, { "height", 48 } }, &result);
		CHECK(result["renderStats"]["deferredInstanceBytes"] == 0);
		CHECK(result["waitedFrames"].get<uint32_t>() >= 2);
		// The lit cube fills the center.
		CHECK(image.At(32, 24).r > 100);
		CHECK(image.At(1, 1).r < 40);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A capture waits until its game UI's text is complete")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
		harness.Run("viewport.setSettings", { { "gameUI", true } });
		// A UI label with more distinct glyphs than a render may rasterize (TextRenderer::c_FrameRasterBudget), new to the
		// capture renderer, which the UI pass shares with the text.
		const nlohmann::json canvas = Tests::RunCommandToCompletion(harness.Context, harness.Commands, "ui.create", { { "type", "canvas" } });
		const nlohmann::json label = Tests::RunCommandToCompletion(harness.Context, harness.Commands, "ui.create",
			{ { "type", "text" }, { "parent", canvas["entity"] }, { "size", { 1800, 400 } } });
		harness.Run("component.set", { { "entity", label["entity"] }, { "component", "UIText" }, { "values",
			{ { "Text", "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789" }, { "FontSize", 40.0 } } } });

		int frames = 0;
		const EditorCommandResult first = harness.RunFrames("viewport.capture", { { "width", 320 }, { "height", 96 } }, &frames);
		REQUIRE(first.Success);
		CHECK(first.Value["ui"]["canvases"] == 1);
		CHECK(first.Value["ui"]["pendingGlyphs"] == 0);
		CHECK(first.Value["waitedFrames"].get<int>() > 0); // It rendered again while glyphs were missing
		nlohmann::json result;
		const DecodedImage complete = harness.Capture({ { "width", 320 }, { "height", 96 } }, &result);
		CHECK(result["waitedFrames"] == 0);
		const std::optional<std::vector<uint8_t>> png = Base64::Decode(first.Value["Image"]["Data"].get<std::string>());
		REQUIRE(png);
		CHECK(harness.CountDifferences(DecodePNG(*png), complete) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Captures follow the viewport's size and camera, and draw overlays on request")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();

		// Without a size: the viewport's, or its aspect ratio for a missing side.
		harness.Context.GetViewport().SetSize(glm::uvec2(80, 40));
		DecodedImage image = harness.Capture(nlohmann::json::object());
		CHECK(image.Width == 80);
		CHECK(image.Height == 40);
		image = harness.Capture({ { "width", 60 } });
		CHECK(image.Height == 30);
		// A viewport larger than the capture limit shrinks to it, keeping its shape.
		harness.Context.GetViewport().SetSize(glm::uvec2(8000, 2000));
		image = harness.Capture(nlohmann::json::object());
		CHECK(image.Width == 4096);
		CHECK(image.Height == 1024);
		harness.Context.GetViewport().SetSize(glm::uvec2(80, 40));

		// The grid and the light's shape are overlays.
		harness.Run("camera.set", { { "position", { 0, 3, 6 } }, { "target", { 0, 0, 0 } } });
		const DecodedImage withOverlays = harness.Capture({ { "width", 64 }, { "height", 64 }, { "overlays", true } });
		const DecodedImage plain = harness.Capture({ { "width", 64 }, { "height", 64 }, { "overlays", false } });
		CHECK(harness.CountDifferences(withOverlays, plain) > 20);
		const DecodedImage again = harness.Capture({ { "width", 64 }, { "height", 64 }, { "overlays", false } });
		CHECK(harness.CountDifferences(again, plain) == 0); // Captures are repeatable

		// The scene camera: required for "scene", used automatically while playing.
		EditorCommandResult failed = harness.RunFrames("viewport.capture", { { "camera", "scene" } });
		CHECK_FALSE(failed.Success);
		CHECK(failed.Error.find("Primary") != std::string::npos);
		harness.Run("entity.create", { { "name", "Camera" }, { "components", { { "Camera", nlohmann::json::object() }, { "Transform", { { "Translation", { 0, 0, 3 } } } } } } });
		nlohmann::json result;
		image = harness.Capture({ { "width", 64 }, { "height", 64 }, { "camera", "scene" } }, &result);
		CHECK(result["camera"] == "scene");
		CHECK(result["overlays"] == false);
		CHECK(image.At(32, 32).r > 100); // The cube is straight ahead
		REQUIRE(harness.Context.Play());
		harness.Capture({ { "width", 64 }, { "height", 64 } }, &result);
		CHECK(result["camera"] == "scene");
		harness.Context.Stop();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Captures are encoded and saved on job threads, not in the frame")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		REQUIRE_FALSE(JobSystem::IsInitialized());

		// One worker, kept busy until the test releases it: the capture can only finish once the worker is free.
		std::atomic<bool> release = false;
		struct JobSystemScope
		{
			std::atomic<bool>& Release;
			explicit JobSystemScope(std::atomic<bool>& release)
				: Release(release)
			{
				JobSystemSpecification specification;
				specification.WorkerThreadCount = 1;
				specification.IOThreadCount = 1;
				JobSystem::Init(specification);
			}
			~JobSystemScope()
			{
				Release = true;
				JobSystem::Shutdown();
			}
		} jobSystem(release);
		JobSystem::Submit([&release]()
		{
			while (!release)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		});

		ViewportGPUHarness harness;
		harness.AddScene();
		const std::filesystem::path path = Tests::CreateTemporaryDirectory("ViewportCaptureJobs") / "Capture.png";
		auto completed = std::make_shared<std::optional<EditorCommandResult>>();
		REQUIRE(harness.Runner.Run(harness.Context, harness.Commands, "viewport.capture", { { "width", 32 }, { "height", 32 }, { "path", FileSystem::ToUTF8(path) } },
			[completed](const EditorCommandResult& result) { *completed = result; }));
		for (int frame = 0; frame < 300; frame++)
		{
			harness.Runner.Update(harness.Context);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		CHECK_FALSE(*completed); // Long after the GPU finished, the encoding still waits for the worker
		CHECK_FALSE(FileSystem::Exists(path));

		release = true;
		for (int frame = 0; frame < 20000 && !*completed; frame++)
		{
			harness.Runner.Update(harness.Context);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE(*completed);
		INFO((*completed)->Error);
		CHECK((*completed)->Success);
		const std::optional<std::vector<uint8_t>> saved = FileSystem::ReadBytes(path);
		REQUIRE(saved);
		CHECK(*Base64::Decode((*completed)->Value["Image"]["Data"].get<std::string>()) == *saved);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Captures save into the project and replace files only on request")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ViewportCaptureProject");
		ViewportGPUHarness harness;
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Game" } });
		harness.AddScene();
		const std::filesystem::path expected = harness.Context.GetProject()->GetProjectDirectory() / "Captures" / "Shot.png";

		nlohmann::json result;
		harness.Capture({ { "width", 32 }, { "height", 32 }, { "path", "Captures/Shot.png" } }, &result);
		CHECK(FileSystem::FromUTF8(result["path"].get<std::string>()) == expected.lexically_normal());
		const std::optional<std::vector<uint8_t>> first = FileSystem::ReadBytes(expected);
		REQUIRE(first);

		// The same path again: refused unless replacing is asked for, and then replaced.
		const EditorCommandResult refused = harness.RunFrames("viewport.capture", { { "width", 32 }, { "height", 32 }, { "path", "Captures/Shot.png" } });
		CHECK_FALSE(refused.Success);
		CHECK(refused.Error.find("overwrite") != std::string::npos);
		CHECK(FileSystem::ReadBytes(expected) == first);
		harness.Capture({ { "width", 48 }, { "height", 32 }, { "path", "Captures/Shot.png" }, { "overwrite", true } }, &result);
		const std::optional<std::vector<uint8_t>> second = FileSystem::ReadBytes(expected);
		REQUIRE(second);
		CHECK(DecodePNG(*second).Width == 48);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Captures and panel renders of changing sizes and cameras keep their render targets until the GPU is done")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		// Nothing to draw from either camera, so frames only clear the entity IDs, which NVRHI's Vulkan backend does not
		// keep alive for the GPU. Captures free their targets right after submitting their frame, and the panel's renderer
		// recreates them at every resize.
		harness.Run("entity.create", { { "name", "Camera" }, { "components", { { "Camera", nlohmann::json::object() } } } });
		EditorContext& context = harness.Context;
		EditorViewport& viewport = context.GetViewport();
		ViewportRenderer* panel = viewport.GetRenderer();
		REQUIRE(panel);
		const glm::uvec2 sizes[] = { { 64, 48 }, { 96, 64 }, { 48, 80 }, { 120, 72 } };
		for (int round = 0; round < 16; round++)
		{
			const glm::uvec2 size = sizes[round % 4];
			const char* camera = round % 2 == 0 ? "editor" : "scene";
			nlohmann::json result;
			harness.Capture({ { "width", size.x }, { "height", size.y }, { "camera", camera }, { "overlays", round % 3 != 0 } }, &result);
			CHECK(result["camera"] == camera);

			const glm::uvec2 panelSize = sizes[(round + 1) % 4];
			const std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Automatic,
				static_cast<float>(panelSize.x) / static_cast<float>(panelSize.y));
			REQUIRE(view);
			REQUIRE(panel->Render(context, panelSize, *view, viewport.GetSettings(), view->EditorOverlays));
			CHECK(panel->GetStats().Instances == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A capture through a camera of its own leaves the editor camera where it was")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
		const nlohmann::json before = harness.Run("camera.get");
		const nlohmann::json size = { { "width", 64 }, { "height", 64 } };
		const DecodedImage editorView = harness.Capture(size);

		nlohmann::json own = size;
		own["position"] = { 3.0, 2.0, 3.0 };
		own["target"] = { 0.0, 0.0, 0.0 };
		own["fov"] = 40.0;
		const auto closeTo = [](const nlohmann::json& value, const glm::vec3& expected)
		{
			return std::abs(value[0].get<float>() - expected.x) < 1.0e-3f && std::abs(value[1].get<float>() - expected.y) < 1.0e-3f
				&& std::abs(value[2].get<float>() - expected.z) < 1.0e-3f;
		};
		const auto vec3 = [](const nlohmann::json& value) { return glm::vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>()); };
		nlohmann::json result;
		const DecodedImage custom = harness.Capture(own, &result);
		CHECK(result["camera"] == "custom");
		CHECK(closeTo(result["view"]["position"], glm::vec3(3.0f, 2.0f, 3.0f)));
		CHECK(closeTo(result["view"]["forward"], glm::normalize(glm::vec3(-3.0f, -2.0f, -3.0f))));
		CHECK(result["view"]["fov"].get<double>() == doctest::Approx(40.0));
		CHECK(harness.Run("camera.get") == before);
		CHECK(harness.CountDifferences(custom, editorView) > 100); // Another view of the cube

		// The same view through the editor camera itself gives the same picture.
		const nlohmann::json moved = harness.Run("camera.set", { { "position", { 3.0, 2.0, 3.0 } }, { "target", { 0.0, 0.0, 0.0 } }, { "fov", 40.0 } });
		CHECK(harness.CountDifferences(harness.Capture(size), custom) == 0);

		// Position alone moves the view, keeping the camera's direction and field of view.
		own = size;
		own["position"] = { 4.0, 2.0, 4.0 };
		harness.Capture(own, &result);
		CHECK(closeTo(result["view"]["position"], glm::vec3(4.0f, 2.0f, 4.0f)));
		CHECK(closeTo(result["view"]["forward"], vec3(moved["forward"])));
		CHECK(result["view"]["fov"].get<double>() == doctest::Approx(40.0));
		CHECK(harness.Run("camera.get") == moved);
		own["camera"] = "scene";

		// Refused: a camera of its own with the scene's camera, and a view from the point it looks at.
		CHECK(harness.Commands.Execute(harness.Context, "viewport.capture", own).ErrorKind == EditorCommandError::InvalidParameters);
		nlohmann::json invalid = size;
		invalid["position"] = { 1.0, 1.0, 1.0 };
		invalid["target"] = { 1.0, 1.0, 1.0 };
		CHECK(harness.Commands.Execute(harness.Context, "viewport.capture", invalid).ErrorKind == EditorCommandError::InvalidParameters);
		invalid = size;
		invalid["fov"] = 0.0;
		CHECK(harness.Commands.Execute(harness.Context, "viewport.capture", invalid).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The first capture after play.start shows the scene's materials")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		const std::filesystem::path project = Tests::CopyFeatureProject(Tests::CreateTemporaryDirectory("CaptureAfterPlay") / "Project");
		// The project's start scene opens in the background: the command answers once it is in.
		const EditorCommandResult opened = harness.RunFrames("project.open", { { "path", FileSystem::ToUTF8(project) } });
		REQUIRE_MESSAGE(opened.Success, opened.Error);
		// The scene's scripts are not what is captured: it plays without building them first.
		harness.Run("play.start", { { "build", false } });
		// Paused, so that the scene stays as it was while the frames go on (physics would move the ball and the boxes).
		harness.Run("play.pause", { { "paused", true } });

		// The scene's load brought its textures in at their mip tails, and nothing has drawn them yet: the capture renders
		// again until its view is complete, with the levels it wants of them.
		const nlohmann::json size = { { "width", 320 }, { "height", 180 }, { "camera", "scene" } };
		nlohmann::json first;
		const DecodedImage image = harness.Capture(size, &first);
		CHECK(first["pendingAssets"] == 0);
		CHECK(first["texturesBelowWanted"] == 0);
		CHECK(first["waitedFrames"].get<int>() > 0);

		// It shows what the scene shows once everything has long arrived.
		for (int frame = 0; frame < 60; frame++)
			harness.Context.Update(Timestep(1.0f / 60.0f));
		nlohmann::json later;
		const DecodedImage settled = harness.Capture(size, &later);
		CHECK(later["pendingAssets"] == 0);
		CHECK(harness.CountDifferences(image, settled) == 0);

		// Without waiting, a capture takes what the frame has: it renders again only for the scene's temporal anti-aliasing, the
		// jitter's phases.
		harness.Run("play.stop");
		harness.Run("play.start", { { "build", false } });
		nlohmann::json immediate = size;
		immediate["waitForAssets"] = false;
		nlohmann::json at;
		harness.Capture(immediate, &at);
		CHECK(at["temporalFrames"] == c_TemporalSettleFrames);
		CHECK(at["waitedFrames"] == c_TemporalSettleFrames - 1);
		harness.Run("play.stop");
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Captures of an animator that animates only while seen show its pose of the moment, paused and playing")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.DeviceFrames = &gpu;
		const std::filesystem::path project = Tests::CopyFeatureProject(Tests::CreateTemporaryDirectory("CaptureWhenVisible") / "Project");
		const EditorCommandResult opened = harness.RunFrames("project.open", { { "path", FileSystem::ToUTF8(project) } });
		REQUIRE_MESSAGE(opened.Success, opened.Error);
		const nlohmann::json size = { { "width", 160 }, { "height", 120 }, { "camera", "scene" }, { "overlays", false } };

		// The feature project's arm swinging in front of the template's camera, once animating always and once only while
		// seen. Nothing but the captures draws it (no viewport), so the second skips its poses while the game runs.
		std::vector<DecodedImage> pausedImages;
		std::vector<double> pausedTimes;
		for (const char* mode : { "AlwaysAnimate", "WhenVisible" })
		{
			CAPTURE(mode);
			REQUIRE(harness.RunFrames("scene.new", { { "name", "Capture" }, { "template", "basic3d" }, { "force", true } }).Success);
			const EditorCommandResult arm = harness.RunFrames("prefab.instantiate", { { "prefab", "Models/Arm.gltf" },
				{ "components", { { "Transform", { { "Translation", { 0.0, 0.5, 2.0 } }, { "Scale", { 1.5, 1.5, 1.5 } } } } } } });
			REQUIRE_MESSAGE(arm.Success, arm.Error);
			const std::string id = arm.Value["id"].get<std::string>();
			harness.Run("component.set", { { "entity", id }, { "component", "Animator" },
				{ "values", { { "CullingMode", mode }, { "Loop", true }, { "PlayOnStart", true }, { "Clip", "B24808E5B27B99CD" } } } }); // Swing
			harness.Run("play.start", { { "build", false } });
			REQUIRE(harness.RunFrames("editor.wait", { { "frames", 40 } }).Success);
			harness.Run("play.pause", { { "paused", true } });

			// Paused: the capture's first render draws the pose the animator had when it was last seen, and the paused frame
			// after it poses the animator at its time; the capture shows that pose.
			nlohmann::json result;
			pausedImages.push_back(harness.Capture(size, &result));
			CHECK(result["stalePoses"] == 0);
			if (std::string_view(mode) == "WhenVisible")
				CHECK(result["waitedFrames"].get<int>() >= 1);
			const EditorCommandResult info = harness.RunFrames("animation.info", { { "entity", id } });
			REQUIRE_MESSAGE(info.Success, info.Error);
			REQUIRE(info.Value["playing"] == true);
			pausedTimes.push_back(info.Value["normalizedTime"].get<double>());

			// Playing: unseen for a while again, and captured while time goes on.
			harness.Run("play.pause", { { "paused", false } });
			REQUIRE(harness.RunFrames("editor.wait", { { "frames", 20 } }).Success);
			harness.Capture(size, &result);
			CHECK(result["stalePoses"] == 0);
			harness.Run("play.stop");
		}
		// At the same time of the swing, both show the same pose: the arm covers the same pixels. A few pixels of the shadows
		// may differ, as the meshes of an animator that may skip poses are also bounded by where its clips take them, which
		// the shadow cascades are fitted to; an older pose moves the arm across hundreds of pixels.
		REQUIRE(pausedTimes.size() == 2);
		CHECK(pausedTimes[0] == doctest::Approx(pausedTimes[1]));
		CHECK(pausedTimes[0] > 0.0);
		const int differences = harness.CountDifferences(pausedImages[0], pausedImages[1]);
		CHECK_MESSAGE(differences < 60, differences, " pixels differ");
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A scene the editor opens, or a running game switches to, has its render proxies built before it is shown")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("PreparedScenes");
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Game" } });
		const uint32_t count = RenderScene::c_PrepareChunk * 3 + 1;
		{
			Scene& scene = *harness.Context.GetEditScene();
			for (uint32_t index = 0; index < count; index++)
			{
				Entity cube = scene.CreateEntity("Cube");
				cube.GetComponent<TransformComponent>().Translation = glm::vec3(static_cast<float>(index % 64) * 2.0f, 0.0f, static_cast<float>(index / 64) * 2.0f);
				cube.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
			}
		}
		const std::string cubes = harness.Run("scene.saveAs", { { "path", "Scenes/Cubes.stscene" } })["scene"].get<std::string>();
		// The project's picture renders the edited scene in the frames after a save: done before the scene is replaced.
		for (int frame = 0; frame < 3000 && harness.Context.GetProjectThumbnail().IsPending(); frame++)
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			harness.Context.Update(Timestep(1.0f / 60.0f));
			gpu.GetDevice().EndFrame();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE_FALSE(harness.Context.GetProjectThumbnail().IsPending());
		harness.Run("scene.new");

		// Nothing renders in these frames (no viewport panel draws): what the scene brings, its load built.
		const EditorCommandResult opened = harness.RunFrames("scene.open", { { "scene", cubes } });
		REQUIRE_MESSAGE(opened.Success, opened.Error);
		const size_t entities = opened.Value["entities"].get<size_t>();
		const RenderScene* edited = RenderScene::Find(*harness.Context.GetEditScene());
		REQUIRE(edited);
		CHECK(edited->GetPendingCount() == 0);
		CHECK(edited->GetStats().Proxies == count);

		// A switch the running game asks for, the same way.
		harness.Run("scene.new");
		harness.Run("play.start", { { "build", false } });
		harness.Context.GetActiveScene()->RequestSceneLoad(UUIDFromJson(nlohmann::json(cubes)).value());
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (harness.Context.GetActiveScene()->GetEntityCount() != entities)
		{
			REQUIRE(std::chrono::steady_clock::now() < deadline);
			harness.Context.Update(Timestep(1.0f / 60.0f));
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const RenderScene* running = RenderScene::Find(*harness.Context.GetActiveScene());
		REQUIRE(running);
		CHECK(running->GetPendingCount() == 0);
		CHECK(running->GetStats().Proxies == count);
		harness.Run("play.stop");
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Playing without a camera captures the editor camera with a notice")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
		REQUIRE(harness.Context.Play());
		nlohmann::json result;
		harness.Capture({ { "width", 32 }, { "height", 32 } }, &result);
		CHECK(result["camera"] == "editor");
		CHECK_FALSE(result["notice"].get<std::string>().empty());
		harness.Context.Stop();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The selection outline follows the selection and the hierarchy")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
		EditorContext& context = harness.Context;
		ViewportRenderer* renderer = context.GetViewport().GetRenderer();
		REQUIRE(renderer);
		auto outlined = [&]()
		{
			const std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Automatic, 1.0f);
			REQUIRE(view);
			REQUIRE(renderer->Render(context, glm::uvec2(64, 64), *view, context.GetViewport().GetSettings(), view->EditorOverlays));
			return renderer->GetStats().OutlinedEntities;
		};
		Entity cube = context.GetActiveScene()->FindEntityByName("Cube");
		REQUIRE(cube);
		const nlohmann::json parent = UUIDToJson(cube.GetUUID());
		harness.Run("entity.create", { { "name", "First" }, { "parent", parent } });

		// Selecting the cube outlines its descendants too; the list is kept while nothing changes.
		context.Select(cube.GetUUID());
		CHECK(outlined() == 2);
		CHECK(outlined() == 2);
		// A child added or deleted later is outlined or not at once.
		const nlohmann::json created = harness.Run("entity.create", { { "name", "Second" }, { "parent", parent } });
		CHECK(outlined() == 3);
		harness.Run("entity.delete", { { "entities", nlohmann::json::array({ created["id"] }) } });
		CHECK(outlined() == 2);
		context.ClearSelection();
		CHECK(outlined() == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Clicks in the viewport pick entities without blocking")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
		EditorContext& context = harness.Context;
		EditorViewport& viewport = context.GetViewport();
		ViewportRenderer* renderer = viewport.GetRenderer();
		REQUIRE(renderer);
		const glm::uvec2 size(64, 64);
		CHECK_FALSE(viewport.RequestPick(glm::uvec2(32, 32), size, ViewportPickMode::Replace)); // Nothing rendered yet

		auto render = [&]()
		{
			const std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Automatic, 1.0f);
			REQUIRE(view);
			REQUIRE(renderer->Render(context, size, *view, viewport.GetSettings(), view->EditorOverlays));
			CHECK(renderer->GetSize() == size);
			CHECK(renderer->GetOutputTexture() != nullptr);
		};
		auto finishPick = [&]()
		{
			for (int frame = 0; frame < 5000 && viewport.IsPickPending(); frame++)
			{
				viewport.UpdatePicking(context);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			CHECK_FALSE(viewport.IsPickPending());
		};
		render();
		Entity cube = context.GetActiveScene()->FindEntityByName("Cube");
		REQUIRE(cube);

		// While the panel is resized, the last frame has another size than the image clicked on: no pick.
		CHECK_FALSE(viewport.RequestPick(glm::uvec2(32, 32), glm::uvec2(65, 64), ViewportPickMode::Replace));
		CHECK_FALSE(viewport.IsPickPending());

		REQUIRE(viewport.RequestPick(glm::uvec2(32, 32), size, ViewportPickMode::Replace));
		CHECK(viewport.IsPickPending());
		finishPick();
		CHECK(context.GetSelection() == std::vector<UUID> { cube.GetUUID() });

		// The selection outline is drawn from now on; empty space clears the selection.
		render();
		CHECK(renderer->GetStats().OutlinedEntities == 1);
		CHECK_FALSE(viewport.RequestPick(size, size, ViewportPickMode::Replace)); // Outside the image
		REQUIRE(viewport.RequestPick(glm::uvec2(1, 1), size, ViewportPickMode::Replace));
		finishPick();
		CHECK(context.GetSelection().empty());

		// A pick that finishes after play mode started belongs to the edited scene's image: it is dropped.
		REQUIRE(viewport.RequestPick(glm::uvec2(32, 32), size, ViewportPickMode::Replace));
		REQUIRE(context.Play());
		finishPick();
		CHECK(context.GetSelection().empty());
		context.Stop();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("While the game plays, the Scene view's editor camera picks the running scene's entities")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
		EditorContext& context = harness.Context;
		EditorViewport& viewport = context.GetViewport();
		ViewportRenderer* renderer = viewport.GetRenderer();
		REQUIRE(renderer);
		const glm::uvec2 size(64, 64);
		const UUID cube = context.GetActiveScene()->FindEntityByName("Cube").GetUUID();
		REQUIRE(context.Play());
		const Ref<Scene> running = context.GetActiveScene();
		REQUIRE(running != context.GetEditScene());

		// The Scene view asks for the editor camera by name: its view of the running scene, with the editor's overlays.
		const std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Editor, 1.0f);
		REQUIRE(view);
		CHECK_FALSE(view->FromScene);
		CHECK(view->EditorView);
		REQUIRE(renderer->Render(context, size, *view, viewport.GetSettings(), view->EditorOverlays));
		CHECK(renderer->GetRenderedScene().lock() == running);
		REQUIRE(viewport.RequestPick(glm::uvec2(32, 32), size, ViewportPickMode::Replace));
		for (int frame = 0; frame < 5000 && viewport.IsPickPending(); frame++)
		{
			viewport.UpdatePicking(context);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		CHECK_FALSE(viewport.IsPickPending());
		CHECK(context.GetSelection() == std::vector<UUID> { cube });
		CHECK(context.GetPrimarySelection().GetScene() == running.get());
		context.Stop();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The Game view renders through a renderer of its own at its resolution, and captures of the scene camera take it")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
		harness.Run("entity.create", { { "name", "Camera" }, { "components", { { "Camera", { { "Primary", true } } },
			{ "Transform", { { "Translation", { 0, 0, 5 } } } } } } });
		EditorContext& context = harness.Context;
		EditorViewport& viewport = context.GetViewport();
		CHECK_FALSE(viewport.HasGameRenderer());

		// A fixed 1280 x 720 Game view renders exactly that, whatever the panel's size.
		harness.Run("viewport.setSettings", { { "gameView", { { "mode", "1280x720" } } } });
		const GameViewFrame frame = ComputeGameViewFrame(viewport.GetSettings().GameView, glm::vec2(500.0f, 400.0f), glm::vec2(1.0f), std::nullopt);
		REQUIRE(frame.RenderSize == glm::uvec2(1280, 720));
		const std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Scene, 1280.0f / 720.0f);
		REQUIRE(view);
		ViewportRenderer* game = viewport.GetGameRenderer();
		REQUIRE(game);
		CHECK(game != viewport.GetRenderer());
		CHECK(game != viewport.GetCaptureRenderer());
		REQUIRE(game->Render(context, frame.RenderSize, *view, viewport.GetSettings(), false));
		CHECK(game->GetSize() == glm::uvec2(1280, 720));
		REQUIRE(game->GetOutputTexture());
		CHECK(game->GetOutputTexture()->getDesc().width == 1280);
		CHECK(game->GetOutputTexture()->getDesc().height == 720);
		viewport.SetGameViewSize(frame.RenderSize);

		// editor.perf reports the Game view's frame beside the viewport's, as viewport.capture and the viewport describe theirs.
		game->GetSceneRenderer().ResolveGPUStats(true); // The GPU-driven path's counts are known once the GPU culled the frame
		nlohmann::json perf = harness.Run("editor.perf");
		CHECK(perf["renderStats"].is_null()); // The viewport panel rendered nothing
		REQUIRE(perf["gameRenderStats"].is_object());
		CHECK(perf["gameRenderStats"] == CommandUtils::DescribeRenderStats(game->GetStats(), game->GetGPUStats()));
		CHECK(perf["gameRenderStats"].contains("gpuMs"));

		// Captures of the scene's camera take the Game view's resolution; the editor camera keeps the viewport's.
		nlohmann::json result;
		DecodedImage image = harness.Capture({ { "camera", "scene" }, { "waitForAssets", false } }, &result);
		CHECK(image.Width == 1280);
		CHECK(image.Height == 720);
		harness.Run("viewport.setSettings", { { "gameView", { { "mode", "4:3" } } } });
		viewport.SetGameViewSize(glm::uvec2(400, 300));
		image = harness.Capture({ { "camera", "scene" }, { "waitForAssets", false } });
		CHECK(image.Width == 400);
		CHECK(image.Height == 300);
		image = harness.Capture({ { "camera", "scene" }, { "width", 200 }, { "waitForAssets", false } });
		CHECK(image.Height == 150);

		// A hidden or closed Game view keeps no render targets: the panel frees them when it is hidden behind another tab or
		// collapsed (OnHidden) and when it is closed (OnDetach), and stops asking for its size.
		REQUIRE(game->GetSize() == glm::uvec2(1280, 720));
		GameViewPanel panel;
		EditorPanelContext panelContext { context, harness.Commands, harness.Runner };
		panel.OnHidden(panelContext);
		CHECK(game->GetSize() == glm::uvec2(0));
		CHECK(viewport.GetGameViewSize() == glm::uvec2(0));
		CHECK(harness.Run("editor.perf")["gameRenderStats"].is_null());
		REQUIRE(game->Render(context, frame.RenderSize, *view, viewport.GetSettings(), false));
		REQUIRE(game->GetSize() == glm::uvec2(1280, 720));
		panel.OnDetach(panelContext);
		CHECK(game->GetSize() == glm::uvec2(0));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The Game view shows a scene without a camera that has UI as the game does: its UI over black")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		EditorContext& context = harness.Context;
		EditorViewport& viewport = context.GetViewport();
		ViewportRenderer* game = viewport.GetGameRenderer();
		REQUIRE(game);
		const glm::uvec2 size(320, 180);
		const float aspectRatio = static_cast<float>(size.x) / static_cast<float>(size.y);

		// Neither a camera nor UI: nothing to show (the panel shows the missing-camera message, like the game).
		std::string error;
		CHECK_FALSE(ResolveGameView(context, *game, aspectRatio, &error));
		CHECK(error.find("Primary") != std::string::npos);

		// A menu without a camera: a red image over the canvas's left half.
		const nlohmann::json canvas = harness.Run("ui.create", { { "type", "canvas" } });
		const nlohmann::json image = harness.Run("ui.create", { { "type", "image" }, { "parent", canvas["entity"] } });
		harness.Run("component.set", { { "entity", image["entity"] }, { "component", "UIRect" }, { "values", { { "AnchorMin", { 0, 0 } },
			{ "AnchorMax", { 0.5, 1 } }, { "Pivot", { 0, 0 } }, { "Position", { 0, 0 } }, { "Size", { 0, 0 } } } } });
		harness.Run("component.set", { { "entity", image["entity"] }, { "component", "UIImage" }, { "values", { { "Color", { 1, 0, 0, 1 } } } } });
		const std::optional<ViewportView> view = ResolveGameView(context, *game, aspectRatio);
		REQUIRE(view);
		CHECK(view->OverlayOnly);
		REQUIRE(game->Render(context, size, *view, viewport.GetSettings(), false));
		ReadbackImage readback;
		REQUIRE(Renderer::ReadTexture(game->GetOutputTexture(), readback));
		REQUIRE(readback.BytesPerPixel == 4);
		REQUIRE(readback.Width == size.x);
		const auto pixel = [&readback](uint32_t x, uint32_t y)
		{
			const uint8_t* bytes = readback.Pixels.data() + (static_cast<size_t>(y) * readback.Width + x) * 4;
			return glm::ivec3(bytes[0], bytes[1], bytes[2]);
		};
		const glm::ivec3 left = pixel(size.x / 4, size.y / 2);
		const glm::ivec3 right = pixel(size.x * 3 / 4, size.y / 2);
		INFO("left ", left.r, " ", left.g, " ", left.b, ", right ", right.r, " ", right.g, " ", right.b);
		CHECK(left.r > 200);
		CHECK(left.g < 30);
		CHECK(left.b < 30);
		CHECK(right.r < 10);
		CHECK(right.g < 10);
		CHECK(right.b < 10);

		// With a camera the scene shows through it again.
		harness.Run("entity.create", { { "name", "Camera" }, { "components", { { "Camera", { { "Primary", true } } } } } });
		const std::optional<ViewportView> camera = ResolveGameView(context, *game, aspectRatio);
		REQUIRE(camera);
		CHECK_FALSE(camera->OverlayOnly);
		CHECK(camera->FromScene);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A new project made from the basic3d template captures lit")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("LitTemplate") / "Lit";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Lit" }, { "template", "basic3d" } });

		// The default capture: the viewport's size (1280x720 while it is hidden), the editor camera and its overlays.
		nlohmann::json result;
		const DecodedImage image = harness.CaptureLoaded(nlohmann::json::object(), &result);
		CHECK(result["previewLighting"] == false); // The template's own sun and sky light it
		const LumaStats luma = MeasureLuma(image);
		MESSAGE("basic3d capture: mean luma ", luma.Mean, ", standard deviation ", luma.StandardDeviation);
		CHECK(luma.Mean >= 60.0);
		CHECK(luma.StandardDeviation >= 15.0);
		// The game's view, through the template's camera, is lit the same way.
		const DecodedImage game = harness.Capture({ { "camera", "scene" } }, &result);
		CHECK(MeasureLuma(game).Mean >= 60.0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The first cube in a basic3d project shows its shape from the default view")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("CubeTemplate") / "Cube";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Cube" }, { "template", "basic3d" } });
		// A unit cube resting on the ground, with the default (white) material.
		harness.Run("entity.create", { { "name", "Cube" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } },
			{ "Transform", { { "Translation", { 0, 0.5, 0 } } } } } } });

		// The view the project opens with (the game's camera at (0, 2, 6)): the top and the front of the cube show.
		const int width = 1280;
		const int height = 720;
		nlohmann::json result;
		const DecodedImage image = harness.CaptureLoaded({ { "width", width }, { "height", height }, { "overlays", false } }, &result);
		CHECK(result["previewLighting"] == false);
		const SceneCamera camera = harness.Context.GetViewport().GetCamera().GetSceneCamera(static_cast<float>(width) / static_cast<float>(height));
		const double top = MeanLumaAround(image, camera, glm::vec3(0.0f, 1.0f, 0.0f));
		const double front = MeanLumaAround(image, camera, glm::vec3(0.0f, 0.5f, 0.5f));
		// The ground seen just above the cube's top edge (from this camera, far behind the cube), and beside it.
		const double groundBehind = MeanLumaAround(image, camera, glm::vec3(0.0f, 0.0f, -9.0f));
		const double groundBeside = MeanLumaAround(image, camera, glm::vec3(1.5f, 0.0f, 0.5f));
		MESSAGE("basic3d cube luma: top ", top, ", front ", front, ", ground behind ", groundBehind, ", ground beside ", groundBeside);
		CHECK(std::abs(top - front) >= 10.0);
		// The silhouette reads: against the ground behind its top and beside its front.
		CHECK(std::abs(top - groundBehind) >= 10.0);
		CHECK(std::abs(front - groundBeside) >= 10.0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Preview lighting shows the shape of a cube in a scene without lights")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.Run("entity.create", { { "name", "Cube" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } });
		const int width = 320;
		const int height = 180;
		const nlohmann::json size = { { "width", width }, { "height", height }, { "overlays", false } };

		// The default editor camera looks at the origin from the +X+Z side and above: the top, +X and +Z faces show.
		nlohmann::json result;
		const DecodedImage lit = harness.Capture(size, &result);
		CHECK(result["previewLighting"] == true);
		const SceneCamera camera = harness.Context.GetViewport().GetCamera().GetSceneCamera(static_cast<float>(width) / static_cast<float>(height));
		const double top = MeanLumaAround(lit, camera, glm::vec3(0.0f, 0.5f, 0.0f));
		const double right = MeanLumaAround(lit, camera, glm::vec3(0.5f, 0.0f, 0.0f));
		const double front = MeanLumaAround(lit, camera, glm::vec3(0.0f, 0.0f, 0.5f));
		MESSAGE("face luma: top ", top, ", +X ", right, ", +Z ", front);
		CHECK(std::abs(top - right) >= 10.0);
		CHECK(std::abs(right - front) >= 10.0);
		CHECK(std::abs(top - front) >= 10.0);

		// Without preview lighting the image is what the scene renders by itself.
		const DecodedImage reference = RenderDirectly(harness.Context, camera, width, height);
		harness.Run("viewport.setSettings", { { "previewLighting", false } });
		const DecodedImage plain = harness.Capture(size, &result);
		CHECK(result["previewLighting"] == false);
		CHECK(harness.CountDifferences(plain, reference) == 0);
		CHECK(std::abs(MeanLumaAround(plain, camera, glm::vec3(0.0f, 0.5f, 0.0f)) - MeanLumaAround(plain, camera, glm::vec3(0.5f, 0.0f, 0.0f))) < 10.0);

		// The scene's camera always shows the scene by itself.
		harness.Run("viewport.setSettings", { { "previewLighting", true } });
		harness.Run("entity.create", { { "name", "Camera" }, { "components", { { "Camera", nlohmann::json::object() },
			{ "Transform", { { "Translation", { 2, 2, 3 } }, { "Rotation", { -30, 35, 0 } } } } } } });
		const DecodedImage scene = harness.Capture({ { "width", width }, { "height", height }, { "camera", "scene" } }, &result);
		CHECK(result["previewLighting"] == false);
		Entity cameraEntity = harness.Context.GetEditScene()->FindEntityByName("Camera");
		REQUIRE(cameraEntity);
		const SceneCamera sceneCamera = SceneCamera::FromEntity(*harness.Context.GetEditScene(), cameraEntity, static_cast<float>(width) / static_cast<float>(height));
		CHECK(harness.CountDifferences(scene, RenderDirectly(harness.Context, sceneCamera, width, height)) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Editor captures of Tetris leave out its HUD, the game's camera shows it")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		std::string error;
		const std::filesystem::path tetris = Tests::CopySampleProject("Tetris", &error);
		REQUIRE_MESSAGE(!tetris.empty(), error);
		ViewportGPUHarness harness;
		Tests::RunCommandToCompletion(harness.Context, harness.Commands, "project.open", { { "path", FileSystem::ToUTF8(tetris) } });

		// The HUD is a UI canvas: its stats panel with score, level and lines, the controls and the (hidden) message panel.
		nlohmann::json result;
		harness.Capture({ { "width", 320 }, { "height", 180 }, { "camera", "editor" } }, &result);
		CHECK(result["ui"]["canvases"] == 0);
		CHECK(result["ui"]["hiddenCanvases"] == 1);
		harness.Capture({ { "width", 320 }, { "height", 180 }, { "camera", "scene" } }, &result);
		CHECK(result["ui"]["canvases"] == 1);
		CHECK(result["ui"]["elements"].get<int>() >= 6);
		CHECK(result["ui"]["hiddenCanvases"] == 0);
		// gameUI shows it in editor views too.
		harness.Run("viewport.setSettings", { { "gameUI", true } });
		harness.Capture({ { "width", 320 }, { "height", 180 }, { "camera", "editor" } }, &result);
		CHECK(result["ui"]["canvases"] == 1);
		CHECK(result["screenSpaceTexts"] == 0); // Nothing of the HUD is a screen-space Text component any more
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("With TAA the selection outline covers the same pixels in all eight jitter phases")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
		EditorContext& context = harness.Context;
		Entity postProcess = context.GetActiveScene()->FindEntityByName("PostProcess");
		REQUIRE(postProcess);
		postProcess.GetComponent<PostProcessComponent>().AntiAliasing = true;
		postProcess.GetComponent<PostProcessComponent>().AntiAliasingMethod = AntiAliasingMode::TAA;
		Entity cube = context.GetActiveScene()->FindEntityByName("Cube");
		REQUIRE(cube);
		// Turned, so its silhouette crosses pixels at slants the jitter moves.
		cube.GetComponent<TransformComponent>().Rotation = glm::quat(glm::vec3(0.4f, 0.7f, 0.1f));
		cube.MarkModified<TransformComponent>();
		context.Select(cube.GetUUID());
		ViewportSettings settings = context.GetViewport().GetSettings();
		settings.ShowGrid = false;
		settings.ShowSceneGizmos = false;
		settings.SelectionColor = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
		ViewportRenderer* renderer = context.GetViewport().GetRenderer();
		REQUIRE(renderer);
		const std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Automatic, 1.0f);
		REQUIRE(view);

		std::vector<bool> first;
		for (uint32_t frame = 0; frame < 2 * SceneRenderer::c_TemporalJitterPhases; frame++)
		{
			CAPTURE(frame);
			REQUIRE(renderer->Render(context, glm::uvec2(64, 64), *view, settings, true));
			REQUIRE(renderer->GetStats().TemporalAntiAliasing);
			CHECK(renderer->GetStats().OutlinedEntities == 1);
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(renderer->GetOutputTexture(), image));
			std::vector<bool> outline;
			for (size_t index = 0; index < image.Pixels.size(); index += 4)
				outline.push_back(image.Pixels[index] == 255 && image.Pixels[index + 1] == 0 && image.Pixels[index + 2] == 255);
			if (frame == 0)
			{
				first = outline;
				CHECK(std::count(first.begin(), first.end(), true) > 20);
				continue;
			}
			size_t moved = 0;
			for (size_t pixel = 0; pixel < outline.size(); pixel++)
				moved += outline[pixel] != first[pixel] ? 1u : 0u;
			CHECK(moved == 0);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A capture of a TAA scene waits for eight frames and equals the viewport's eighth frame after a reset")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.DeviceFrames = &gpu;
		harness.AddScene();
		EditorContext& context = harness.Context;
		Entity postProcess = context.GetActiveScene()->FindEntityByName("PostProcess");
		REQUIRE(postProcess);
		postProcess.GetComponent<PostProcessComponent>().AntiAliasing = true;
		postProcess.GetComponent<PostProcessComponent>().AntiAliasingMethod = AntiAliasingMode::TAA;
		Entity cube = context.GetActiveScene()->FindEntityByName("Cube");
		REQUIRE(cube);
		cube.GetComponent<TransformComponent>().Rotation = glm::quat(glm::vec3(0.4f, 0.7f, 0.1f));
		cube.MarkModified<TransformComponent>();

		nlohmann::json result;
		const DecodedImage captured = harness.CaptureLoaded({ { "width", 64 }, { "height", 64 }, { "overlays", false } }, &result);
		CHECK(result["temporalFrames"] == c_TemporalSettleFrames);
		CHECK(result["waitedFrames"].get<uint32_t>() >= c_TemporalSettleFrames - 1);

		// The viewport, its history started again, shows the same picture at its eighth frame.
		ViewportRenderer* renderer = context.GetViewport().GetRenderer();
		REQUIRE(renderer);
		const std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Automatic, 1.0f);
		REQUIRE(view);
		renderer->GetSceneRenderer().ResetHistory();
		for (uint32_t frame = 0; frame < c_TemporalSettleFrames; frame++)
			REQUIRE(renderer->Render(context, glm::uvec2(64, 64), *view, context.GetViewport().GetSettings(), false));
		CHECK(renderer->GetStats().TemporalFrames == c_TemporalSettleFrames);
		ReadbackImage viewport;
		REQUIRE(Renderer::ReadTexture(renderer->GetOutputTexture(), viewport));
		REQUIRE(viewport.Pixels.size() == captured.Pixels.size());
		int difference = 0;
		for (size_t index = 0; index < viewport.Pixels.size(); index++)
		{
			if (index % 4 != 3)
				difference = std::max(difference, std::abs(static_cast<int>(viewport.Pixels[index]) - static_cast<int>(captured.Pixels[index])));
		}
		CHECK(difference <= 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A capture of a 100,000 lux scene waits for its pre-exposure to settle")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.DeviceFrames = &gpu;
		harness.Run("entity.create", { { "name", "PostProcess" }, { "components", { { "PostProcess", { { "Tonemapper", "None" }, { "AutoExposure", true },
			{ "AutoExposureMaxEV", 16.0 }, { "Vignette", 0.0 }, { "Bloom", false }, { "AmbientOcclusion", false }, { "AntiAliasing", false } } } } } });
		Scene& scene = *harness.Context.GetActiveScene();
		Entity floor = scene.CreateEntity("Floor");
		floor.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -0.5f, 0.0f);
		floor.GetComponent<TransformComponent>().Scale = glm::vec3(60.0f, 1.0f, 60.0f);
		floor.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
		Entity sun = scene.CreateEntity("Sun");
		sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f)));
		sun.AddComponent<DirectionalLightComponent>().Intensity = 100000.0f;
		harness.Run("camera.set", { { "position", { 0, 6, 8 } }, { "target", { 0, 0, 0 } } });

		// The first frame starts at a pre-exposure of 1; the capture renders again until its metering came back and kept it.
		nlohmann::json result;
		const DecodedImage image = harness.CaptureLoaded({ { "width", 128 }, { "height", 128 }, { "overlays", false } }, &result);
		CHECK(result["preExposureSettled"] == true);
		CHECK(result["waitedFrames"] >= 1);
		ViewportRenderer* capture = harness.Context.GetViewport().GetCaptureRenderer();
		REQUIRE(capture);
		const SceneRendererStats& stats = capture->GetSceneRenderer().GetStats();
		CHECK(stats.PreExposureSettled);
		CHECK(stats.PreExposure <= 1.0f / 4096.0f);
		// The sunlit floor, metered to middle gray.
		const size_t center = (static_cast<size_t>(image.Height / 2) * image.Width + image.Width / 2) * 4;
		CHECK(std::abs(static_cast<int>(image.Pixels[center]) - 118) <= 12);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A capture of a scene with sixteen shadowed point lights shows every light's shadow in its first complete frame")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.DeviceFrames = &gpu;
		harness.Run("entity.create", { { "name", "PostProcess" }, { "components", { { "PostProcess", { { "Tonemapper", "None" }, { "AutoExposure", false },
			{ "Vignette", 0.0 }, { "Bloom", false }, { "AmbientOcclusion", false }, { "AntiAliasing", false } } } } } });
		Scene& scene = *harness.Context.GetActiveScene();
		Entity floor = scene.CreateEntity("Floor");
		floor.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -0.5f, 0.0f);
		floor.GetComponent<TransformComponent>().Scale = glm::vec3(60.0f, 1.0f, 60.0f);
		floor.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
		std::vector<Entity> lamps;
		std::vector<glm::vec3> shadowCenters;
		for (int index = 0; index < 16; index++)
		{
			const glm::vec3 position(static_cast<float>(index % 4) * 5.0f - 7.5f, 3.0f, static_cast<float>(index / 4) * 5.0f - 7.5f);
			Entity lamp = scene.CreateEntity("Lamp");
			lamp.GetComponent<TransformComponent>().Translation = position;
			PointLightComponent& light = lamp.AddComponent<PointLightComponent>();
			light.Intensity = 30.0f;
			light.Range = 4.5f;
			light.CastShadows = true;
			lamps.push_back(lamp);
			// A cube halfway down, offset along X: its shadow twice as far along X on the floor.
			Entity cube = scene.CreateEntity("Cube");
			cube.GetComponent<TransformComponent>().Translation = position + glm::vec3(0.5f, -1.5f, 0.0f);
			cube.GetComponent<TransformComponent>().Scale = glm::vec3(0.8f);
			cube.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
			shadowCenters.push_back(position + glm::vec3(1.0f, -3.0f, 0.0f));
		}
		harness.Run("camera.set", { { "position", { 1, 24, 14 } }, { "target", { 0, 0, 0 } } });
		const nlohmann::json parameters = { { "width", 256 }, { "height", 256 }, { "overlays", false } };

		// The capture renderer draws every view of every light in its first frame, into its smaller atlas: nothing to wait for.
		nlohmann::json result;
		const DecodedImage shadowed = harness.CaptureLoaded(parameters, &result);
		CHECK(result["localShadowViewsPending"] == 0);
		CHECK(result["reflectionProbesPending"] == 0);
		CHECK(result["waitedFrames"] == 0);
		CHECK(result["renderStats"]["localShadowLights"] == 16);
		CHECK(result["renderStats"]["localShadowViewsDrawn"] == 96);
		ViewportRenderer* capture = harness.Context.GetViewport().GetCaptureRenderer();
		REQUIRE(capture);
		CHECK(capture->GetSceneRenderer().GetSpecification().LocalShadowAtlasSize == 2048);
		CHECK_FALSE(capture->GetSceneRenderer().GetSpecification().SpreadLightingUpdates);
		CHECK(capture->GetSceneRenderer().GetLocalShadowAtlas() == nullptr); // Released with the targets after the picture

		for (Entity lamp : lamps)
		{
			lamp.GetComponent<PointLightComponent>().CastShadows = false;
			lamp.MarkModified<PointLightComponent>();
		}
		const DecodedImage lit = harness.CaptureLoaded(parameters, &result);
		const SceneCamera camera = harness.Context.GetViewport().GetCamera().GetSceneCamera(1.0f);
		for (size_t index = 0; index < lamps.size(); index++)
		{
			CAPTURE(index);
			CHECK(MeanLumaAround(shadowed, camera, shadowCenters[index]) < MeanLumaAround(lit, camera, shadowCenters[index]) * 0.7);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The selection outline has the color of the viewport's settings")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
		Entity cube = harness.Context.GetActiveScene()->FindEntityByName("Cube");
		REQUIRE(cube);
		harness.Context.Select(cube.GetUUID());
		// The outline is the only overlay.
		ViewportSettings& settings = harness.Context.GetViewport().GetSettings();
		settings.ShowGrid = false;
		settings.ShowSceneGizmos = false;
		REQUIRE(settings.ShowSelectionOutline);

		// The editor's UI sets the color (its theme's accent); outlines are drawn in exact display colors.
		const auto countPixels = [](const DecodedImage& image, const glm::ivec3& color)
		{
			int count = 0;
			for (int y = 0; y < image.Height; y++)
			{
				for (int x = 0; x < image.Width; x++)
					count += glm::all(glm::lessThanEqual(glm::abs(glm::ivec3(image.At(x, y)) - color), glm::ivec3(24))) ? 1 : 0;
			}
			return count;
		};
		const glm::ivec3 magenta(255, 0, 255);
		const glm::ivec3 cyan(0, 255, 255);
		settings.SelectionColor = glm::vec4(1.0f, 0.0f, 1.0f, 1.0f);
		const DecodedImage first = harness.Capture({ { "width", 64 }, { "height", 64 }, { "overlays", true } });
		settings.SelectionColor = glm::vec4(0.0f, 1.0f, 1.0f, 1.0f);
		const DecodedImage second = harness.Capture({ { "width", 64 }, { "height", 64 }, { "overlays", true } });
		CHECK(countPixels(first, magenta) > 20);
		CHECK(countPixels(first, cyan) == 0);
		CHECK(countPixels(second, cyan) > 20);
		CHECK(countPixels(second, magenta) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Captures keep nothing resident: what only a capture showed can be evicted, and a closed project's assets go")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("CaptureResidency") / "Game";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" }, { "template", "basic3d" } });
		std::weak_ptr<AssetManagerBase> closed = AssetManager::GetActive();
		{
			const Ref<AssetManagerBase> manager = closed.lock();
			REQUIRE(manager);
			const AssetHandle ground = manager->FindAssetByPath("Materials/Ground.stmat");
			REQUIRE(ground.IsValid());

			// Only the capture renderer draws here (no viewport panel): its last frame showed the ground's material.
			harness.CaptureLoaded({ { "width", 64 }, { "height", 64 } });
			REQUIRE(manager->GetAssetState(ground) == AssetState::Ready);
			// The editor's frames go on without captures: what the capture used goes like anything else no longer used.
			const uint32_t grace = manager->GetEvictionGraceFrames();
			for (uint32_t frame = 0; frame <= grace; frame++)
				harness.Context.Update(Timestep(1.0f / 60.0f));
			CHECK(manager->TrimUnused(grace) > 0);
			CHECK(manager->GetAssetState(ground) == AssetState::Unloaded);
			harness.CaptureLoaded({ { "width", 64 }, { "height", 64 } });
			CHECK(manager->GetAssetState(ground) == AssetState::Ready);
		}

		// The project closes after a capture: its asset manager goes with it, whatever the capture renderer drew last.
		harness.Run("project.close");
		CHECK(closed.expired());
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A capture counts a texture at its tail as loaded and reports the levels a texture budget keeps from it")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		// The capture under a budget renders on 120 frames.
		harness.DeviceFrames = &gpu;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("CaptureMipBudget") / "Game";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" }, { "template", "basic3d" } });
		harness.Run("project.close");
		// A 1024x1024 texture of fine stripes, imported as the project opens: a view close to it wants its finer levels.
		constexpr uint32_t size = 1024;
		std::vector<uint8_t> pixels(size * size * 4, 255);
		for (uint32_t y = 0; y < size; y++)
		{
			for (uint32_t x = 0; x < size; x++)
			{
				const uint8_t value = (x / 4) % 2 == 0 ? 40 : 220;
				for (uint32_t channel = 0; channel < 3; channel++)
					pixels[(static_cast<size_t>(y) * size + x) * 4 + channel] = value;
			}
		}
		REQUIRE(FileSystem::WriteBytes(directory / "Assets" / "Textures" / "Stripes.png", Tests::EncodePNG(size, size, pixels)));
		const EditorCommandResult opened = harness.RunFrames("project.open", { { "path", FileSystem::ToUTF8(directory) } });
		REQUIRE_MESSAGE(opened.Success, opened.Error);
		const AssetHandle stripes = harness.Context.GetAssetManager()->FindAssetByPath("Textures/Stripes.png");
		REQUIRE(stripes.IsValid());
		const nlohmann::json material = harness.Run("material.create", { { "path", "Materials/Stripes.stmat" },
			{ "properties", { { "BaseColorMap", UUIDToJson(stripes) }, { "Unlit", true } } } });
		harness.Run("entity.create", { { "name", "Floor" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::PlaneMesh) },
			{ "Material", material["asset"] } } }, { "Transform", { { "Translation", { 0, 0.05, 0 } }, { "Scale", { 4, 1, 4 } } } } } } });
		harness.Run("camera.set", { { "position", { 0, 2, 2 } }, { "target", { 0, 0, 0 } } });
		const nlohmann::json parameters = { { "width", 320 }, { "height", 180 }, { "overlays", false }, { "debugView", "mip" } };
		const auto lacking = [](const nlohmann::json& result)
		{
			const nlohmann::json& perShortfall = result["debugView"]["entitiesPerMipShortfall"];
			return perShortfall[1].get<int>() + perShortfall[2].get<int>() + perShortfall[3].get<int>();
		};

		// Within 10 KB of textures the stripes keep at most their 64x64 level. The texture's tail is resident, so it is no
		// pending asset; it lacks the levels the view wants, so the capture waits its bounded frames for them and reports it.
		harness.Run("asset.setBudget", { { "gpuTexturesMB", 0.01 } });
		nlohmann::json budgeted;
		harness.Capture(parameters, &budgeted);
		CHECK(budgeted["pendingAssets"] == 0);
		CHECK(budgeted["texturesBelowWanted"] == 1);
		CHECK(budgeted["waitedFrames"] == 120);
		CHECK(lacking(budgeted) == 1);

		// With the budget back, the capture waits until the levels arrive and lacks none.
		harness.Run("asset.setBudget", { { "reset", true } });
		nlohmann::json whole;
		harness.Capture(parameters, &whole);
		CHECK(whole["pendingAssets"] == 0);
		CHECK(whole["texturesBelowWanted"] == 0);
		CHECK(whole["waitedFrames"].get<int>() < 120);
		CHECK(lacking(whole) == 0);
		CHECK(whole["debugView"]["entitiesPerMipShortfall"][0].get<int>() >= 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("viewport.capture shows the LOD, streaming, mip and motion debug views")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
		// No overlays: the debug views apply without them.
		const nlohmann::json size = { { "width", 64 }, { "height", 64 }, { "overlays", false } };
		auto withView = [&size](const char* view)
		{
			nlohmann::json parameters = size;
			parameters["debugView"] = view;
			return parameters;
		};

		nlohmann::json result;
		const DecodedImage plain = harness.Capture(withView("none"), &result);
		CHECK(result["debugView"]["view"] == "none");
		CHECK(result["debugView"]["tintedEntities"] == 0);

		// The cube (a built-in mesh with one level of detail) is drawn at level 0: tinted in that level's color.
		ViewportDebugColors& colors = harness.Context.GetViewport().GetSettings().DebugColors;
		colors.LOD[0] = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
		const DecodedImage lod = harness.Capture(withView("lod"), &result);
		CHECK(result["debugView"]["view"] == "lod");
		CHECK(result["debugView"]["tintedEntities"] == 1);
		CHECK(result["debugView"]["entitiesPerLOD"] == nlohmann::json::array({ 1, 0, 0, 0 }));
		CHECK(lod.At(32, 32) == glm::ivec4(0, 255, 0, 255));
		CHECK(lod.At(1, 1) == plain.At(1, 1)); // The background is untouched
		CHECK(harness.CountDifferences(lod, plain) > 50);

		// Nothing is loading: the streaming view tints nothing.
		const DecodedImage streaming = harness.Capture(withView("streaming"), &result);
		CHECK(result["debugView"]["view"] == "streaming");
		CHECK(result["debugView"]["tintedEntities"] == 0);
		CHECK(result["debugView"]["pendingEntities"] == 0);
		CHECK(result["debugView"]["placeholderBoxes"] == 0);
		CHECK(harness.CountDifferences(streaming, plain) == 0);

		// The cube's default material has no textures, so no mip levels are missing: the mip view tints nothing.
		const DecodedImage mip = harness.Capture(withView("mip"), &result);
		CHECK(result["debugView"]["view"] == "mip");
		CHECK(result["debugView"]["tintedEntities"] == 0);
		CHECK(result["debugView"]["entitiesPerMipShortfall"] == nlohmann::json::array({ 0, 0, 0, 0 }));
		CHECK(harness.CountDifferences(mip, plain) == 0);

		// The motion view renders a frame more, against which nothing moved: the image darkened, nothing tinted.
		const DecodedImage motion = harness.Capture(withView("motion"), &result);
		CHECK(result["debugView"]["view"] == "motion");
		CHECK(result["waitedFrames"] == 1);
		CHECK(result["debugView"]["tintedEntities"] == 0);
		CHECK(harness.CountDifferences(motion, plain) > 50);
		for (const glm::ivec2 pixel : { glm::ivec2(1, 1), glm::ivec2(32, 32) })
		{
			const glm::ivec4 expected(glm::vec4(plain.At(pixel.x, pixel.y)) * 0.35f + 0.5f);
			for (int channel = 0; channel < 3; channel++)
				CHECK(std::abs(motion.At(pixel.x, pixel.y)[channel] - expected[channel]) <= 1);
		}

		// Captures default to the viewport's debug view; unknown views are refused.
		harness.Run("viewport.setSettings", { { "debugView", "lod" } });
		harness.Capture(size, &result);
		CHECK(result["debugView"]["view"] == "lod");
		const EditorCommandResult invalid = harness.Commands.Execute(harness.Context, "viewport.capture", withView("mips"));
		CHECK(invalid.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(invalid.Error.find("debugView") != std::string::npos);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A live capture returns the Game view's frames at exact steps of a game started paused")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.DeviceFrames = &gpu;
		harness.AddScene();
		EditorContext& context = harness.Context;
		// Temporal anti-aliasing, so a frame of an unchanged game differs from the one before it (another jitter phase).
		PostProcessComponent& postProcess = context.GetActiveScene()->FindEntityByName("PostProcess").GetComponent<PostProcessComponent>();
		postProcess.AntiAliasing = true;
		postProcess.AntiAliasingMethod = AntiAliasingMode::TAA;
		harness.Run("entity.create", { { "name", "Camera" }, { "components", { { "Camera", { { "Primary", true } } },
			{ "Transform", { { "Translation", { 0, 0, 5 } } } } } } });
		// A body that falls from the first step on: every step changes the picture.
		harness.Run("entity.create", { { "name", "Falling" }, { "components", { { "Transform", { { "Translation", { 1.2, 1.0, 0 } },
			{ "Scale", { 0.4, 0.4, 0.4 } } } }, { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } },
			{ "RigidBody", nlohmann::json::object() }, { "BoxCollider", nlohmann::json::object() } } } });
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ViewportLiveCapture");

		EditorViewport& viewport = context.GetViewport();
		const glm::uvec2 size(160, 90);
		harness.Run("viewport.setSettings", { { "gameView", { { "mode", "custom" }, { "width", size.x }, { "height", size.y } } } });
		ViewportRenderer* game = viewport.GetGameRenderer();
		REQUIRE(game);

		// The editor's frames as the Game view panel shows them: the context's update, the commands' polls, then the view's
		// render, read back here (with the steps it shows) to compare with what the live capture returned.
		struct ShownFrame
		{
			uint64_t FixedSteps = 0;
			ReadbackImage Image;
		};
		std::vector<ShownFrame> shown;
		bool render = true;
		auto runFrame = [&]()
		{
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			context.Update(Timestep(1.0f / 60.0f));
			harness.Runner.Update(context);
			viewport.SetGameViewSize(render ? size : glm::uvec2(0));
			if (render)
			{
				const std::optional<ViewportView> view = ResolveGameView(context, *game, static_cast<float>(size.x) / static_cast<float>(size.y));
				REQUIRE(view);
				REQUIRE(game->Render(context, size, *view, viewport.GetSettings(), false));
				ShownFrame frame;
				frame.FixedSteps = context.GetFixedStepCount();
				REQUIRE(Renderer::ReadTexture(game->GetOutputTexture(), frame.Image));
				shown.push_back(std::move(frame));
			}
			gpu.GetDevice().EndFrame();
		};
		auto start = [&](const nlohmann::json& parameters, std::shared_ptr<std::optional<EditorCommandResult>> completed)
		{
			const bool pending = harness.Runner.Run(context, harness.Commands, "viewport.capture", parameters,
				[completed](const EditorCommandResult& result) { *completed = result; });
			CHECK(pending);
		};
		auto sameImage = [](const ReadbackImage& expected, const DecodedImage& image)
		{
			REQUIRE(static_cast<int>(expected.Width) == image.Width);
			REQUIRE(static_cast<int>(expected.Height) == image.Height);
			int difference = 0;
			for (size_t index = 0; index < image.Pixels.size(); index++)
			{
				if (index % 4 != 3) // Captures are encoded opaque
					difference = std::max(difference, std::abs(static_cast<int>(expected.Pixels[index]) - static_cast<int>(image.Pixels[index])));
			}
			return difference;
		};

		// A game started paused runs no step until it is stepped.
		runFrame();
		CHECK(harness.Run("play.start", { { "paused", true } })["paused"] == true);
		for (int frame = 0; frame < 5; frame++)
			runFrame();
		CHECK(harness.Run("play.state")["fixedSteps"] == 0);

		// Armed before the steps run: step 3's frame and the next, read back from the view's own output. A file at the path
		// itself stays as it is: several frames are written only beside it, as numbered files.
		const std::vector<uint8_t> existing = { 1, 2, 3 };
		REQUIRE(FileSystem::WriteBytes(directory / "Strip.png", existing));
		auto completed = std::make_shared<std::optional<EditorCommandResult>>();
		start({ { "live", true }, { "camera", "scene" }, { "atFixedStep", 3 }, { "frames", 2 }, { "path", FileSystem::ToUTF8(directory / "Strip.png") } },
			completed);
		harness.Run("play.step", { { "steps", 4 }, { "wait", false } });
		for (int frame = 0; frame < 600 && !*completed; frame++)
			runFrame();
		REQUIRE(*completed);
		INFO((*completed)->Error);
		REQUIRE((*completed)->Success);
		const nlohmann::json& result = (*completed)->Value;
		CHECK(result["live"] == true);
		CHECK(result["camera"] == "scene");
		CHECK_FALSE(result.contains("Image")); // Several frames go to files only
		REQUIRE(result["images"].size() == 2);
		for (uint64_t index = 0; index < 2; index++)
		{
			const nlohmann::json& image = result["images"][index];
			CHECK(image["fixedSteps"] == 3 + index);
			CHECK(image["temporalFrames"].get<uint32_t>() > 0);
			CHECK(image["width"] == size.x);
			CHECK(image["height"] == size.y);
			const std::filesystem::path path = directory / fmt::format("Strip-{}.png", index + 1);
			CHECK(FileSystem::FromUTF8(image["path"].get<std::string>()) == path);
			const std::optional<std::vector<uint8_t>> png = FileSystem::ReadBytes(path);
			REQUIRE(png);
			const auto frame = std::find_if(shown.begin(), shown.end(), [index](const ShownFrame& candidate) { return candidate.FixedSteps == 3 + index; });
			REQUIRE(frame != shown.end());
			CHECK(sameImage(frame->Image, DecodePNG(*png)) == 0);
		}
		CHECK(FileSystem::ReadBytes(directory / "Strip.png") == existing);
		// The game is paused at step 4 again; the view's next frame shows the same state with its history moved on.
		while (context.GetPendingSteps() > 0)
			runFrame();
		runFrame();
		REQUIRE(shown.back().FixedSteps == 4);
		const std::optional<std::vector<uint8_t>> last = FileSystem::ReadBytes(directory / "Strip-2.png");
		REQUIRE(last);
		CHECK(sameImage(shown.back().Image, DecodePNG(*last)) > 0);

		// Without atFixedStep, one frame: the view's next one, returned as the image.
		auto single = std::make_shared<std::optional<EditorCommandResult>>();
		const size_t shownBefore = shown.size();
		start({ { "live", true }, { "camera", "scene" } }, single);
		for (int frame = 0; frame < 600 && !*single; frame++)
			runFrame();
		REQUIRE(*single);
		REQUIRE((*single)->Success);
		CHECK((*single)->Value["images"][0]["fixedSteps"] == 4);
		const std::optional<std::vector<uint8_t>> singlePng = Base64::Decode((*single)->Value["Image"]["Data"].get<std::string>());
		REQUIRE(singlePng);
		const DecodedImage singleImage = DecodePNG(*singlePng);
		CHECK(std::any_of(shown.begin() + static_cast<std::ptrdiff_t>(shownBefore), shown.end(),
			[&](const ShownFrame& frame) { return sameImage(frame.Image, singleImage) == 0; }));

		// Refused: no camera, what live captures do not take, frames above 1 without a path, a step already run, and live-only
		// parameters without live.
		auto refuse = [&](const nlohmann::json& parameters, EditorCommandError kind, std::string_view words)
		{
			const EditorCommandResult refused = Tests::CompleteCommand(context, harness.Commands.Execute(context, "viewport.capture", parameters));
			CHECK_FALSE(refused.Success);
			CHECK(refused.ErrorKind == kind);
			CHECK_MESSAGE(refused.Error.find(words) != std::string::npos, refused.Error);
		};
		refuse({ { "live", true } }, EditorCommandError::InvalidParameters, "camera");
		const nlohmann::json refusedParameters = { { "width", 64 }, { "height", 64 }, { "overlays", true }, { "position", { 0, 0, 1 } },
			{ "target", { 0, 0, 0 } }, { "fov", 40.0 }, { "debugView", "lod" }, { "waitForAssets", false } };
		for (const auto& [name, value] : refusedParameters.items())
			refuse({ { "live", true }, { "camera", "scene" }, { name, value } }, EditorCommandError::InvalidParameters, name);
		refuse({ { "live", true }, { "camera", "scene" }, { "frames", 2 } }, EditorCommandError::InvalidParameters, "path");
		refuse({ { "live", true }, { "camera", "scene" }, { "atFixedStep", 4 } }, EditorCommandError::Failed, "already ran");
		refuse({ { "atFixedStep", 9 } }, EditorCommandError::InvalidParameters, "live");
		refuse({ { "frames", 2 } }, EditorCommandError::InvalidParameters, "live");
		// A view that does not render: the Scene view never did here.
		refuse({ { "live", true }, { "camera", "editor" } }, EditorCommandError::Failed, "Scene view does not render");

		// A view that stops rendering while the capture waits fails it, as stopping play does.
		auto hidden = std::make_shared<std::optional<EditorCommandResult>>();
		start({ { "live", true }, { "camera", "scene" }, { "atFixedStep", 100 } }, hidden);
		render = false;
		for (int frame = 0; frame < 10 && !*hidden; frame++)
			runFrame();
		REQUIRE(*hidden);
		CHECK_FALSE((*hidden)->Success);
		CHECK((*hidden)->Error.find("stopped rendering") != std::string::npos);
		render = true;
		runFrame();
		auto stopped = std::make_shared<std::optional<EditorCommandResult>>();
		start({ { "live", true }, { "camera", "scene" }, { "atFixedStep", 100 } }, stopped);
		runFrame();
		harness.Run("play.stop");
		for (int frame = 0; frame < 10 && !*stopped; frame++)
			runFrame();
		REQUIRE(*stopped);
		CHECK_FALSE((*stopped)->Success);
		CHECK((*stopped)->Error.find("Play mode stopped") != std::string::npos);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
