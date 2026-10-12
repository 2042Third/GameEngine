#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Math/AABB.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <span>
#include <string>
#include <vector>

namespace Strata
{

	enum class GPUUploadQueueKind : uint8_t;

	// Per-vertex shading data (stream 1). Positions live in their own stream (stream 0) so depth-only passes such as
	// shadow maps fetch the minimum. Tangent.w holds the bitangent sign (MikkTSpace convention).
	struct MeshVertexAttributes
	{
		glm::vec3 Normal = { 0.0f, 1.0f, 0.0f };
		glm::vec4 Tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
		glm::vec2 TexCoord = { 0.0f, 0.0f };
	};
	static_assert(sizeof(MeshVertexAttributes) == 36, "MeshVertexAttributes layout is part of the cooked mesh format");

	// The skinning data of a vertex (stream 2 of skinned meshes): four influences, each a joint of the mesh's skin (an index
	// into MeshSkinBinding::Joints) and its weight in 1/65535ths. The weights of a vertex add up to 65535; unused influences
	// have weight 0.
	struct MeshSkinVertex
	{
		uint16_t Joints[4] = { 0, 0, 0, 0 };
		uint16_t Weights[4] = { 65535, 0, 0, 0 };
	};
	static_assert(sizeof(MeshSkinVertex) == 16, "MeshSkinVertex layout is part of the cooked mesh format and the skinned vertex input");

	// How a skinned mesh is bound to its skeleton (one glTF skin): for each joint of the skin, the skeleton joint that moves
	// it, its inverse bind matrix (from the mesh's space into the joint's space at bind time) and the bind-pose extent of the
	// vertices it influences (empty for joints that influence none). The skinning palette of a posed instance holds, per skin
	// joint, the joint's model-space transform times its inverse bind matrix; the vertex shader blends the palette entries
	// of a vertex's influences. A posed instance's bounds are the union of each joint's extent transformed by its palette
	// entry: every skinned vertex is a weighted average of points inside those boxes.
	struct MeshSkinBinding
	{
		static constexpr uint32_t c_MaxJoints = 4096;

		std::vector<uint16_t> Joints;               // Skeleton joint of each skin joint
		std::vector<glm::mat4> InverseBindMatrices; // Affine, per skin joint
		std::vector<AABB> JointBounds;              // Per skin joint, in the mesh's (bind) space; computed by Mesh::CreateSkinned
		// The farthest a vertex lies from the origin of a joint that influences it, in that joint's space (its inverse bind
		// matrix times the vertex): in any pose, the skinned vertices lie within this distance (times the joints' scale) of
		// their joints' origins. Bounds where joints may go, not where they are (SkeletonPose::Reach), grow by it. Computed by
		// Mesh::CreateSkinned.
		float MaxJointExtent = 0.0f;
	};

	struct MeshLOD
	{
		uint32_t IndexOffset = 0; // Into the mesh index buffer
		uint32_t IndexCount = 0;
	};

	struct Submesh
	{
		std::string Name;
		uint32_t BaseVertex = 0; // Added to every index of this submesh
		uint32_t VertexCount = 0;
		std::vector<MeshLOD> LODs; // LODs[0] is full detail; later levels are progressively simplified
		AssetHandle Material = UUID::Null(); // Default material (overridable per MeshRenderer)
		AABB Bounds;
		// Texture coordinate units per unit of the mesh's own space along its surfaces (of LOD 0), for the surfaces whose
		// normals point along each axis most (x: those facing +X or -X), 0 for an axis no surface faces: what a texture's
		// mip level on screen follows (TextureStreamingPolicy::GetUVPixels). Mesh::Create computes it.
		glm::vec3 UVDensity = glm::vec3(0.0f);
	};

	// Indexed triangle mesh asset with one or more submeshes (one per material) and optional LOD levels. Front faces
	// are counter-clockwise. Positions and indices stay on the CPU after upload (physics colliders read them); the vertex
	// attributes, which only shading reads, live on the GPU alone once uploaded. Skinned meshes (CreateSkinned) also have
	// a skin stream (a MeshSkinVertex per vertex, also GPU-only once uploaded) and a skin binding; their positions are the
	// bind pose, which physics' mesh colliders use.
	class Mesh : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::Mesh; }
		AssetType GetType() const override { return GetStaticType(); }

		static Ref<Mesh> Create(std::vector<glm::vec3> positions, std::vector<MeshVertexAttributes> attributes, std::vector<uint32_t> indices,
			std::vector<Submesh> submeshes, std::string* outError = nullptr);
		// A skinned mesh: one skin vertex per position, every influence with a weight naming a joint of the binding, the weights
		// of each vertex adding up to 65535, finite affine inverse bind matrices (one per joint). The joints' bind-pose extents
		// are computed here (JointBounds given are replaced).
		static Ref<Mesh> CreateSkinned(std::vector<glm::vec3> positions, std::vector<MeshVertexAttributes> attributes, std::vector<uint32_t> indices,
			std::vector<Submesh> submeshes, std::vector<MeshSkinVertex> skinVertices, MeshSkinBinding binding, std::string* outError = nullptr);

		// Cooked format: "STMH" header (version, submesh count, flags), vertex streams, indices, submesh table, and for
		// skinned meshes (flag bit 0) the skin stream, the skin's joints and their inverse bind matrices. Version 1 (without
		// flags or skins) still loads. Serialize only meshes that were not uploaded (importers' and tools' meshes): the upload
		// releases the vertex attributes and the skin stream.
		static constexpr uint32_t c_CookedVersion = 2;
		std::vector<uint8_t> Serialize() const;
		static Ref<Mesh> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);

		// Creates the GPU vertex and index buffers and uploads the geometry into them through the renderer's upload queue
		// (GPUUploadQueue), in steps the queue sizes to what is left of the context's upload bytes, upload time and deadline
		// (Pending: the next call continues). Steps go through staging buffers of the renderer's pool (StagingPool) and wait for
		// staging room within the context's StagingBytes like texture steps. On the transfer queue
		// the buffers are handed over to the graphics queue once the copies completed (AwaitingGPU meanwhile, when the context
		// allows it). Once uploaded, the CPU copy of the vertex attributes is released; positions and indices stay for physics.
		AssetFinalizeResult FinalizeOnMainThread(const AssetFinalizeContext& context) override;
		// The CPU geometry and, once uploaded, the GPU buffers (each counted once, in its own pool).
		AssetMemoryUsage GetMemoryUsage() const override;
		// The CPU geometry and, with a renderer, the GPU buffers it will create, without the vertex attributes the upload
		// releases.
		AssetMemoryUsage GetFinalizedMemoryUsage() const override;

		const std::vector<glm::vec3>& GetPositions() const { return m_Positions; }
		// Empty once the mesh is uploaded: from then on only the GPU holds the vertex attributes.
		const std::vector<MeshVertexAttributes>& GetAttributes() const { return m_Attributes; }
		const std::vector<uint32_t>& GetIndices() const { return m_Indices; }
		const std::vector<Submesh>& GetSubmeshes() const { return m_Submeshes; }
		// The bind pose's bounds for skinned meshes (a posed instance's bounds follow its joints, see MeshSkinBinding).
		const AABB& GetBounds() const { return m_Bounds; }
		uint32_t GetTriangleCount() const;

		bool IsSkinned() const { return m_SkinBinding != nullptr; }
		// The skin binding of a skinned mesh (null otherwise): immutable and shared, so caches keep it after the mesh goes.
		const Ref<const MeshSkinBinding>& GetSkinBinding() const { return m_SkinBinding; }
		// Empty once the mesh is uploaded, like the vertex attributes.
		const std::vector<MeshSkinVertex>& GetSkinVertices() const { return m_SkinVertices; }

		nvrhi::IBuffer* GetPositionBuffer() const { return m_PositionBuffer; }
		nvrhi::IBuffer* GetAttributeBuffer() const { return m_AttributeBuffer; }
		nvrhi::IBuffer* GetSkinBuffer() const { return m_SkinBuffer; } // Null for meshes without a skin
		nvrhi::IBuffer* GetIndexBuffer() const { return m_IndexBuffer; }
	private:
		Mesh() = default;
		// Validates and assembles a mesh; the skin vertices are empty and the binding null for unskinned meshes.
		static Ref<Mesh> Build(std::vector<glm::vec3> positions, std::vector<MeshVertexAttributes> attributes, std::vector<uint32_t> indices,
			std::vector<Submesh> submeshes, std::vector<MeshSkinVertex> skinVertices, Scope<MeshSkinBinding> binding, std::string* outError);
	private:
		std::vector<glm::vec3> m_Positions;
		std::vector<MeshVertexAttributes> m_Attributes;
		std::vector<uint32_t> m_Indices;
		std::vector<Submesh> m_Submeshes;
		AABB m_Bounds;
		std::vector<MeshSkinVertex> m_SkinVertices;
		Ref<const MeshSkinBinding> m_SkinBinding;

		nvrhi::BufferHandle m_PositionBuffer;
		nvrhi::BufferHandle m_AttributeBuffer;
		nvrhi::BufferHandle m_SkinBuffer;
		nvrhi::BufferHandle m_IndexBuffer;
		// Bytes of the geometry uploaded so far, counting positions, attributes and indices one after the other.
		uint64_t m_UploadOffset = 0;
		GPUUploadQueueKind m_UploadQueue {}; // The queue the upload's copies run on (the graphics queue until one is chosen)
		uint64_t m_TransferTicket = 0; // While the transfer queue's copies complete: the ticket of the release (GPUUploadTicket)
		bool m_Uploaded = false;
	};

	namespace MeshUtils
	{
		// Area-weighted vertex normals for indexed triangles (indices relative to the given vertices).
		void GenerateNormals(std::span<const glm::vec3> positions, std::span<const uint32_t> indices, std::span<MeshVertexAttributes> attributes);
		// MikkTSpace tangents (requires normals and texture coordinates). Vertices shared by several triangles keep the
		// tangent of one of them; use GenerateSeamTangents where UV seams or mirrored UVs meet at shared vertices.
		bool GenerateTangents(std::span<const glm::vec3> positions, std::span<const uint32_t> indices, std::span<MeshVertexAttributes> attributes);
		// Exact MikkTSpace tangents: unwelds the triangles, generates per-corner tangents, then welds identical vertices
		// again (vertices on UV seams and mirror lines end up split). With flatNormals the triangles also get faceted
		// normals first. Replaces all three streams, and the skin stream when one is given (one entry per vertex: it is welded
		// with the others); returns false (leaving them unchanged) on invalid input.
		bool GenerateSeamTangents(std::vector<glm::vec3>& positions, std::vector<MeshVertexAttributes>& attributes, std::vector<uint32_t>& indices, bool flatNormals,
			std::vector<MeshSkinVertex>* skinVertices = nullptr);
		// Reorders triangles for the post-transform vertex cache (indices relative to the given vertex count).
		void OptimizeIndices(std::vector<uint32_t>& indices, size_t vertexCount);
		// Simplified index lists for LOD 1..maxLevels (each about half the previous triangle count), stopping early
		// when simplification can no longer reduce the mesh within the error tolerance.
		std::vector<std::vector<uint32_t>> GenerateLODs(std::span<const glm::vec3> positions, std::span<const uint32_t> indices, uint32_t maxLevels);
		AABB ComputeBounds(std::span<const glm::vec3> positions);
		// Submesh::UVDensity of indexed triangles (indices relative to the given vertices): per axis, the square root of the
		// texture coordinate area over the area of the triangles whose normals point along it most. An axis whose triangles
		// make less than c_MinUVDensityAreaShare of the area gets 0 (slivers would decide alone otherwise), as do triangles
		// without area or with non-finite texture coordinates.
		constexpr double c_MinUVDensityAreaShare = 0.01;
		glm::vec3 ComputeUVDensity(std::span<const glm::vec3> positions, std::span<const MeshVertexAttributes> attributes, std::span<const uint32_t> indices);
	}

}
