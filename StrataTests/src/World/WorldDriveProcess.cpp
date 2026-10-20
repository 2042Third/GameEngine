#include "World/WorldDriveProcess.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Runtime/GameRuntime.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/World/WorldComponents.h"
#include "World/WorldTestUtils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace Strata::Tests
{

	namespace
	{

		constexpr float c_DriveCellSize = 32.0f;
		constexpr float c_DriveSpeed = 40.0f; // m/s
		constexpr double c_FrameSeconds = 1.0 / 60.0;

		nlohmann::json ToJson(const WorldDriveResult& result)
		{
			return { { "frames", result.Frames }, { "frameP95Ms", result.FrameP95Ms }, { "frameMaxMs", result.FrameMaxMs },
				{ "peakPrivateMB", result.PeakPrivateMB }, { "loadsStarted", result.LoadsStarted }, { "mostCellsLoaded", result.MostCellsLoaded },
				{ "mostBodiesWaiting", result.MostBodiesWaiting }, { "error", result.Error } };
		}

		WorldDriveResult Drive(const std::filesystem::path& manifest, uint32_t frames)
		{
			WorldDriveResult result;
			std::string error;
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
			if (!game)
			{
				result.Error = fmt::format("The game cannot start: {}", error);
				return result;
			}
			if (!game->WaitForStart(std::chrono::minutes(5)))
			{
				result.Error = "The game did not start within 5 minutes";
				return result;
			}
			const Ref<Scene> scene = game->GetScene();
			Entity driver = scene ? scene->FindEntityByName("Driver") : Entity();
			if (!driver)
			{
				result.Error = "The game's scene has no Driver";
				return result;
			}
			// Along the diagonal from its first cell's middle, as far as the frames take it.
			glm::vec3 position(c_DriveCellSize * 0.5f, 1.0f, c_DriveCellSize * 0.5f);
			const glm::vec3 step = glm::normalize(glm::vec3(1.0f, 0.0f, 1.0f)) * (c_DriveSpeed * static_cast<float>(c_FrameSeconds));
			std::vector<double> work;
			work.reserve(frames);
			for (uint32_t frame = 0; frame < frames; frame++)
			{
				const auto frameStart = std::chrono::steady_clock::now();
				position += step;
				driver.GetComponent<TransformComponent>().Translation = position;
				driver.MarkModified<TransformComponent>();
				game->Update(Timestep(static_cast<float>(c_FrameSeconds)));
				work.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count());
				if (const IWorldStreaming* streaming = scene->GetService<IWorldStreaming>())
					result.MostCellsLoaded = std::max(result.MostCellsLoaded, streaming->GetStats().CellsLoaded);
				if (const IPhysicsCapacity* capacity = scene->GetService<IPhysicsCapacity>())
					result.MostBodiesWaiting = std::max(result.MostBodiesWaiting, capacity->GetCapacityInfo().WaitingForSlot);
				std::this_thread::sleep_until(frameStart + std::chrono::microseconds(16'667));
			}
			if (const IWorldStreaming* streaming = scene->GetService<IWorldStreaming>())
				result.LoadsStarted = streaming->GetStats().LoadsStarted;
			result.Frames = frames;
			std::vector<double> sorted = work;
			std::sort(sorted.begin(), sorted.end());
			if (!sorted.empty())
			{
				result.FrameP95Ms = sorted[std::min(sorted.size() - 1, static_cast<size_t>(static_cast<double>(sorted.size()) * 0.95))];
				result.FrameMaxMs = sorted.back();
			}
			if (const std::optional<ProcessMemoryInfo> memory = Platform::GetProcessMemory())
				result.PeakPrivateMB = static_cast<double>(memory->PeakPrivateBytes) / (1024.0 * 1024.0);
			return result;
		}

	}

	std::optional<std::filesystem::path> WriteWorldDriveWorld(const std::filesystem::path& directory, uint32_t side, uint32_t entitiesPerCell,
		uint32_t bodiesPerCell, std::string* outError, GeneratedWorld* outWorld)
	{
		GeneratedWorldSpecification specification;
		specification.Name = "Drive";
		specification.GridX = side;
		specification.GridZ = side;
		specification.CellSize = c_DriveCellSize;
		specification.StreamingDistance = 128.0f;
		specification.EntitiesPerCell = entitiesPerCell;
		specification.BodiesPerCell = bodiesPerCell;
		specification.Floors = true;
		specification.GenerateCellsWhenPacked = true;
		specification.CookDocuments = true;
		specification.BuildPersistent = [](Scene& persistent, AssetHandle world)
		{
			persistent.CreateEntity("World").AddComponent<WorldComponent>().World = world;
			persistent.CreateEntity("Driver").AddComponent<StreamingSourceComponent>();
		};
		return TryWriteGeneratedWorldGame(directory, specification, outWorld, outError);
	}

	int RunWorldDriveProcess(int argc, char** argv)
	{
		if (argc < 5)
		{
			std::fprintf(stderr, "Usage: --strata-test-helper=world-drive <manifest> <frames> <result file>\n");
			return 2;
		}
		const std::filesystem::path manifest = FileSystem::FromUTF8(argv[2]);
		const long frames = std::strtol(argv[3], nullptr, 10);
		const std::filesystem::path resultFile = FileSystem::FromUTF8(argv[4]);
		if (frames <= 0)
		{
			std::fprintf(stderr, "The frame count must be positive, not '%s'\n", argv[3]);
			return 2;
		}
		// Like an application: worker and I/O pools with the default sizes, at full speed.
		Platform::DisableExecutionSpeedThrottling();
		JobSystem::Init();
		const WorldDriveResult result = Drive(manifest, static_cast<uint32_t>(frames));
		JobSystem::Shutdown();
		if (!FileSystem::WriteText(resultFile, JsonUtils::Dump(ToJson(result))))
		{
			std::fprintf(stderr, "Could not write '%s'\n", argv[4]);
			return 1;
		}
		return 0;
	}

	std::optional<WorldDriveResult> ReadWorldDriveResult(const std::filesystem::path& file, std::string* outError)
	{
		std::string error;
		const std::optional<std::string> text = FileSystem::ReadText(file);
		const std::optional<nlohmann::json> json = text ? JsonUtils::Parse(*text, &error) : std::nullopt;
		if (!json || !json->is_object())
		{
			if (outError)
				*outError = text ? fmt::format("'{}' holds no result: {}", FileSystem::ToUTF8(file), error) : fmt::format("'{}' cannot be read", FileSystem::ToUTF8(file));
			return std::nullopt;
		}
		const auto number = [&json](std::string_view key)
		{
			const nlohmann::json* value = JsonUtils::Find(*json, key);
			return value && value->is_number() ? value->get<double>() : 0.0;
		};
		WorldDriveResult result;
		result.Frames = static_cast<uint32_t>(JsonUtils::GetUInt(*json, "frames", 0));
		result.FrameP95Ms = number("frameP95Ms");
		result.FrameMaxMs = number("frameMaxMs");
		result.PeakPrivateMB = number("peakPrivateMB");
		result.LoadsStarted = JsonUtils::GetUInt(*json, "loadsStarted", 0);
		result.MostCellsLoaded = static_cast<uint32_t>(JsonUtils::GetUInt(*json, "mostCellsLoaded", 0));
		result.MostBodiesWaiting = static_cast<uint32_t>(JsonUtils::GetUInt(*json, "mostBodiesWaiting", 0));
		result.Error = JsonUtils::GetString(*json, "error", "");
		return result;
	}

}
