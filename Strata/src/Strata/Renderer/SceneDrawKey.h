#pragma once

#include "Strata/Renderer/SceneRenderData.h"

#include <cstdint>

namespace Strata
{

	// The sort key of a CPU-path draw, which SceneRenderer::BuildBatches decodes: pipeline state first (cascade, alpha mask,
	// winding, culling, skinning), then mesh, submesh and level of detail, which together make a batch; the material in the
	// lowest bits only when batches are split by material. The view's draws, the sun's cascades and the point and spot
	// lights' views (cascade 0: each view keeps its own list) share the layout.
	namespace SceneDrawKey
	{
		constexpr uint32_t c_CascadeShift = 61; // Three bits: RenderData::c_MaxCascades
		constexpr uint32_t c_AlphaMaskShift = 60;
		constexpr uint32_t c_MirroredShift = 59;
		constexpr uint32_t c_DoubleSidedShift = 58;
		constexpr uint32_t c_SkinnedShift = 57;
		constexpr uint32_t c_MeshShift = 37;
		constexpr uint32_t c_SubmeshShift = 21;
		constexpr uint32_t c_LODShift = 16;
		constexpr uint32_t c_MeshBits = 20;     // Mesh slots: at most one per entity (Scene::c_MaxEntities)
		constexpr uint32_t c_SubmeshBits = 16;
		constexpr uint32_t c_LODBits = 5;
		constexpr uint32_t c_MaterialBits = 16; // Material slots beyond this share keys: their batches merge, which is still correct
		constexpr uint32_t c_MaxSubmeshes = 1u << c_SubmeshBits;
		constexpr uint32_t c_MaxLODs = 1u << c_LODBits;
		static_assert(RenderData::c_MaxCascades <= (1u << (64 - c_CascadeShift)), "The key's cascade bits hold every cascade");
		static_assert(c_MeshShift + c_MeshBits == c_SkinnedShift && c_SubmeshShift + c_SubmeshBits == c_MeshShift
			&& c_LODShift + c_LODBits == c_SubmeshShift && c_MaterialBits == c_LODShift, "The key's fields tile its 64 bits");

		inline uint64_t Make(uint32_t cascade, bool alphaMask, bool mirrored, bool doubleSided, bool skinned, uint32_t meshSlot, uint32_t submesh, uint32_t lod,
			uint32_t material)
		{
			return (static_cast<uint64_t>(cascade) << c_CascadeShift) | (static_cast<uint64_t>(alphaMask) << c_AlphaMaskShift)
				| (static_cast<uint64_t>(mirrored) << c_MirroredShift) | (static_cast<uint64_t>(doubleSided) << c_DoubleSidedShift)
				| (static_cast<uint64_t>(skinned) << c_SkinnedShift) | (static_cast<uint64_t>(meshSlot) << c_MeshShift)
				| (static_cast<uint64_t>(submesh) << c_SubmeshShift) | (static_cast<uint64_t>(lod) << c_LODShift)
				| static_cast<uint64_t>(material & ((1u << c_MaterialBits) - 1));
		}
	}

}
