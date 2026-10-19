#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Renderer/GPUTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/Base64.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Reflection/PropertyJson.h>

#include <stb_image.h>

#include <chrono>
#include <memory>
#include <optional>
#include <thread>

using namespace Strata;

namespace
{

	glm::ivec4 DecodeCapturedCenter(const nlohmann::json& capture)
	{
		const std::optional<std::vector<uint8_t>> png = Base64::Decode(capture["Image"]["Data"].get<std::string>());
		REQUIRE(png);
		int width = 0;
		int height = 0;
		int channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(png->data(), static_cast<int>(png->size()), &width, &height, &channels, 4);
		REQUIRE(pixels);
		const stbi_uc* center = pixels + (static_cast<size_t>(height / 2) * width + width / 2) * 4;
		const glm::ivec4 pixel(center[0], center[1], center[2], center[3]);
		stbi_image_free(pixels);
		return pixel;
	}

	struct AssetOperationsGPUHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			const EditorCommandResult result = Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		// viewport.capture through the runner until no asset is loading any more; the center pixel.
		glm::ivec4 CaptureCenter()
		{
			for (int attempt = 0; attempt < 100; attempt++)
			{
				auto completed = std::make_shared<std::optional<EditorCommandResult>>();
				Runner.Run(Context, Commands, "viewport.capture", { { "width", 64 }, { "height", 64 }, { "overlays", false } },
					[completed](const EditorCommandResult& result) { *completed = result; });
				for (int frame = 0; !*completed && frame < 5000; frame++)
				{
					Context.Update(Timestep(1.0f / 60.0f));
					Runner.Update(Context);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				REQUIRE(*completed);
				const EditorCommandResult& result = **completed;
				INFO(result.Error);
				REQUIRE(result.Success);
				if (result.Value["pendingAssets"] == 0)
					return DecodeCapturedCenter(result.Value);
			}
			FAIL("Assets were still loading after 100 captures");
			return {};
		}
	};

}

TEST_SUITE("GPU.Editor.AssetOperations")
{
	TEST_CASE("A material deleted with force renders as missing, and undo makes the scene render it again")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		AssetOperationsGPUHarness harness;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("GPUEditorAssetDelete") / "Game";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		const std::string red = harness.Run("material.create", { { "path", "Materials/Red.stmat" },
			{ "properties", { { "BaseColor", { 1, 0, 0, 1 } } } } })["asset"].get<std::string>();
		harness.Run("entity.create", { { "name", "PostProcess" }, { "components", { { "PostProcess", { { "Tonemapper", "None" }, { "AutoExposure", false },
			{ "Vignette", 0.0 }, { "Bloom", false }, { "AmbientOcclusion", false }, { "AntiAliasing", false } } } } } });
		harness.Run("entity.create", { { "name", "Sun" }, { "components", { { "DirectionalLight", { { "Intensity", 3.0 } } } } } });
		harness.Run("entity.create", { { "name", "Cube" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) },
			{ "Material", red } } } } } });
		harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		harness.Run("camera.set", { { "position", { 0, 0, 4 } }, { "target", { 0, 0, 0 } } });

		const glm::ivec4 before = harness.CaptureCenter();
		CHECK(before.r > before.b + 80);

		// Deleted although the cube uses it: the cube falls back to the default (white) material.
		harness.Run("asset.delete", { { "asset", red }, { "force", true } });
		const glm::ivec4 deleted = harness.CaptureCenter();
		CHECK(std::abs(deleted.r - deleted.b) < 20);

		// Undo: the same handle again, so the cube is red again.
		harness.Run("edit.undo");
		const glm::ivec4 restored = harness.CaptureCenter();
		CHECK(glm::all(glm::lessThanEqual(glm::abs(restored - before), glm::ivec4(2))));

		harness.Runner.CancelAll("The test ends");
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
