#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Math/AABB.h"
#include "Strata/Renderer/GlobalIlluminationLayout.h"
#include "Strata/Renderer/SceneRenderData.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace Strata
{

	class Mesh;
	class RenderScene;

	// What the voxel clipmap reads besides the render scene: the residency and tables of its GPU copy and the loaded meshes
	// and materials. The renderer's reads RenderSceneGPU and the scene's asset manager; tests give their own.
	class IVoxelSceneSource
	{
	public:
		virtual ~IVoxelSceneSource() = default;

		// Whether the proxy's record and instances are on the GPU (RenderSceneGPU::IsResident): only those are voxelized.
		virtual bool IsResident(uint32_t proxy) const = 0;
		// Changes whenever a mesh table entry or a material may have changed (RenderSceneGPU::GetTablesVersion).
		virtual uint64_t GetTablesVersion() const = 0;
		// The GPU's mesh table entry of a mesh slot (RenderSceneGPU::GetMeshTableEntry): 0 while its mesh is not loaded.
		virtual uint32_t GetMeshTableEntry(uint32_t meshSlot) const = 0;
		// The loaded mesh of a slot whose entry is not 0 (null otherwise).
		virtual Ref<const Mesh> GetMesh(uint32_t meshSlot) const = 0;
		// A material slot as the voxels take it (flags 0 while there is nothing to draw it with).
		virtual RenderData::GIMaterial GetMaterial(uint32_t materialSlot) const = 0;
	};

	struct VoxelClipmapSettings
	{
		// Triangles a frame voxelizes at most (at least one region a frame); 0: no limit (renderers of single images).
		uint32_t TrianglesPerFrame = RenderData::c_GIVoxelizeTrianglesPerFrame;
		// The same on a frame that rebuilds or jumps a window.
		uint32_t TrianglesOnCut = RenderData::c_GIVoxelizeTrianglesOnCut;
	};

	// Regions of one level the GPU voxelizes together: their voxels fit the layout's scratch (BatchVoxels).
	struct VoxelFrameBatch
	{
		uint32_t Level = 0;
		uint32_t FirstRegion = 0;
		uint32_t RegionCount = 0;
		uint32_t FirstGroup = 0; // Of the frame's triangle groups
		uint32_t GroupCount = 0;
		uint32_t Voxels = 0;     // Of its regions together
		uint32_t Columns = 0;    // Columns (x, z) of its regions together
	};

	// One frame's voxelization, as the GPU passes take it (Docs/DeveloperGuide.md, "Global illumination").
	struct VoxelFrame
	{
		std::vector<RenderData::VoxelRegion> Regions;
		std::vector<RenderData::VoxelWorkItem> Items;
		std::vector<RenderData::VoxelBatch> Groups;
		std::vector<VoxelFrameBatch> Batches;
		uint32_t Triangles = 0;            // Of the frame's work items
		uint32_t QueuedRegions = 0;        // Regions left for later frames
		uint32_t WaitingProxies = 0;       // Left out until they are resident with a loaded mesh
		uint32_t MovingProxies = 0;        // Left out while they move (placed anew within c_GIMovingFrames frames)
		// Per region taken: submeshes beyond c_VoxelItemMaxSubmesh, and items beyond the frame's limits (a closed mesh missing
		// any is voxelized open)
		uint32_t UnvoxelizedSubmeshes = 0;
		// The clipmap's levels 0-2 do not hold their windows (shrunk by two scroll steps) yet: a rebuild or a jump not done.
		bool Pending = false;
		bool Rebuilt = false;              // The frame started the clipmap over (its first, another render scene)
		bool Cut = false;                  // ...or a window moved by a whole window or more (a jump): the cut's budget
		bool MaterialsChanged = false;     // The material table changed (GetMaterials): upload it

		void Clear();
	};

	// The voxel clipmap of a renderer's global illumination, on the CPU (D3 and D5.1 of the world-space radiance cascades):
	// per level a window of the layout's dimensions around the camera, snapped to steps of c_GIScrollVoxels voxels and stored
	// toroidally (a world voxel at its index modulo the dimensions), a queue of regions to voxelize and the box of voxels that
	// are valid. Each frame (BuildFrame) it follows the camera (scrolling queues the slabs that entered; a move of a window or
	// more, or a renderer's first frame, queues the whole window) and the scene: the render scene's proxy log, the GPU copy's
	// tables (meshes and materials loaded, changed or dropped) and the proxies waiting for residency or moving, queuing the
	// boxes their voxels were made with and their new bounds. It then takes regions in order (changes, slabs, then whole
	// windows; level 0 first) while their triangles fit the frame's budget, at least one, and lists the work the GPU passes do
	// for them. Main thread only; never per proxy of the scene but for changes and the regions it takes.
	class VoxelClipmap
	{
	public:
		explicit VoxelClipmap(GlobalIlluminationLayout layout);

		const GlobalIlluminationLayout& GetLayout() const { return m_Layout; }

		void BuildFrame(const RenderScene& renderScene, const IVoxelSceneSource& source, const glm::dvec3& cameraPosition, const VoxelClipmapSettings& settings,
			VoxelFrame& outFrame);
		// The next BuildFrame starts over (its windows queued whole): a frame whose work never reached the GPU (its recording
		// failed) left the voxels without it.
		void Reset() { m_Started = false; }

		// A level as the frame left it, in world voxel indices.
		struct LevelState
		{
			glm::i64vec3 Window{ 0 }; // The window's minimum corner
			glm::i64vec3 Anchor{ 0 }; // The voxel regions and voxel coordinates are relative to
			VoxelBox Valid;           // The voxels that hold the scene (within the window)
		};
		const LevelState& GetLevel(uint32_t level) const { return m_Levels[level].State; }
		// The material table by material slot (RenderData::GIMaterial), as the voxelizer reads it.
		std::span<const RenderData::GIMaterial> GetMaterials() const { return m_Materials; }
		// The largest channel of the albedos of the submeshes voxelized since the clipmap started over (Reset), clamped below
		// c_GIMaxBounceAlbedo as re-injection clamps it: what each bounce keeps at most of the one before (the bounces' settling,
		// GlobalIllumination::GetSettleFrames). It never falls while the clipmap lives: a brighter material once voxelized stays.
		float GetBounceAlbedo() const { return std::min(static_cast<float>(m_LargestAlbedo) / 255.0f, RenderData::c_GIMaxBounceAlbedo); }
		// Frames built (BuildFrame), and the render scene the clipmap follows.
		uint64_t GetFrameCount() const { return m_Frame; }
		uint64_t GetRenderSceneId() const { return m_SceneId; }

		// The voxel box (relative to nothing: world voxel indices) a world box touches in a level: every voxel whose closed box
		// meets it, and one more around for the rounding of the GPU's float transforms.
		static VoxelBox GetTouchedVoxels(const AABB& bounds, float voxelSize);
	private:
		enum class RegionKind : uint8_t
		{
			Change = 0, // A proxy's old or new bounds
			Slab,       // Entered with a scroll: the level is not valid there until it is done
			Fill,       // A whole window after a rebuild or a jump: the level is not valid until all are done
			Refresh     // A whole window again whose voxels stay valid meanwhile (a new anchor, the proxy log overflowed)
		};
		static constexpr size_t c_RegionKinds = 4;

		struct Level
		{
			LevelState State;
			std::array<std::deque<VoxelBox>, c_RegionKinds> Queues;
		};

		enum class ProxyStatus : uint8_t
		{
			None = 0,   // Not in the voxels: not drawable as the voxels take it
			Voxelized,  // In the voxels, with Bounds
			Waiting,    // Drawable but not resident, or its mesh not loaded
			Moving      // Placed anew within c_GIMovingFrames frames: out of the voxels until it rests
		};

		struct ProxyState
		{
			AABB Bounds;              // The world bounds its voxels were made with (Voxelized)
			uint64_t Placement = 0;   // Hash of its world transform
			uint64_t Content = 0;     // Hash of its mesh slot, override material slot and flags
			uint64_t MovedFrame = 0;  // The frame it last moved (Moving)
			ProxyStatus Status = ProxyStatus::None;
		};

		struct MeshInfo
		{
			struct SubmeshInfo
			{
				AABB Bounds;
				std::vector<uint32_t> LODTriangles; // Per level of detail
				std::vector<float> LODErrors;       // MeshLOD::Error per level
				bool HasLODErrors = false;          // MeshUtils::HasLODErrors
			};
			uint32_t Entry = 0;  // The mesh table entry it was made for (0: not loaded)
			bool Closed = false;
			std::vector<SubmeshInfo> Submeshes;
		};

		// A region the frame takes, with its work.
		struct TakenRegion
		{
			uint32_t Level = 0;
			VoxelBox Box;
			std::vector<RenderData::VoxelWorkItem> Items; // Region index not set yet
			std::vector<uint32_t> ItemTriangles;
			std::vector<uint32_t> ItemProxies;            // A proxy's items are together
			uint32_t Triangles = 0;
		};

		void StartOver(const RenderScene& renderScene, const IVoxelSceneSource& source, const glm::dvec3& cameraPosition);
		void FollowCamera(const glm::dvec3& cameraPosition, bool& outCut);
		void FollowTables(const RenderScene& renderScene, const IVoxelSceneSource& source, VoxelFrame& outFrame);
		void FollowProxies(const RenderScene& renderScene, const IVoxelSceneSource& source);
		void Rescan(const RenderScene& renderScene, const IVoxelSceneSource& source, bool queueWindows);

		bool IsDrawable(const RenderScene& renderScene, uint32_t proxy) const;
		bool IsReady(const RenderScene& renderScene, const IVoxelSceneSource& source, uint32_t proxy) const;
		uint64_t GetPlacementHash(const RenderScene& renderScene, uint32_t proxy) const;
		uint64_t GetContentHash(const RenderScene& renderScene, uint32_t proxy) const;
		// Puts a proxy that is in none of the voxels (status None or Waiting) in: queues its bounds when it is ready (with
		// `queue`), else lets it wait.
		void Admit(const RenderScene& renderScene, const IVoxelSceneSource& source, uint32_t proxy, bool queue);
		// Takes a proxy out of the voxels (queuing the bounds they were made with) and puts it in again as it is now.
		void Refresh(const RenderScene& renderScene, const IVoxelSceneSource& source, uint32_t proxy);
		void QueueBounds(const AABB& bounds);
		void QueueWindow(uint32_t level, RegionKind kind);

		bool TakeRegion(const RenderScene& renderScene, uint32_t level, const VoxelBox& box, TakenRegion& outRegion, uint32_t& outUnvoxelized);
		void EmitFrame(std::vector<TakenRegion>& taken, VoxelFrame& outFrame) const;
	private:
		GlobalIlluminationLayout m_Layout;
		std::vector<Level> m_Levels;
		uint64_t m_Frame = 0;
		uint64_t m_SceneId = 0;
		bool m_Started = false;
		uint64_t m_ProxyVersion = 0;
		uint64_t m_TablesVersion = 0;
		std::vector<ProxyState> m_Proxies;
		std::vector<uint32_t> m_Waiting; // Proxies with status Waiting (or that had it: checked as they are read)
		std::vector<uint32_t> m_Moving;  // Likewise for Moving
		std::vector<MeshInfo> m_Meshes;  // By mesh slot
		std::vector<RenderData::GIMaterial> m_Materials; // By material slot
		uint32_t m_LargestAlbedo = 0;                    // 0-255: GetBounceAlbedo
		std::vector<uint32_t> m_Scratch;
		std::vector<uint32_t> m_Candidates;
		std::vector<uint32_t> m_Below;
	};

}
