#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SubScene.h"
#include "Strata/World/WorldGrid.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Strata
{

	class Scene;
	class WorldAsset;

	// A point cells stream around: a StreamingSourceComponent, the primary camera, the editor camera or a request
	// (IWorldStreaming::RequestCellsAround).
	struct WorldStreamingSource
	{
		glm::vec3 Position { 0.0f };
		glm::vec3 Forward { 0.0f, 0.0f, -1.0f }; // Cells ahead rank nearer (ViewBias); zero: no direction
		float Radius = 0.0f;                     // 0: each layer's StreamingDistance times the world's DistanceScale
		uint32_t LayerMask = ~0u;                // Bit i: the world's layer i (in WorldData::Layers order) streams
		int32_t Priority = 0;                    // Higher-priority sources' cells load first
		float ViewBias = 0.25f;                  // Cells ahead rank as if this share of the radius nearer (ordering only)
	};

	enum class WorldCellStatus : uint8_t
	{
		Unloaded = 0,
		Loading,   // Its scene loads, or its entities are created
		Loaded,
		Unloading, // Its entities go a slice per update
		Failed     // Its load failed (Error says why); not retried until its scene changes
	};

	const char* WorldCellStatusToString(WorldCellStatus status);

	// One cell of the world as the streamer sees it (WorldStreamer::GetCellStates: the cell map).
	struct WorldCellState
	{
		WorldCellKey Key;
		AssetHandle Scene = UUID::Null();
		WorldCellStatus Status = WorldCellStatus::Unloaded;
		SubSceneID SubScene = 0;
		bool Wanted = false; // Within a source's radius, or pinned
		bool Pinned = false;
		uint64_t Entities = 0; // Created so far (Loading, Loaded), left (Unloading)
		std::string Error;
	};

	// What an update decided (WorldStreamer::GetLastDecisions): tools and tests compare drives through them.
	struct WorldStreamingDecision
	{
		enum class Action : uint8_t
		{
			Load = 0,
			Unload // Also the cancellation of a load
		};

		Action Kind = Action::Load;
		WorldCellKey Key;
	};

	// Streams a world's cells into a scene as sub-scenes (Scene::LoadSubScene, UnloadSubScene) around its sources: the core
	// that the World scene system (Play, Simulate) and the editor (Edit) drive. Main thread.
	//
	// Each update it takes the World asset from the scene's asset manager (requested every update, never held), finds the
	// wanted cells (per source and enabled layer, the existing cells whose squares lie within the source's radius; pinned
	// cells always), keeps loaded ones while they lie within the radius plus a hysteresis H = max(CellSize / 2,
	// 0.1 * radius), unloads (sliced) or cancels the others, and starts loads in a total order (priority descending,
	// view-biased distance ascending, packed key ascending), at most c_MaxConcurrentCellLoads at once. With a cooked index
	// it first reserves physics room (IPhysicsCapacity::Reserve) for the cells loaded, loading and starting. The sub-scenes
	// advance in the scene's own updates (Scene::UpdateSubScenes, within the frame's SceneWorkBudget). Decisions depend only
	// on the inputs and the cells' states, so drives that see the same states decide alike whatever the thread count. When
	// no source moved by more than c_SourceEpsilon, nothing was pinned or toggled and no load or unload finished, an update
	// costs a check of each source and of the cells in flight.
	class WorldStreamer
	{
	public:
		static constexpr uint32_t c_MaxConcurrentCellLoads = 4;
		static constexpr float c_SourceEpsilon = 0.01f;
		static constexpr float c_DefaultViewBias = 0.25f;

		explicit WorldStreamer(Scene& scene);
		~WorldStreamer(); // Leaves the cells as they are (UnloadAll unloads them)

		WorldStreamer(const WorldStreamer&) = delete;
		WorldStreamer& operator=(const WorldStreamer&) = delete;

		// The world it streams and its DistanceScale (clamped to 0.1-10). Another world unloads the cells of the previous one.
		void SetWorld(AssetHandle world, float distanceScale = 1.0f);
		AssetHandle GetWorld() const { return m_World; }
		float GetDistanceScale() const { return m_DistanceScale; }
		// The World asset, once it is loaded (null before, and for a handle that is not a world).
		Ref<WorldAsset> GetWorldAsset() const;

		// This update's sources (the owner gathers them every update).
		void SetSources(std::span<const WorldStreamingSource> sources);
		const std::vector<WorldStreamingSource>& GetSources() const { return m_Sources; }

		// Layers by name, starting from their EnabledByDefault: a disabled layer's cells unload. Set before the world is
		// loaded, a name is remembered and applied when it is. SetLayerEnabled returns false for a name the loaded world does
		// not have; IsLayerEnabled returns nullopt for one, and before the world is loaded for names never set.
		bool SetLayerEnabled(std::string_view layer, bool enabled);
		std::optional<bool> IsLayerEnabled(std::string_view layer) const;

		// Cells kept loaded whatever the sources do (dirty or selected cells in the editor, requests), counted: each PinCell
		// needs its UnpinCell.
		void PinCell(const WorldCellKey& key);
		void UnpinCell(const WorldCellKey& key);
		bool IsPinned(const WorldCellKey& key) const;

		// Takes over the sub-scenes of the scene whose sources are cells of the world (a prepared start's, the cells the
		// editor adopted into a play copy), so that they are streamed like the ones it loads.
		void AdoptLoadedCells();

		void Update();

		// Whether every existing cell of every enabled layer within `radius` of `position` is loaded (true for a region
		// without cells; false before the world is loaded).
		bool IsLoadedAround(const glm::vec3& position, float radius) const;
		// The loads, unloads and cancellations the last update started, in the order it started them.
		const std::vector<WorldStreamingDecision>& GetLastDecisions() const { return m_Decisions; }
		WorldStreamingStats GetStats() const;
		// Every cell of the world with its state (empty before the world is loaded).
		void GetCellStates(std::vector<WorldCellState>& outStates) const;
		// The sub-scene of a cell, 0 when it has none (unloaded, failed).
		SubSceneID GetCellSubScene(const WorldCellKey& key) const;

		// Unloads (sliced) or cancels every cell the streamer loaded or adopted, and forgets them.
		void UnloadAll();
	private:
		struct CellRecord
		{
			WorldCellStatus Status = WorldCellStatus::Unloaded;
			SubSceneID SubScene = 0;
			AssetHandle Scene = UUID::Null();
			size_t CreatedSeen = 0;   // The sub-scene's CreatedEntities at the last update
			size_t DestroyedSeen = 0; // Its DestroyedEntities at the last update (Unloading)
			size_t EntitiesAtUnload = 0;
			uint64_t FailedRevision = 0; // Failed: the source revision of its scene then (GetSourceRevisions)
			std::string Error;
		};

		// What an evaluation found for a cell: its best rank among the sources that want it.
		struct Candidate
		{
			uint64_t Key = 0;
			int32_t Priority = 0;
			double Distance = 0.0; // View-biased
			bool High = false;     // Within half the radius of a source that wants it
		};

		bool RefreshWorld(const Ref<WorldAsset>& world);
		bool PollCellsInFlight();
		bool RetryChangedFailures();
		bool SourcesMoved() const;
		void AdoptCells(const WorldAsset& world);
		void Evaluate(const WorldAsset& world);
		void StartLoad(const WorldAsset& world, const Candidate& candidate);
		void Unload(uint64_t key, CellRecord& record);
		void ReserveBodies(const WorldAsset& world, std::span<const Candidate> starting);
		bool IsLayerOn(size_t layerIndex) const;
		float GetRadius(const WorldStreamingSource& source, size_t layerIndex) const;

		Scene& m_Scene;
		AssetHandle m_World = UUID::Null();
		float m_DistanceScale = 1.0f;
		std::weak_ptr<WorldAsset> m_SeenWorld; // The World asset object the tables below were built from
		std::vector<std::vector<int32_t>> m_LayerCells; // Per layer index: its cells (indexes into WorldData::Cells)
		std::vector<bool> m_LayerEnabled;
		std::vector<std::pair<std::string, bool>> m_LayerOverrides; // Set by name, applied to every world loaded later
		std::unordered_map<uint64_t, uint32_t> m_Pins;
		std::vector<WorldStreamingSource> m_Sources;
		std::vector<WorldStreamingSource> m_EvaluatedSources; // The sources of the last evaluation
		std::unordered_map<uint64_t, CellRecord> m_Cells; // Cells that are not unloaded (or failed)
		std::vector<uint64_t> m_InFlight;                 // Keys of the Loading and Unloading cells
		std::unordered_set<uint64_t> m_Wanted;            // The last evaluation's wanted cells
		bool m_Dirty = true;
		bool m_AdoptPending = false;   // AdoptLoadedCells was called before the world was loaded
		uint32_t m_FailedCells = 0;    // Records in state Failed
		uint64_t m_FailureVersion = 0; // The asset manager's content version when failures were last checked

		std::vector<WorldStreamingDecision> m_Decisions;
		WorldStreamingStats m_Stats;
		std::vector<Candidate> m_Candidates;                // Scratch of Evaluate
		std::unordered_map<uint64_t, size_t> m_CandidateIndex; // Scratch of Evaluate: key -> index into m_Candidates
		std::unordered_set<uint64_t> m_Keep;                 // Scratch of Evaluate: cells within a source's radius plus H
		std::vector<uint64_t> m_Keys;                        // Scratch
	};

}
