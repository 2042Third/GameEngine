#pragma once

#include "Strata/Renderer/BufferReadback.h"
#include "Strata/Renderer/GlobalIlluminationLayout.h"
#include "Strata/Renderer/SceneRenderData.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Renderer/VoxelClipmap.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <vector>

// A scene renderer's global illumination (SceneRendererGlobalIllumination.cpp): what SceneRenderer keeps of it, made by the first
// frame that wants it and released whole by the first that does not.
namespace Strata
{

	struct SceneRenderer::GlobalIlluminationResources
	{
		GlobalIlluminationLayout Layout;
		Scope<VoxelClipmap> Clipmap;
		VoxelFrame Frame;                            // The latest frame's voxelization
		// The latest frame that built its voxelization (Prepare) recorded it: a frame that did not (it failed after its
		// extraction) makes the next start the clipmap over.
		bool Recorded = true;
		bool UnvoxelizedLogged = false;              // GIUnvoxelizedSubmeshes was logged (once while the resources live)
		RenderData::GlobalIlluminationConstants Constants = {};
		// Per voxel level: the material (albedo and occupancy), emission and normal textures, 3D, toroidal.
		struct Level
		{
			nvrhi::TextureHandle Material;
			nvrhi::TextureHandle Emission;
			nvrhi::TextureHandle Normal;
		};
		std::vector<Level> Levels;
		bool Cleared = false;                        // The textures were cleared (their first frame)
		nvrhi::BufferHandle ConstantBuffer;          // RenderData::GlobalIlluminationConstants (b1)
		nvrhi::BufferHandle Scratch;                 // A batch's eight words per voxel
		nvrhi::BufferHandle Tiles;                   // The large triangles' tiles of a batch
		nvrhi::BufferHandle TileArguments;           // VoxelTiles' dispatch arguments
		nvrhi::BufferHandle Counters;                // Tiles made, tile list overflows
		nvrhi::BufferHandle Regions;                 // The frame's VoxelRegion records
		nvrhi::BufferHandle Items;                   // VoxelWorkItem
		nvrhi::BufferHandle Groups;                  // VoxelBatch
		nvrhi::BufferHandle Materials;               // GIMaterial per material slot
		uint32_t MaterialCapacity = 0;
		nvrhi::SamplerHandle RepeatSampler;
		// The voxelizer: one layout for its passes but the tiles', which has no dispatch arguments (it reads them indirectly), and
		// a binding set of each per level (the level's textures), made again when the buffers they name change.
		nvrhi::BindingLayoutHandle VoxelLayout;
		nvrhi::BindingLayoutHandle TilesLayout;
		nvrhi::ComputePipelineHandle TrianglesPipeline;
		nvrhi::ComputePipelineHandle TilesPipeline;
		nvrhi::ComputePipelineHandle FillPipeline;
		nvrhi::ComputePipelineHandle FinalizePipeline;
		std::vector<nvrhi::BindingSetHandle> VoxelSets;
		std::vector<nvrhi::BindingSetHandle> TilesSets;
		const RenderSceneGPU* BoundScene = nullptr;
		uint64_t BoundSceneBuffers = 0;
		uint64_t BoundPoolBuffers = 0;
		uint64_t BoundPoolTables = 0;
		const nvrhi::IBuffer* BoundMaterials = nullptr;
		// The voxels view.
		nvrhi::BindingLayoutHandle ViewLayout;
		nvrhi::GraphicsPipelineHandle ViewPipeline;
		nvrhi::BindingSetHandle ViewSet;
		// The counters' copies, each with the frame it copied (RenderCount + 1).
		struct CounterReadback
		{
			Scope<BufferReadback> Readback;
			uint64_t Render = 0;
		};
		std::array<CounterReadback, 3> Readbacks;
		uint32_t ReadbackIndex = 0;
		bool ReadbackRecorded = false;
		uint64_t MemoryBytes = 0;
	};

}
