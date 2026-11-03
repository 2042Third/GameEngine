#pragma once

#include "Renderer/SceneRendererTestUtils.h"

#include <glm/ext/vector_int3_sized.hpp>

#include <cstdint>
#include <vector>

// The scenes indirect light is measured on (DeveloperGuide "Global illumination"): a yard with a hut, a fence and a field of
// grass cards, and the look-dev room, with the camera paths that strafe them. The look-dev project mirrors them
// (Tools/LookDev/LookDevScene.json, scenes Yard and Grass): change both.
namespace Strata::Tests
{

	struct YardOptions
	{
		bool Fence = true;        // 34 posts and two rails 17 m long in the strafe's foreground
		uint32_t GrassCards = 0;  // Alpha-masked cards of blades in front of the fence (the grass scene: 500)
		float LanternPower = 1.0f; // Scales the two lanterns' emission
	};

	// The yard (the audit's CPU model's scene, in meters, y up): 60 x 60 m of ground, a plaster hut 6 x 5 m with a door, a
	// window and a roof, barrels, crates, a covered car, boulders, the fence and two lanterns, under a shadowed sun and a
	// procedural sky; with AddNeutralPostProcess (no ambient occlusion, no multi-bounce: the measured light is the indirect
	// light alone).
	void BuildYard(Scene& scene, SceneTestAssets& assets, const YardOptions& options = {});

	// The look-dev room (LookDevScene.json's Room entities on its ground): plaster walls, floor and ceiling with a window to
	// the south, a box, a ball and a shadowed point lamp, under the look-dev scene's sun and atmosphere; neutral post-processing.
	void BuildRoom(Scene& scene, SceneTestAssets& assets);

	// The fence strafe: from (-3.5, 1.7, -9.5) toward (2.5, 1.0, -0.5), both moving +x by 0.02 m times `speed` a frame, a
	// vertical field of view of 55 degrees.
	SceneCamera YardStrafe(uint32_t frame, float speed, float aspectRatio);

	// Inside the room: from (-9, 1.5, -4.5) toward (-10, 1.2, 1.1), moving +x by 0.01 m a frame, 70 degrees.
	SceneCamera RoomStrafe(uint32_t frame, float aspectRatio);

	// The voxels a mesh's full-detail triangles mark in a level of global illumination's clipmap, as the voxelizer finds them
	// (Include/Voxelize.slang: the voxel coordinates relative to the anchor, GlobalIllumination::GetVoxelCoordinate, and
	// Akenine-Moller's triangle-box test, touching included): world voxel indices, sorted, each once.
	std::vector<glm::i64vec3> ReferenceVoxels(const Mesh& mesh, const glm::mat4& world, const glm::i64vec3& anchorVoxel, float voxelSize);
	// Include/Voxelize.slang's triangle-box test (Akenine-Moller's, touching included), on the CPU.
	bool TriangleBoxOverlap(const glm::vec3& center, const glm::vec3& halfSize, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c);

}
