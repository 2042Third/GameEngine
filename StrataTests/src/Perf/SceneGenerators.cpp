#include "Perf/SceneGenerators.h"

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Physics/PhysicsComponents.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scripting/ScriptingComponents.h"

#include <doctest/doctest.h>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace Strata::Tests::Perf
{

	// The generators write the transforms of entities they have just created: new entities are recomputed by the next
	// UpdateWorldTransforms anyway, so these writes need no signal (see Scene, "the transform contract").

	namespace
	{

		// Ends the test when the scene could not create an entity (it is full). Checked without an assertion per entity,
		// which would cost more than creating it.
		Entity Created(Entity entity)
		{
			if (!entity)
				FAIL("The scene could not create an entity (it holds ", Scene::c_MaxEntities, " at most)");
			return entity;
		}

	}

	std::vector<Entity> CreateFlatRoots(Scene& scene, size_t count)
	{
		std::vector<Entity> roots;
		roots.reserve(count);
		for (size_t index = 0; index < count; index++)
		{
			Entity root = Created(scene.CreateEntity("Root"));
			root.GetTransform().Translation = glm::vec3(0.01f * static_cast<float>(index), 0.0f, 0.0f);
			roots.push_back(root);
		}
		return roots;
	}

	NestedHierarchy CreateNestedHierarchy(Scene& scene, size_t count, uint32_t fanOut)
	{
		REQUIRE(count > 0);
		REQUIRE(fanOut > 0);

		NestedHierarchy hierarchy;
		hierarchy.Entities.reserve(count);
		hierarchy.Root = Created(scene.CreateEntity("Root"));
		hierarchy.Entities.push_back(hierarchy.Root);
		// Breadth first: the entity at `parent` gets all its children before the next one gets any.
		for (size_t parent = 0; hierarchy.Entities.size() < count; parent++)
		{
			const Entity parentEntity = hierarchy.Entities[parent];
			for (uint32_t child = 0; child < fanOut && hierarchy.Entities.size() < count; child++)
			{
				Entity entity = Created(scene.CreateChildEntity(parentEntity, "Node"));
				entity.GetTransform().Translation = glm::vec3(0.1f, 0.01f * static_cast<float>(child), 0.0f);
				hierarchy.Entities.push_back(entity);
			}
		}
		return hierarchy;
	}

	std::vector<Entity> CreateScriptedEntities(Scene& scene, size_t count, std::string_view className)
	{
		std::vector<Entity> entities;
		entities.reserve(count);
		for (size_t index = 0; index < count; index++)
		{
			Entity entity = Created(scene.CreateEntity("Scripted"));
			entity.GetTransform().Translation = glm::vec3(0.01f * static_cast<float>(index), 1.0f, 0.0f);
			ScriptEntry& entry = entity.AddComponent<ScriptComponent>().Scripts.emplace_back();
			entry.ClassName = std::string(className);
			entities.push_back(entity);
		}
		return entities;
	}

	BoxStacks CreateBoxStacks(Scene& scene, size_t count, uint32_t height)
	{
		REQUIRE(height > 0);
		constexpr float boxSize = 1.0f;
		constexpr float gap = 0.2f;
		constexpr float pitch = boxSize + gap;
		const size_t columns = (count + height - 1) / height;
		const size_t side = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(columns))));
		const float extent = 0.5f * static_cast<float>(side) * pitch;

		BoxStacks stacks;
		stacks.Floor = Created(scene.CreateEntity("Floor"));
		stacks.Floor.GetTransform().Translation = glm::vec3(0.0f, -0.5f, 0.0f);
		stacks.Floor.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(extent + 10.0f, 0.5f, extent + 10.0f);

		stacks.Boxes.reserve(count);
		for (size_t index = 0; index < count; index++)
		{
			const size_t column = index / height;
			const size_t level = index % height;
			const float x = (static_cast<float>(column % side) + 0.5f) * pitch - extent;
			const float z = (static_cast<float>(column / side) + 0.5f) * pitch - extent;
			const float y = gap + 0.5f * boxSize + static_cast<float>(level) * pitch;

			Entity box = Created(scene.CreateEntity("Box"));
			box.GetTransform().Translation = glm::vec3(x, y, z);
			box.AddComponent<RigidBodyComponent>();
			box.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.5f * boxSize);
			stacks.Boxes.push_back(box);
		}
		return stacks;
	}

	void AddCameraAndLight(Scene& scene)
	{
		Entity camera = Created(scene.CreateEntity("Camera"));
		camera.GetTransform().Translation = glm::vec3(0.0f, 2.0f, 10.0f);
		camera.AddComponent<CameraComponent>().Primary = true;

		Entity light = Created(scene.CreateEntity("Sun"));
		light.GetTransform().Rotation = glm::angleAxis(glm::radians(-50.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		light.AddComponent<DirectionalLightComponent>();
	}

	namespace
	{

		// Position of grid cell `index` of a square grid holding `count` cells, c_GridSpacing apart and centered on the
		// origin in XZ.
		glm::vec3 GridPosition(size_t index, size_t count, float height)
		{
			const size_t side = std::max<size_t>(1, static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(count)))));
			const float offset = (static_cast<float>(side) - 1.0f) * c_GridSpacing * 0.5f;
			const float x = static_cast<float>(index % side) * c_GridSpacing - offset;
			const float z = static_cast<float>(index / side) * c_GridSpacing - offset;
			return glm::vec3(x, height, z);
		}

	}

	std::vector<Entity> CreateCubeGrid(Scene& scene, size_t count, std::span<const AssetHandle> materials, Entity parent)
	{
		std::vector<Entity> cubes;
		cubes.reserve(count);
		for (size_t index = 0; index < count; index++)
		{
			Entity cube = Created(parent ? scene.CreateChildEntity(parent, "Cube") : scene.CreateEntity("Cube"));
			cube.GetTransform().Translation = GridPosition(index, count, 0.5f);
			MeshRendererComponent& renderer = cube.AddComponent<MeshRendererComponent>();
			renderer.Mesh = BuiltinAssets::CubeMesh;
			if (!materials.empty())
				renderer.Material = materials[index % materials.size()];
			cubes.push_back(cube);
		}
		return cubes;
	}

	std::vector<Entity> CreatePointLightGrid(Scene& scene, size_t count, float range, float height, Entity parent)
	{
		std::vector<Entity> lights;
		lights.reserve(count);
		for (size_t index = 0; index < count; index++)
		{
			Entity light = Created(parent ? scene.CreateChildEntity(parent, "Light") : scene.CreateEntity("Light"));
			light.GetTransform().Translation = GridPosition(index, count, height);
			PointLightComponent& component = light.AddComponent<PointLightComponent>();
			component.Range = range;
			// Neighbors differ in color, so a frame of them shows each light.
			component.Color = glm::vec3(index % 3 == 0 ? 1.0f : 0.3f, index % 3 == 1 ? 1.0f : 0.3f, index % 3 == 2 ? 1.0f : 0.3f);
			lights.push_back(light);
		}
		return lights;
	}

	ScopedApplicationJobSystem::ScopedApplicationJobSystem()
	{
		REQUIRE_FALSE(JobSystem::IsInitialized());
		JobSystem::Init(JobSystemSpecification());
	}

	ScopedApplicationJobSystem::~ScopedApplicationJobSystem()
	{
		JobSystem::Shutdown();
	}

}
