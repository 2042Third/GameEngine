#include "Perf/SceneGenerators.h"

#include "Strata/Core/JobSystem.h"
#include "Strata/Scene/Components.h"

#include <doctest/doctest.h>
#include <glm/gtc/quaternion.hpp>

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

	void AddCameraAndLight(Scene& scene)
	{
		Entity camera = Created(scene.CreateEntity("Camera"));
		camera.GetTransform().Translation = glm::vec3(0.0f, 2.0f, 10.0f);
		camera.AddComponent<CameraComponent>().Primary = true;

		Entity light = Created(scene.CreateEntity("Sun"));
		light.GetTransform().Rotation = glm::angleAxis(glm::radians(-50.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		light.AddComponent<DirectionalLightComponent>();
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
