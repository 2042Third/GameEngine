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

	// Per-vertex shading data (stream 1). Positions live in their own stream (stream 0) so depth-only passes such as
	// shadow maps fetch the minimum. Tangent.w holds the bitangent sign (MikkTSpace convention).
	struct MeshVertexAttributes
	{
		glm::vec3 Normal = { 0.0f, 1.0f, 0.0f };
		glm::vec4 Tangent = { 1.0f, 0.0f, 0.0f, 1.0f };
		glm::vec2 TexCoord = { 0.0f, 0.0f };
	};
	static_assert(sizeof(MeshVertexAttributes) == 36, "MeshVertexAttributes layout is part of the cooked mesh format");

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
	};

	// Indexed triangle mesh asset with one or more submeshes (one per material) and optional LOD levels. Front faces
	// are counter-clockwise. CPU geometry stays available after upload (physics colliders, picking).
	class Mesh : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::Mesh; }
		AssetType GetType() const override { return GetStaticType(); }

		static Ref<Mesh> Create(std::vector<glm::vec3> positions, std::vector<MeshVertexAttributes> attributes, std::vector<uint32_t> indices,
			std::vector<Submesh> submeshes, std::string* outError = nullptr);

		// Cooked format: "STMH" header, vertex streams, indices, submesh table.
		static constexpr uint32_t c_CookedVersion = 1;
		std::vector<uint8_t> Serialize() const;
		static Ref<Mesh> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);

		// Creates the GPU vertex and index buffers (the CPU geometry stays for physics and picking).
		bool FinalizeOnMainThread(const AssetFinalizeContext& context) override;
		// The CPU geometry and, once uploaded, the GPU buffers (each counted once, in its own pool).
		AssetMemoryUsage GetMemoryUsage() const override;

		const std::vector<glm::vec3>& GetPositions() const { return m_Positions; }
		const std::vector<MeshVertexAttributes>& GetAttributes() const { return m_Attributes; }
		const std::vector<uint32_t>& GetIndices() const { return m_Indices; }
		const std::vector<Submesh>& GetSubmeshes() const { return m_Submeshes; }
		const AABB& GetBounds() const { return m_Bounds; }
		uint32_t GetTriangleCount() const;

		nvrhi::IBuffer* GetPositionBuffer() const { return m_PositionBuffer; }
		nvrhi::IBuffer* GetAttributeBuffer() const { return m_AttributeBuffer; }
		nvrhi::IBuffer* GetIndexBuffer() const { return m_IndexBuffer; }
	private:
		Mesh() = default;
	private:
		std::vector<glm::vec3> m_Positions;
		std::vector<MeshVertexAttributes> m_Attributes;
		std::vector<uint32_t> m_Indices;
		std::vector<Submesh> m_Submeshes;
		AABB m_Bounds;

		nvrhi::BufferHandle m_PositionBuffer;
		nvrhi::BufferHandle m_AttributeBuffer;
		nvrhi::BufferHandle m_IndexBuffer;
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
		// normals first. Replaces all three streams; returns false (leaving them unchanged) on invalid input.
		bool GenerateSeamTangents(std::vector<glm::vec3>& positions, std::vector<MeshVertexAttributes>& attributes, std::vector<uint32_t>& indices, bool flatNormals);
		// Reorders triangles for the post-transform vertex cache (indices relative to the given vertex count).
		void OptimizeIndices(std::vector<uint32_t>& indices, size_t vertexCount);
		// Simplified index lists for LOD 1..maxLevels (each about half the previous triangle count), stopping early
		// when simplification can no longer reduce the mesh within the error tolerance.
		std::vector<std::vector<uint32_t>> GenerateLODs(std::span<const glm::vec3> positions, std::span<const uint32_t> indices, uint32_t maxLevels);
		AABB ComputeBounds(std::span<const glm::vec3> positions);
	}

}
