#include "World/WorldDriveGame.h"

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Project/GameManifest.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scripting/ScriptingComponents.h"
#include "Strata/World/WorldComponents.h"
#include "World/WorldTestUtils.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace Strata::Tests
{

	int WriteWorldDriveGame(const std::filesystem::path& directory, uint32_t entitiesPerCell, uint32_t bodiesPerCell)
	{
		GeneratedWorldSpecification specification;
		specification.Name = "WorldDrive";
		specification.GridX = 6;
		specification.GridZ = 6;
		specification.CellSize = 32.0f;
		specification.EntitiesPerCell = entitiesPerCell;
		specification.BodiesPerCell = bodiesPerCell;
		specification.Floors = true;
		specification.PropsPerCell = 4;
		specification.BuildPersistent = [&specification](Scene& persistent, AssetHandle world)
		{
			persistent.CreateEntity("World").AddComponent<WorldComponent>().World = world;
			Entity driver = persistent.CreateEntity("Driver");
			driver.AddComponent<StreamingSourceComponent>();
			ScriptEntry& drive = AddScriptEntry(driver, "WorldDrive");
			AddFieldOverride(drive, "GridX", PropertyType::Int, PropertyValue(static_cast<int32_t>(specification.GridX)));
			AddFieldOverride(drive, "GridZ", PropertyType::Int, PropertyValue(static_cast<int32_t>(specification.GridZ)));
			AddFieldOverride(drive, "CellSize", PropertyType::Float, PropertyValue(specification.CellSize));
			AddFieldOverride(drive, "Origin", PropertyType::Vec3,
				PropertyValue(glm::vec3(static_cast<float>(specification.GridOrigin.x), 0.0f, static_cast<float>(specification.GridOrigin.y))));
			// The floor is a body of the cell too.
			AddFieldOverride(drive, "BodiesPerCell", PropertyType::Int, PropertyValue(static_cast<int32_t>(specification.BodiesPerCell + 1)));
		};
		std::string error;
		JobSystem::Init();
		const std::optional<std::filesystem::path> written = TryWriteGeneratedWorldGame(directory, specification, nullptr, &error);
		JobSystem::Shutdown();
		if (!written)
		{
			std::fprintf(stderr, "Cannot write the world drive game: %s\n", error.c_str());
			return 1;
		}
		const std::filesystem::path& manifestPath = *written;

		const std::filesystem::path module = GetTestScriptModule(STRATA_TEST_SCRIPTS_WORLD);
		std::optional<GameManifest> manifest = GameManifest::Load(manifestPath, &error);
		if (!manifest)
		{
			std::fprintf(stderr, "Cannot write the world drive game: %s\n", error.c_str());
			return 1;
		}
		if (!FileSystem::Copy(module, directory / module.filename()))
		{
			std::fprintf(stderr, "Cannot write the world drive game: the script module '%s' cannot be copied\n", FileSystem::ToUTF8(module).c_str());
			return 1;
		}
		manifest->ScriptModule = FileSystem::ToUTF8(module.filename());
		if (!manifest->Save(manifestPath, &error))
		{
			std::fprintf(stderr, "Cannot write the world drive game: %s\n", error.c_str());
			return 1;
		}
		std::printf("Wrote the world drive game to '%s'\n", FileSystem::ToUTF8(manifestPath).c_str());
		return 0;
	}

}
