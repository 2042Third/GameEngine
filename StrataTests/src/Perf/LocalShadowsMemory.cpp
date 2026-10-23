#include "Perf/LocalShadowsMemory.h"

#include "Perf/SceneGenerators.h"
#include "Renderer/GPUTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Scene/Scene.h"
#include "TestHelpers.h"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace Strata::Tests
{

	namespace
	{

		constexpr uint32_t c_Width = 1920;
		constexpr uint32_t c_Height = 1080;
		constexpr size_t c_Cubes = 100'000;
		constexpr size_t c_Lamps = 16;
		constexpr float c_LampSpacing = 60.0f;

		LocalShadowsMemoryResult Measure(GPUContext& gpu)
		{
			LocalShadowsMemoryResult result;
			GraphicsDevice& device = gpu.GetDevice();
			result.ValidationEnabled = device.GetInfo().ValidationEnabled;
			const uint64_t errorsBefore = device.GetErrorCount();
			// The built-in assets only, as the renderer tests have them.
			const std::filesystem::path pack = CreateTemporaryDirectory("LocalShadowsMemory") / "Empty.stpak";
			if (!AssetPackWriter::Write(pack, std::vector<AssetMetadata>(), [](const AssetMetadata&, std::vector<uint8_t>&, std::string*) { return true; }))
			{
				result.Error = "The empty pack could not be written";
				return result;
			}
			Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack);
			if (!manager)
			{
				result.Error = "The asset manager could not be created";
				return result;
			}
			AssetManager::SetActive(manager);
			{
				Scene scene("TemporalShadowedCubes");
				Perf::AddCameraAndLight(scene);
				Perf::CreateCubeGrid(scene, c_Cubes);
				Perf::CreateShadowedLampGrid(scene, c_Lamps, c_LampSpacing);
				scene.CreateEntity("Post Process").AddComponent<PostProcessComponent>().AntiAliasingMethod = AntiAliasingMode::TAA;
				// The view of the whole grid from above (RendererPerfTests' GridView).
				const float half = static_cast<float>(std::ceil(std::sqrt(static_cast<double>(c_Cubes)))) * Perf::c_GridSpacing * 0.5f;
				const float aspect = static_cast<float>(c_Width) / static_cast<float>(c_Height);
				SceneCamera camera;
				camera.Position = glm::vec3(0.0f, half * 1.6f, half * 1.3f);
				camera.View = glm::lookAt(camera.Position, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
				camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), aspect, 0.1f, half * 10.0f);
				camera.Near = 0.1f;
				camera.Far = half * 10.0f;
				camera.VerticalFOV = glm::radians(60.0f);

				SceneRenderer renderer;
				renderer.SetViewportSize(c_Width, c_Height);
				for (uint32_t frame = 0; frame < c_LocalShadowsMemoryFrames && result.Error.empty(); frame++)
				{
					if (!device.BeginFrame())
					{
						result.Error = "The device could not begin a frame";
						break;
					}
					Renderer::BeginFrame();
					manager->Update();
					if (!renderer.Render(scene, camera))
						result.Error = "A frame was not rendered";
					device.EndFrame();
				}
				device.WaitForIdle();
				result.LocalShadowLights = renderer.GetStats().LocalShadowLights;
				result.TemporalAntiAliasing = renderer.GetStats().TemporalAntiAliasing;
				if (const std::optional<ProcessMemoryInfo> memory = Platform::GetProcessMemory())
					result.PeakPrivateMB = static_cast<double>(memory->PeakPrivateBytes) / (1024.0 * 1024.0);
			}
			AssetManager::SetActive(nullptr);
			result.NewErrors = static_cast<uint32_t>(device.GetErrorCount() - errorsBefore);
			result.Completed = result.Error.empty();
			return result;
		}

	}

	int RunLocalShadowsMemoryProcess(int argc, char** argv)
	{
		if (argc < 3)
		{
			std::fprintf(stderr, "Usage: --strata-test-helper=local-shadows-memory <result file>\n");
			return 2;
		}
		// Like an application: worker and I/O pools with the default sizes, at full speed.
		Platform::DisableExecutionSpeedThrottling();
		JobSystem::Init();
		LocalShadowsMemoryResult result;
		{
			GPUContext gpu;
			if (gpu.IsValid())
				result = Measure(gpu);
			else
				result.Error = "No GPU device";
		}
		JobSystem::Shutdown();
		GPUContext::ShutdownShared();

		const nlohmann::json json = { { "Completed", result.Completed }, { "Error", result.Error }, { "ValidationEnabled", result.ValidationEnabled },
			{ "PeakPrivateMB", result.PeakPrivateMB }, { "LocalShadowLights", result.LocalShadowLights },
			{ "TemporalAntiAliasing", result.TemporalAntiAliasing }, { "NewErrors", result.NewErrors } };
		if (!FileSystem::WriteText(FileSystem::FromUTF8(argv[2]), JsonUtils::Dump(json)))
		{
			std::fprintf(stderr, "Could not write '%s'\n", argv[2]);
			return 1;
		}
		return 0;
	}

	std::optional<LocalShadowsMemoryResult> ReadLocalShadowsMemoryResult(const std::filesystem::path& file, std::string& error)
	{
		const std::optional<std::string> text = FileSystem::ReadText(file);
		if (!text)
		{
			error = "the result file could not be read";
			return std::nullopt;
		}
		const std::optional<nlohmann::json> json = JsonUtils::Parse(*text, &error);
		if (!json || !json->is_object())
		{
			if (error.empty())
				error = "the result is not an object";
			return std::nullopt;
		}
		LocalShadowsMemoryResult result;
		try
		{
			result.Completed = json->at("Completed").get<bool>();
			result.Error = json->at("Error").get<std::string>();
			result.ValidationEnabled = json->at("ValidationEnabled").get<bool>();
			result.PeakPrivateMB = json->at("PeakPrivateMB").get<double>();
			result.LocalShadowLights = json->at("LocalShadowLights").get<uint32_t>();
			result.TemporalAntiAliasing = json->at("TemporalAntiAliasing").get<bool>();
			result.NewErrors = json->at("NewErrors").get<uint32_t>();
		}
		catch (const nlohmann::json::exception& exception)
		{
			error = exception.what();
			return std::nullopt;
		}
		return result;
	}

}
