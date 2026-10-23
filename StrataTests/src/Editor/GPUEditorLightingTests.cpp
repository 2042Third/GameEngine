#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Renderer/GPUTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/ImageWriter.h>
#include <Strata/Renderer/Renderer.h>

#include <stb_image.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

using namespace Strata;

namespace
{

	struct LightingGPUHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;
		Tests::GPUContext* DeviceFrames = nullptr;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			const EditorCommandResult result = Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		// Through the runner, one editor frame at a time inside device frames, as the editor runs it; the frames it took.
		EditorCommandResult RunFrames(std::string_view name, const nlohmann::json& parameters, int* outFrames = nullptr)
		{
			auto completed = std::make_shared<std::optional<EditorCommandResult>>();
			Runner.Run(Context, Commands, name, parameters, [completed](const EditorCommandResult& result) { *completed = result; });
			int frames = 0;
			for (; !*completed && frames < 20000; frames++)
			{
				REQUIRE(DeviceFrames->GetDevice().BeginFrame());
				Renderer::BeginFrame();
				Context.Update(Timestep(1.0f / 60.0f));
				Runner.Update(Context);
				DeviceFrames->GetDevice().EndFrame();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			REQUIRE(*completed);
			if (outFrames)
				*outFrames = frames;
			return **completed;
		}

		nlohmann::json Bake(const nlohmann::json& parameters = nlohmann::json::object())
		{
			int frames = 0;
			const EditorCommandResult result = RunFrames("lighting.bakeProbes", parameters, &frames);
			INFO(result.Error);
			REQUIRE(result.Success);
			CHECK(frames >= 2); // Rendered, read back, written and imported over later frames, never inside the command
			return result.Value;
		}

		std::filesystem::path GetAssetPath(const std::string& path) const
		{
			return Context.GetProject()->GetAssetDirectory() / FileSystem::FromUTF8(path);
		}

		std::string GetCapture(const std::string& entity)
		{
			return Run("component.get", { { "entity", entity }, { "component", "ReflectionProbe" } })["values"]["Capture"].get<std::string>();
		}
	};

	// A bake result's entry of a probe.
	const nlohmann::json& FindProbe(const nlohmann::json& bake, const std::string& entity)
	{
		for (const nlohmann::json& probe : bake["probes"])
		{
			if (probe["entity"] == entity)
				return probe;
		}
		FAIL("The bake did not report probe ", entity);
		return bake;
	}

	// The linear RGB of a Radiance file's texel.
	glm::vec3 ReadHDRTexel(const std::filesystem::path& path, int x, int y)
	{
		const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(path);
		REQUIRE(bytes);
		int width = 0;
		int height = 0;
		int channels = 0;
		float* pixels = stbi_loadf_from_memory(bytes->data(), static_cast<int>(bytes->size()), &width, &height, &channels, 3);
		REQUIRE(pixels != nullptr);
		CHECK(width == 1024);
		CHECK(height == 512);
		const size_t index = (static_cast<size_t>(y) * width + x) * 3;
		const glm::vec3 texel(pixels[index], pixels[index + 1], pixels[index + 2]);
		stbi_image_free(pixels);
		return texel;
	}

}

TEST_SUITE("GPU.Editor.Lighting")
{
	TEST_CASE("lighting.bakeProbes writes a capture per probe, sets Capture in one step, and a rebake trashes only its own earlier file")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		LightingGPUHarness harness;
		harness.DeviceFrames = &gpu;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("GPUEditorBakeProbes") / "Game";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		// A red wall toward +X and a green one toward -X, glowing (unlit): what the probes between them see.
		const std::string red = harness.Run("material.create", { { "path", "Materials/Red.stmat" },
			{ "properties", { { "BaseColor", { 1, 0, 0, 1 } }, { "Unlit", true } } } })["asset"].get<std::string>();
		const std::string green = harness.Run("material.create", { { "path", "Materials/Green.stmat" },
			{ "properties", { { "BaseColor", { 0, 1, 0, 1 } }, { "Unlit", true } } } })["asset"].get<std::string>();
		for (const auto& [name, material, x] : { std::tuple("Red Wall", red, 3.0), std::tuple("Green Wall", green, -3.0) })
		{
			harness.Run("entity.create", { { "name", name }, { "components", {
				{ "Transform", { { "Translation", { x, 0, 0 } }, { "Scale", { 0.5, 8, 8 } } } },
				{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) }, { "Material", material } } } } } });
		}
		// Two probes with the same name under different parents.
		std::vector<std::string> probes;
		for (const char* parent : { "Left", "Right" })
		{
			const std::string group = harness.Run("entity.create", { { "name", parent } })["id"].get<std::string>();
			probes.push_back(harness.Run("entity.create", { { "name", "Probe" }, { "parent", group },
				{ "components", { { "ReflectionProbe", { { "Extents", { 2, 2, 2 } } } } } } })["id"].get<std::string>());
		}
		harness.Run("scene.saveAs", { { "path", "Scenes/Room.stscene" } });
		EditorAssetManager& assets = *harness.Context.GetAssetManager();

		// Refused while playing, and for an entity without a probe, without a trace.
		const size_t steps = harness.Context.GetUndoStack().GetPosition();
		const nlohmann::json wall = harness.Run("entity.find", { { "name", "Red Wall" } })["entities"][0];
		const EditorCommandResult notAProbe = harness.RunFrames("lighting.bakeProbes", { { "entities", { wall } } });
		CHECK_FALSE(notAProbe.Success);
		CHECK(notAProbe.ErrorKind == EditorCommandError::InvalidParameters);
		harness.RunFrames("play.start", { { "build", false } });
		const EditorCommandResult playing = harness.RunFrames("lighting.bakeProbes", nlohmann::json::object());
		CHECK_FALSE(playing.Success);
		CHECK(playing.Error.find("Stop playing") != std::string::npos);
		harness.Run("play.stop");
		CHECK(harness.Context.GetUndoStack().GetPosition() == steps);
		CHECK_FALSE(FileSystem::Exists(harness.GetAssetPath("Lighting")));

		// The first bake: a file per probe, named after it, imported before the command answers, set in one step.
		const nlohmann::json first = harness.Bake();
		CHECK(first["directory"] == "Lighting/Room");
		REQUIRE(first["probes"].size() == 2);
		CHECK(harness.Context.GetUndoStack().GetPosition() == steps + 1);
		std::vector<std::string> firstCaptures;
		for (size_t index = 0; index < 2; index++)
		{
			const nlohmann::json& probe = FindProbe(first, probes[index]);
			CHECK(probe["path"].get<std::string>() == "Lighting/Room/" + probes[index] + ".hdr");
			CHECK(probe["complete"] == true);
			CHECK(probe["previous"].is_null());
			CHECK(probe["trashed"] == false);
			const std::string capture = probe["capture"].get<std::string>();
			CHECK(harness.GetCapture(probes[index]) == capture);
			const std::optional<AssetMetadata> metadata = assets.GetMetadata(*UUIDFromJson(probe["capture"]));
			REQUIRE(metadata);
			CHECK(metadata->Type == AssetType::Texture);
			firstCaptures.push_back(capture);
			// What the probe sees: the red wall along +X (the image's middle), the green one along -X (its left edge).
			const std::filesystem::path file = harness.GetAssetPath(probe["path"].get<std::string>());
			const glm::vec3 east = ReadHDRTexel(file, 512, 256);
			const glm::vec3 west = ReadHDRTexel(file, 1, 256);
			CAPTURE(east);
			CAPTURE(west);
			CHECK(east.r > 0.8f);
			CHECK(east.g < 0.1f);
			CHECK(west.g > 0.8f);
			CHECK(west.r < 0.1f);
		}
		CHECK(firstCaptures[0] != firstCaptures[1]);
		// The staging files go once the import ended (on an I/O job).
		const std::filesystem::path staging = harness.Context.GetProject()->GetIntermediateDirectory() / "Bake";
		const auto stagingDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (FileSystem::IsDirectory(staging) && !std::filesystem::is_empty(staging) && std::chrono::steady_clock::now() < stagingDeadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		CHECK((!FileSystem::IsDirectory(staging) || std::filesystem::is_empty(staging)));

		// A rebake: new files (the names are taken), the earlier bake's files to the trash.
		const nlohmann::json second = harness.Bake();
		REQUIRE(second["probes"].size() == 2);
		for (size_t index = 0; index < 2; index++)
		{
			const nlohmann::json& probe = FindProbe(second, probes[index]);
			CHECK(probe["previous"] == firstCaptures[index]);
			CHECK(probe["trashed"] == true);
			CHECK(probe["path"].get<std::string>() == "Lighting/Room/" + probes[index] + " (1).hdr");
			CHECK_FALSE(assets.GetMetadata(*UUIDFromJson(firstCaptures[index])));
			CHECK_FALSE(FileSystem::Exists(harness.GetAssetPath("Lighting/Room/" + probes[index] + ".hdr")));
		}
		// Undo: the earlier files and captures again, the new files gone; redo: the new ones again.
		harness.Run("edit.undo");
		for (size_t index = 0; index < 2; index++)
		{
			CHECK(harness.GetCapture(probes[index]) == firstCaptures[index]);
			CHECK(FileSystem::Exists(harness.GetAssetPath("Lighting/Room/" + probes[index] + ".hdr")));
			CHECK_FALSE(FileSystem::Exists(harness.GetAssetPath("Lighting/Room/" + probes[index] + " (1).hdr")));
		}
		harness.Run("edit.redo");
		for (size_t index = 0; index < 2; index++)
		{
			CHECK(harness.GetCapture(probes[index]) == FindProbe(second, probes[index])["capture"].get<std::string>());
			CHECK(FileSystem::Exists(harness.GetAssetPath("Lighting/Room/" + probes[index] + " (1).hdr")));
			CHECK_FALSE(FileSystem::Exists(harness.GetAssetPath("Lighting/Room/" + probes[index] + ".hdr")));
		}

		// A Capture naming another texture (a sky map): baked over, never written over or trashed.
		const std::optional<std::vector<uint8_t>> sky = ImageWriter::EncodeHDR(2, 1, { 0.2f, 0.3f, 0.6f, 0.2f, 0.3f, 0.6f });
		REQUIRE(sky);
		const std::filesystem::path skySource = Tests::CreateTemporaryDirectory("GPUEditorBakeSky") / "Sky.hdr";
		REQUIRE(FileSystem::WriteBytes(skySource, *sky));
		harness.Run("asset.import", { { "file", FileSystem::ToUTF8(skySource) }, { "directory", "Imported" } });
		const std::string skyMap = UUIDToJson(assets.FindAssetByPath("Imported/Sky.hdr")).get<std::string>();
		harness.Run("component.set", { { "entity", probes[0] }, { "component", "ReflectionProbe" }, { "values", { { "Capture", skyMap } } } });
		// The probe named twice is baked once.
		const nlohmann::json third = harness.Bake({ { "entities", { probes[0], probes[0] } } });
		REQUIRE(third["probes"].size() == 1);
		CHECK(third["probes"][0]["path"].get<std::string>() == "Lighting/Room/" + probes[0] + ".hdr");
		CHECK(third["probes"][0]["previous"] == skyMap);
		CHECK(third["probes"][0]["trashed"] == false);
		CHECK(assets.GetMetadata(*UUIDFromJson(skyMap)));
		const std::optional<std::vector<uint8_t>> kept = FileSystem::ReadBytes(harness.GetAssetPath("Imported/Sky.hdr"));
		REQUIRE(kept);
		CHECK(*kept == *sky);

		harness.Runner.CancelAll("The test ends");
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("lighting.bakeProbes bakes the feature scene's probe on a copy of the project, keeping its committed capture")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		LightingGPUHarness harness;
		harness.DeviceFrames = &gpu;
		const std::filesystem::path project = Tests::CopyFeatureProject(Tests::CreateTemporaryDirectory("BakeFeatureProbe") / "Project");
		const EditorCommandResult opened = harness.RunFrames("project.open", { { "path", FileSystem::ToUTF8(project) } });
		REQUIRE_MESSAGE(opened.Success, opened.Error);
		const std::string probe = "F7E000000000008F";
		const std::string committed = harness.GetCapture(probe);
		const nlohmann::json baked = harness.Bake();
		const nlohmann::json& entry = FindProbe(baked, probe);
		CHECK(entry["path"].get<std::string>() == "Lighting/Feature/" + probe + ".hdr");
		CHECK(entry["previous"] == committed);
		CHECK(entry["trashed"] == false); // Lighting/Feature/Probe.hdr is not a file a bake made for this probe
		CHECK(harness.GetCapture(probe) == entry["capture"].get<std::string>());
		CHECK(harness.Context.GetAssetManager()->GetMetadata(*UUIDFromJson(committed)));
		CHECK(FileSystem::Exists(harness.GetAssetPath("Lighting/Feature/Probe.hdr")));
		harness.Runner.CancelAll("The test ends");
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("lighting.bakeProbes trashes the earlier bake's file also when the saved scene names it, and undo and redo follow")
	{
		// Bake, save, bake again: the scene's file names the first capture until it is saved again, but the probe in memory
		// names the new one, so the earlier file goes to the trash as the bake promises.
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		LightingGPUHarness harness;
		harness.DeviceFrames = &gpu;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("GPUEditorRebakeSaved") / "Game";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		const std::string probe = harness.Run("entity.create", { { "name", "Probe" },
			{ "components", { { "ReflectionProbe", { { "Extents", { 2, 2, 2 } } } } } } })["id"].get<std::string>();
		harness.Run("scene.saveAs", { { "path", "Scenes/Hall.stscene" } });
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		const AssetHandle scene = harness.Context.GetSceneHandle();

		const std::string first = FindProbe(harness.Bake(), probe)["capture"].get<std::string>();
		const AssetHandle firstHandle = *UUIDFromJson(first);
		harness.Run("scene.save");
		// The saved scene's recorded references name the first capture.
		const std::vector<AssetHandle> referencers = assets.GetReferencers(firstHandle);
		REQUIRE(std::find(referencers.begin(), referencers.end(), scene) != referencers.end());

		const nlohmann::json second = harness.Bake();
		const nlohmann::json& entry = FindProbe(second, probe);
		CHECK(entry["previous"] == first);
		CHECK(entry["trashed"] == true);
		CHECK_FALSE(assets.GetMetadata(firstHandle));
		CHECK_FALSE(FileSystem::Exists(harness.GetAssetPath("Lighting/Hall/" + probe + ".hdr")));

		// Undo restores the earlier file and capture; redo trashes it again although the saved scene still names it.
		harness.Run("edit.undo");
		CHECK(harness.GetCapture(probe) == first);
		CHECK(FileSystem::Exists(harness.GetAssetPath("Lighting/Hall/" + probe + ".hdr")));
		harness.Run("edit.redo");
		CHECK(harness.GetCapture(probe) == entry["capture"].get<std::string>());
		CHECK_FALSE(FileSystem::Exists(harness.GetAssetPath("Lighting/Hall/" + probe + ".hdr")));
		CHECK(FileSystem::Exists(harness.GetAssetPath("Lighting/Hall/" + probe + " (1).hdr")));

		// Another scene that names the earlier capture keeps it: a copy saved with it, then the first scene again.
		harness.Run("edit.undo");
		harness.Run("scene.saveAs", { { "path", "Scenes/Copy.stscene" } });
		harness.Run("scene.open", { { "scene", "Scenes/Hall.stscene" } });
		REQUIRE(harness.Context.GetSceneHandle() == scene);
		const nlohmann::json third = harness.Bake();
		CHECK(FindProbe(third, probe)["previous"] == first);
		CHECK(FindProbe(third, probe)["trashed"] == false);
		CHECK(assets.GetMetadata(firstHandle));

		harness.Runner.CancelAll("The test ends");
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
