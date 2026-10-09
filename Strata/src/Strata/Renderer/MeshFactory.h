#pragma once

#include "Strata/Renderer/Mesh.h"

namespace Strata
{

	// Procedural primitive meshes. All are centered at the origin, use counter-clockwise front faces, carry normals,
	// tangents and texture coordinates, and have a single submesh using the given material.
	class MeshFactory
	{
	public:
		static Ref<Mesh> CreateCube(float size = 1.0f, AssetHandle material = UUID::Null());
		static Ref<Mesh> CreateSphere(float radius = 0.5f, uint32_t segments = 48, uint32_t rings = 24, AssetHandle material = UUID::Null());
		static Ref<Mesh> CreatePlane(float size = 1.0f, uint32_t subdivisions = 1, AssetHandle material = UUID::Null()); // XZ plane, facing +Y
		static Ref<Mesh> CreateQuad(float size = 1.0f, AssetHandle material = UUID::Null());                               // XY plane, facing +Z
		static Ref<Mesh> CreateCylinder(float radius = 0.5f, float height = 1.0f, uint32_t segments = 48, AssetHandle material = UUID::Null());
		// Y-aligned capsule matching CapsuleColliderComponent: total height = 2 * (halfHeight + radius).
		static Ref<Mesh> CreateCapsule(float radius = 0.5f, float halfHeight = 0.5f, uint32_t segments = 48, uint32_t rings = 12, AssetHandle material = UUID::Null());
		static Ref<Mesh> CreateCone(float radius = 0.5f, float height = 1.0f, uint32_t segments = 48, AssetHandle material = UUID::Null());
		static Ref<Mesh> CreateTorus(float majorRadius = 0.375f, float minorRadius = 0.125f, uint32_t majorSegments = 48, uint32_t minorSegments = 24, AssetHandle material = UUID::Null());
	};

}
