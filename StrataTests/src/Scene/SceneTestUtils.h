#pragma once

#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/GameUI/GameUIScene.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/UnknownComponents.h"

#include <glm/gtc/quaternion.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Strata::Tests
{

	// The scene's caches agree with a full recomputation: hierarchy links, sibling positions, lookup indices, activity, every
	// world transform that is not known to be stale, and the game UI's layout.
	inline void CheckSceneCaches(const Scene& scene)
	{
		std::string error;
		CHECK_MESSAGE(scene.ValidateHierarchy(&error), error);
		error.clear();
		CHECK_MESSAGE(scene.ValidateWorldTransforms(&error), error);
		// The game UI's layout, once the scene has one (validation only reads the scene; the cast reaches the UI's own state).
		if (GameUIScene* ui = GameUIScene::Find(const_cast<Scene&>(scene)))
		{
			error.clear();
			CHECK_MESSAGE(ui->Validate(&error), error);
		}
	}

	// A committed scene document (a sample's, the feature project's, a template's) loads with every component registered (none
	// kept as unknown) and without warnings, and serializing what it loaded is a fixed point: saving it again writes the same.
	inline void CheckSceneFileLoadsCleanly(const std::filesystem::path& path)
	{
		CAPTURE(FileSystem::ToUTF8(path));
		const std::optional<std::string> text = FileSystem::ReadText(path);
		REQUIRE(text);
		const std::optional<nlohmann::json> document = JsonUtils::Parse(*text);
		REQUIRE(document);
		Scene scene;
		std::string error;
		std::vector<std::string> warnings;
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(scene, *document, &error, &warnings), error);
		for (const std::string& warning : warnings)
			FAIL_CHECK("Warning: ", warning);
		size_t unknown = 0;
		for ([[maybe_unused]] const auto entity : scene.GetRegistry().view<UnknownComponentsComponent>())
			unknown++;
		CHECK(unknown == 0);
		CHECK(scene.GetEntityCount() > 0);

		const nlohmann::json saved = SceneSerializer::Serialize(scene);
		Scene reloaded;
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(reloaded, saved, &error, &warnings), error);
		CHECK(SceneSerializer::Serialize(reloaded) == saved);
	}

	// Watches where a scene is destroyed: a variable in its registry's context, destroyed with the registry, records the
	// thread that destroyed it. The watch does not keep the scene alive.
	class SceneDestructionWatch
	{
	public:
		enum class Place : int
		{
			NotYet = 0,
			WatchingThread,   // The thread that made the watch (the main thread of a test)
			BackgroundThread, // A background thread of the job system (JobPriority::Background)
			OtherThread
		};

		explicit SceneDestructionWatch(Scene& scene)
			: m_State(std::make_shared<State>())
		{
			m_State->Watcher = std::this_thread::get_id();
			scene.GetRegistry().ctx().emplace<Witness>(m_State);
		}

		Place GetPlace() const { return static_cast<Place>(m_State->Where.load()); }
		// Waits until the scene was destroyed (up to `timeout`); returns where.
		Place WaitForDestruction(std::chrono::milliseconds timeout = std::chrono::milliseconds(10000)) const
		{
			const auto deadline = std::chrono::steady_clock::now() + timeout;
			while (GetPlace() == Place::NotYet && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return GetPlace();
		}
	private:
		struct State
		{
			std::atomic<int> Where = static_cast<int>(Place::NotYet);
			std::thread::id Watcher;
		};

		// Movable only: a witness the context moves leaves nothing behind that could report.
		struct Witness
		{
			std::shared_ptr<State> Shared;

			explicit Witness(std::shared_ptr<State> shared)
				: Shared(std::move(shared))
			{
			}

			Witness(Witness&& other) noexcept
				: Shared(std::move(other.Shared))
			{
			}

			Witness& operator=(Witness&& other) noexcept
			{
				Shared = std::move(other.Shared);
				return *this;
			}

			Witness(const Witness&) = delete;
			Witness& operator=(const Witness&) = delete;

			~Witness()
			{
				if (!Shared)
					return;
				Place place = Place::OtherThread;
				if (std::this_thread::get_id() == Shared->Watcher)
					place = Place::WatchingThread;
				else if (JobSystem::IsBackgroundThread())
					place = Place::BackgroundThread;
				Shared->Where = static_cast<int>(place);
			}
		};

		std::shared_ptr<State> m_State;
	};

	// A transform with a recognizable, non-trivial pose (rotation and non-uniform scale), chosen by `random`.
	inline void RandomizeTransform(Entity entity, std::mt19937& random)
	{
		std::uniform_real_distribution<float> offset(-5.0f, 5.0f);
		std::uniform_real_distribution<float> angle(-3.0f, 3.0f);
		std::uniform_real_distribution<float> scale(0.5f, 1.5f);
		TransformComponent& transform = entity.GetTransform();
		transform.Translation = glm::vec3(offset(random), offset(random), offset(random));
		transform.Rotation = glm::angleAxis(angle(random), glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f)));
		transform.Scale = glm::vec3(scale(random), scale(random), scale(random));
		entity.MarkModified<TransformComponent>();
	}

	// `count` entities with random transforms, each attached under a random earlier entity or left at the top level.
	inline std::vector<Entity> CreateRandomHierarchy(Scene& scene, size_t count, std::mt19937& random)
	{
		std::vector<Entity> entities;
		entities.reserve(count);
		for (size_t index = 0; index < count; index++)
		{
			Entity entity = scene.CreateEntity("Node" + std::to_string(index));
			RandomizeTransform(entity, random);
			if (!entities.empty() && std::uniform_int_distribution<int>(0, 3)(random) != 0)
			{
				const size_t parent = std::uniform_int_distribution<size_t>(0, entities.size() - 1)(random);
				REQUIRE(scene.SetParent(entity, entities[parent], false));
			}
			entities.push_back(entity);
		}
		return entities;
	}

}
