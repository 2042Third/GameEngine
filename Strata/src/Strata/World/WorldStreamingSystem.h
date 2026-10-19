#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneSystem.h"
#include "Strata/World/WorldStreamer.h"

#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Strata
{

	class Scene;
	struct SceneWorkBudget;

	// The "World" scene system (Play and Simulate): streams the world of the scene's WorldComponent around its
	// StreamingSourceComponents (else the primary camera) and the requests scripts make, through a WorldStreamer, and offers
	// IWorldStreaming. A scene without a WorldComponent costs it a check of one component storage per update.
	class WorldStreamingSystem final : public SceneSystem, public IWorldStreaming
	{
	public:
		static constexpr int32_t c_RequestPriority = 100;

		explicit WorldStreamingSystem(Scene& scene);
		~WorldStreamingSystem() override;

		WorldStreamingSystem(const WorldStreamingSystem&) = delete;
		WorldStreamingSystem& operator=(const WorldStreamingSystem&) = delete;

		void OnRuntimeStart() override;
		void OnRuntimeStop() override;
		void OnUpdate(Timestep timestep) override;

		uint64_t RequestCellsAround(const glm::vec3& position, float radius) override;
		bool ReleaseCells(uint64_t request) override;
		bool IsLoadedAround(const glm::vec3& position, float radius) const override;
		bool SetLayerEnabled(std::string_view layer, bool enabled) override;
		std::optional<bool> IsLayerEnabled(std::string_view layer) const override;
		WorldStreamingStats GetStats() const override;
		bool IsStreaming() const override { return m_Streamer != nullptr; }

		// The streamer, while the scene streams a world (null otherwise): the cell map reads its cell states.
		const WorldStreamer* GetStreamer() const { return m_Streamer.get(); }
		// Keeps cells loaded for the session whatever the sources do (the editor's play copy pins the cells with unsaved edits,
		// whose saved documents the game would read otherwise). False while the scene streams no world.
		bool PinCell(const WorldCellKey& key);
		// For tools and tests that start a world without preparing it (Scene::OnRuntimeStart directly): advances the asset
		// manager, the scene's sub-scenes and the streaming until the cells of the LoadBeforeStart layers around the sources
		// are loaded or failed. False at the timeout, or when the scene streams no world.
		bool WaitForStartCells(std::chrono::milliseconds timeout);

		// The World module's preparation of a loaded scene (SceneSystemDescriptor::PrepareLoadedScene): in a scene with a
		// WorldComponent, loads the cells of the LoadBeforeStart layers around its sources (within the frame's budget, the
		// sub-scenes advancing with Scene::UpdateSubScenes) and returns true once they are loaded or failed. Physics'
		// preparation then builds their bodies, so a world starts with its ground in place.
		static bool PrepareLoadedScene(Scene& scene, SceneWorkBudget& budget);
	private:
		// The scene's world (its first active WorldComponent in hierarchy order), and the sources around which it streams.
		static Entity FindWorldEntity(Scene& scene);
		static void GatherSources(Scene& scene, const WorldStreamer& streamer, std::vector<WorldStreamingSource>& outSources, std::set<std::string>* warnedLayers);
		// Whether the cells of the LoadBeforeStart layers the last evaluation wanted are all loaded or failed.
		static bool AreStartCellsDone(const WorldStreamer& streamer);
		void SyncWorld();

		Scene& m_Scene;
		Scope<WorldStreamer> m_Streamer;
		std::vector<WorldStreamingSource> m_Sources;
		std::map<uint64_t, WorldStreamingSource> m_Requests; // Request ID -> its source, in the order made
		uint64_t m_NextRequest = 1;
		std::set<std::string> m_WarnedLayers; // Unknown names of StreamingSourceComponent::Layers, warned about once
	};

}
