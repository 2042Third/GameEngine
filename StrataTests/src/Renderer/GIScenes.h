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

	// The look-dev room's shell lit by the sun alone, for bounce light measured against a reference: the Room Floor, Ceiling and
	// Wall entities of LookDevScene.json on its ground (its box, ball and lamp left out; Tools/LookDev/LookDevScene.json's scenes
	// SunRoom and SealedRoom mirror it: change both), under a sun whose light enters through the south window and lights a patch
	// of the floor, and no sky. Every light on the room's surfaces then comes from that patch, the window's sunlit sill and
	// reveals and the sunlit ground the window shows, which SunRoomReference integrates exactly.
	struct SunRoomOptions
	{
		glm::vec3 SunDirection = glm::vec3(0.0f, -1.0f, 0.0f); // The direction the sunlight travels (SunRoomDirection)
		float SunIlluminance = 100000.0f;                      // Lux, as the look-dev sun
		glm::vec3 FloorAlbedo = glm::vec3(0.58f);              // The look-dev plaster
		bool SealWindow = false;                               // A 0.2 m plaster box fills the window
		glm::vec3 Offset = glm::vec3(0.0f);                    // Moves the room (not the ground): probe planes cut its walls elsewhere
		bool PairedRoom = false;                               // A sealed room B east of it, sharing its east wall
		glm::vec3 AmbientColor = glm::vec3(0.0f);              // A constant sky of this luminance (nits) when not zero
	};

	// The direction a sun light rotated by (pitch, yaw, 0) degrees sends its light, as the look-dev scene rotates its sun
	// (-40, 30, 0).
	glm::vec3 SunRoomDirection(float pitch, float yaw);
	// SunIlluminance 0 builds no sun.
	void BuildSunRoom(Scene& scene, SceneTestAssets& assets, const SunRoomOptions& options);
	// The reference for the room's bounce light at a point of a surface with the given normal, independent of the renderer: the
	// irradiance over pi (what a white diffuse surface there reflects) from the sunlit surfaces it sees, by numeric integration
	// over `strata` x `strata` cosine-weighted directions traced against the room's boxes (each hit's radiance its albedo times
	// the sun's irradiance there, if the sun reaches it, over pi). One bounce: surfaces the sun does not reach send nothing.
	glm::vec3 SunRoomReference(const SunRoomOptions& options, const glm::vec3& point, const glm::vec3& normal, uint32_t strata = 128);
	// The reference's radiance of a surface point the sun lights directly (albedo times the sun's irradiance over pi, 0 in its
	// shadow): what the lit image shows of a diffuse surface there.
	glm::vec3 SunRoomRadiance(const SunRoomOptions& options, const glm::vec3& point, const glm::vec3& normal);
	// The power of the room floor's sunlit patch: the sun's radiance of the floor's top integrated over it (radiance times area,
	// sampled every `step` meters), for telling a miss of the geometry (the window's opening as voxels) from one of the probes.
	float SunRoomPatchPower(const SunRoomOptions& options, float step = 0.05f);

	// The fence strafe: from (-3.5, 1.7, -9.5) toward (2.5, 1.0, -0.5), both moving +x by 0.02 m times `speed` a frame, a
	// vertical field of view of 55 degrees.
	SceneCamera YardStrafe(uint32_t frame, float speed, float aspectRatio);

	// Inside the room: from (-9, 1.5, -4.5) toward (-10, 1.2, 1.1), moving +x by 0.01 m a frame, 70 degrees.
	SceneCamera RoomStrafe(uint32_t frame, float aspectRatio);

	// The voxels a mesh's full-detail triangles mark in a level of global illumination's clipmap, as the voxelizer finds them
	// (Include/Voxelize.slang: the voxel coordinates relative to the anchor, GlobalIllumination::GetVoxelCoordinate, and
	// Akenine-Moller's triangle-box test, touching included): world voxel indices, sorted, each once. `outFacing`, when given,
	// receives those of them some triangle faces (not wholly behind its plane, IsVoxelBehind): the voxels that get a normal.
	// `outSolid`, when given, receives those some triangle reaches into or touches from behind (not wholly in front of its plane,
	// IsVoxelInFront): the others, only touched by faces in front of them, take c_GITouchOccupancy.
	std::vector<glm::i64vec3> ReferenceVoxels(const Mesh& mesh, const glm::mat4& world, const glm::i64vec3& anchorVoxel, float voxelSize,
		std::vector<glm::i64vec3>* outFacing = nullptr, std::vector<glm::i64vec3>* outSolid = nullptr);
	// Include/Voxelize.slang's triangle-box test (Akenine-Moller's, touching included), on the CPU.
	bool TriangleBoxOverlap(const glm::vec3& center, const glm::vec3& halfSize, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c);

}
