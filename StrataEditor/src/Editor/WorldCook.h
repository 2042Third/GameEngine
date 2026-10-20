#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/Base.h>
#include <Strata/Core/Crypto.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/World/WorldAsset.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class EditorAssetManager;
	struct WorldCookPersistentRead; // WorldCook.cpp

	// What the cook keeps of the cells it read in an editor session, by cell scene: the summary and the entity IDs of the
	// stamp it read, so that a later cook re-reads only cells that changed and still finds IDs a changed cell shares with an
	// unchanged one. Main thread (the cook copies what its jobs need).
	struct WorldCookCache
	{
		struct Cell
		{
			uint64_t Stamp = 0;
			WorldCellSummary Summary;
			std::vector<UUID> EntityIDs; // Sorted
		};
		std::unordered_map<UUID, Cell> Cells;
	};

	// What a cook found (world.cook, the export).
	struct WorldCookReport
	{
		WorldIndex Index;              // Fresh: every cell's summary, the persistent scene's bodies, MaxBodiesInRadius
		uint32_t CellsRead = 0;        // Cells whose documents were read (their stamps changed, or nothing was known)
		uint32_t CellsReused = 0;      // Cells whose summaries were still current
		uint32_t SourceCount = 1;      // Streaming sources of the persistent scene (at least 1)
		uint32_t RecommendedMaxBodies = 0; // WorldIndex::GetRecommendedMaxBodies
		uint64_t ReservedBytes = 0;    // What a physics world of that capacity reserves (PhysicsWorld::EstimateReservedBytes)
		std::vector<std::string> Warnings; // Cells beyond the supported extent, empty cells
	};

	// Measures a world's cells for its index, off the frame (world.cook, the export): reads every cell document whose
	// content stamp (EditorAssetManager::GetContentStamp) differs from its index entry's (and from what the session's cache
	// holds) on I/O jobs (WorldCellIO::ReadCellSummary), and computes, per cell, the entities, the physics bodies
	// (PhysicsWorld::CountBodyCandidates: the rule physics sizes by), the bounds (entities' positions, renderers' recorded mesh
	// bounds, colliders) and the stored bytes of its dependency closure without other scenes; then the most bodies the
	// persistent scene's streaming sources can keep loaded anywhere (MaxBodiesInRadius, see ComputeMaxBodiesInRadius). Entity
	// IDs shared by cells or with the persistent scene fail the cook, naming them. The cook shares the asset manager and the
	// cache with the editor, so that both stay alive while it runs even when the project closes meanwhile. Main thread, except
	// its jobs.
	class WorldCook
	{
	public:
		// Starts a cook of a world of the project. Null with the reason for a handle that is not a world of the project or a
		// world document that cannot be read.
		static Scope<WorldCook> Start(const Ref<EditorAssetManager>& assets, AssetHandle world, const Ref<WorldCookCache>& cache,
			std::string* outError = nullptr);
		// Cooks at once, waiting for the reads (tools and tests). Nullopt with the reason when it fails.
		static std::optional<WorldCookReport> Run(const Ref<EditorAssetManager>& assets, AssetHandle world, const Ref<WorldCookCache>& cache,
			std::string* outError = nullptr);
		~WorldCook();

		WorldCook(const WorldCook&) = delete;
		WorldCook& operator=(const WorldCook&) = delete;

		// Once per frame: true once the cook finished (successfully or not). Main thread.
		bool Poll();
		// Stops the cook (its project closed): reads that have not started are skipped, and Poll finishes, failed, once the
		// running ones end, leaving the cache as it was. Main thread.
		void Cancel();
		bool Succeeded() const { return m_Finished && m_Error.empty(); }
		const std::string& GetError() const { return m_Error; }
		const WorldCookReport& GetReport() const { return m_Report; }
		AssetHandle GetWorld() const { return m_World; }
		// The World document with the fresh index (once Succeeded).
		const WorldData& GetCookedData() const { return m_Data; }
		// Whether the World document still holds the bytes the cook started from. False once something wrote it meanwhile
		// (a save that listed new cells, a change of layers) or it cannot be read: a document made from the cook's reading
		// would undo that write, so the cook starts again instead. Main thread.
		bool IsSourceCurrent() const;

		// The most bodies the sources can keep loaded at once: for each layer, the radius the sources use (the largest
		// StreamingSourceComponent::Radius, else the layer's StreamingDistance times DistanceScale) plus its hysteresis; for
		// each cell, the bodies of the cells of its layer whose squares lie within that radius of any point of it (a bound on
		// what a source anywhere in the cell keeps loaded); the largest such sum per layer, summed over the layers, times the
		// number of sources (at least 1), plus the persistent scene's bodies. `layerRadii` holds the radius per layer (in
		// WorldData::Layers order) before the hysteresis.
		static uint64_t ComputeMaxBodiesInRadius(const WorldData& data, std::span<const WorldCellSummary> cells, std::span<const double> layerRadii,
			uint32_t sourceCount, uint64_t persistentBodies);
	private:
		WorldCook() = default;

		struct CellJob
		{
			WorldCell Cell;
			uint64_t Stamp = 0;
			std::optional<WorldCellSummary> Summary;
			std::vector<UUID> EntityIDs;
			std::string Error;
		};

		void Finish();

		Ref<EditorAssetManager> m_Assets; // The jobs read through it (the destructor waits for them)
		Ref<WorldCookCache> m_Cache;
		AssetHandle m_World = UUID::Null();
		WorldData m_Data;
		Sha256Digest m_SourceHash {}; // Of the World document the cook started from
		std::vector<Scope<CellJob>> m_Reads; // The cells read by jobs
		std::vector<JobHandle> m_Jobs;
		Ref<WorldCookPersistentRead> m_Persistent; // Shared with its job
		std::vector<double> m_LayerRadii;
		std::atomic<bool> m_Cancelled = false;
		bool m_Finished = false;
		std::string m_Error;
		WorldCookReport m_Report;
	};

}
