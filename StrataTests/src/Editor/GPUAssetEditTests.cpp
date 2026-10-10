#include <doctest/doctest.h>

#include "Editor/AssetEdit.h"
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
#include <Strata/Renderer/Material.h>

#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <thread>

using namespace Strata;

namespace
{

	// The pixel at the center of a viewport capture (RGBA8).
	glm::ivec4 DecodeCenter(const nlohmann::json& capture)
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

	struct MaterialGPUHarness
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

		// viewport.capture through the runner, one editor frame (context update, then the runner) at a time, again until no
		// asset is loading any more.
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
					return DecodeCenter(result.Value);
			}
			FAIL("Assets were still loading after 100 captures");
			return {};
		}
	};

}

TEST_SUITE("GPU.Editor.Material")
{
	TEST_CASE("Changing a material's base color through the asset inspector's path changes the viewport, and undo restores it")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		MaterialGPUHarness harness;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("GPUEditorMaterial") / "Game";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		const AssetHandle red = *UUIDFromJson(harness.Run("material.create", { { "path", "Materials/Red.stmat" },
			{ "properties", { { "BaseColor", { 1, 0, 0, 1 } } } } })["asset"]);

		// Plain output: no automatic exposure, tone curve, vignette or anti-aliasing.
		harness.Run("entity.create", { { "name", "PostProcess" }, { "components", { { "PostProcess", { { "Tonemapper", "None" }, { "AutoExposure", false },
			{ "Vignette", 0.0 }, { "Bloom", false }, { "AmbientOcclusion", false }, { "AntiAliasing", false } } } } } });
		harness.Run("entity.create", { { "name", "Sun" }, { "components", { { "DirectionalLight", { { "Intensity", 3.0 } } } } } });
		harness.Run("entity.create", { { "name", "Cube" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) },
			{ "Material", UUIDToJson(red) } } } } } });
		harness.Run("camera.set", { { "position", { 0, 0, 4 } }, { "target", { 0, 0, 0 } } });

		const glm::ivec4 before = harness.CaptureCenter();
		// The lit face is red (the sun's specular highlight adds some of every channel).
		CHECK(before.r > before.b + 80);

		// The panel previews while the color is dragged and saves when the drag ends.
		const std::vector<PropertyInfo>& properties = Material::GetPropertyInfos();
		const auto baseColor = std::find_if(properties.begin(), properties.end(), [](const PropertyInfo& property) { return property.Name == "BaseColor"; });
		REQUIRE(baseColor != properties.end());
		std::string error;
		REQUIRE_MESSAGE(AssetEdit::SetMaterialProperty(harness.Context, red, *baseColor, PropertyValue(glm::vec4(0.5f, 0.0f, 0.5f, 1.0f)), true, &error), error);
		const glm::ivec4 previewed = harness.CaptureCenter();
		CHECK(std::abs(previewed.r - previewed.b) < 20);
		REQUIRE_MESSAGE(AssetEdit::SetMaterialProperty(harness.Context, red, *baseColor, PropertyValue(glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)), false, &error), error);
		harness.Context.GetUndoStack().BreakMerge();
		const glm::ivec4 blue = harness.CaptureCenter();
		CHECK(blue.b > blue.r + 80);

		harness.Run("edit.undo");
		const glm::ivec4 restored = harness.CaptureCenter();
		CHECK(glm::all(glm::lessThanEqual(glm::abs(restored - before), glm::ivec4(2))));
		harness.Run("edit.redo");
		const glm::ivec4 redone = harness.CaptureCenter();
		CHECK(glm::all(glm::lessThanEqual(glm::abs(redone - blue), glm::ivec4(2))));

		harness.Runner.CancelAll("The test ends");
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
