#include <doctest/doctest.h>

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorViewport.h"
#include "Renderer/GPUTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/Base64.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Reflection/PropertyJson.h>

#include <stb_image.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

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

	struct ViewportGPUHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			REQUIRE_FALSE(result.IsPending());
			return result.Value;
		}

		// Runs a command through the runner, one update per frame, until it completes.
		EditorCommandResult RunFrames(std::string_view name, const nlohmann::json& parameters, int* outFrames = nullptr)
		{
			// Shared with the completion, which may outlive this call if the command never finishes.
			auto completed = std::make_shared<std::optional<EditorCommandResult>>();
			const bool pending = Runner.Run(Context, Commands, name, parameters, [completed](const EditorCommandResult& result) { *completed = result; });
			int frames = 0;
			for (; !*completed && frames < 20000; frames++)
			{
				Runner.Update(Context);
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

	TEST_CASE("A capture waits until its text is complete")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		ViewportGPUHarness harness;
		harness.AddScene();
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
}
