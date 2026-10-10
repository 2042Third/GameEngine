#include "stpch.h"
#include "Strata/Renderer/Mesh.h"

#include "Strata/Core/BinaryStream.h"
#include "Strata/Renderer/Renderer.h"

#include <meshoptimizer.h>
#include <mikktspace.h>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_MeshMagic = 0x484D5453; // "STMH"
		constexpr uint64_t c_MaxVertices = 1ull << 28;
		constexpr uint64_t c_MaxIndices = 1ull << 30;

		struct CookedMeshHeader
		{
			uint32_t Magic;
			uint32_t Version;
			uint32_t SubmeshCount;
			uint32_t Reserved;
		};

		struct CookedSubmesh
		{
			uint32_t BaseVertex;
			uint32_t VertexCount;
			uint32_t LODCount;
			uint32_t Reserved;
			uint64_t Material;
			glm::vec3 BoundsMin;
			glm::vec3 BoundsMax;
		};

		nvrhi::BufferHandle CreateStaticBuffer(nvrhi::ICommandList* commandList, const void* data, size_t size, const char* name, bool vertex)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = std::max<size_t>(size, 4);
			desc.debugName = name;
			desc.isVertexBuffer = vertex;
			desc.isIndexBuffer = !vertex;
			// Also readable from shaders (e.g. GPU culling or ray tracing later).
			desc.canHaveRawViews = true;
			desc.initialState = vertex ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::IndexBuffer;
			desc.keepInitialState = true;

			nvrhi::BufferHandle buffer = Renderer::GetDevice()->createBuffer(desc);
			if (buffer && size > 0)
				commandList->writeBuffer(buffer, data, size);
			return buffer;
		}

	}

	Ref<Mesh> Mesh::Create(std::vector<glm::vec3> positions, std::vector<MeshVertexAttributes> attributes, std::vector<uint32_t> indices,
		std::vector<Submesh> submeshes, std::string* outError)
	{
		auto fail = [outError](const std::string& message) -> Ref<Mesh>
		{
			if (outError)
				*outError = message;
			return nullptr;
		};

		if (positions.empty() || indices.empty())
			return fail("Mesh has no geometry");
		if (positions.size() != attributes.size())
			return fail("Mesh vertex streams have different lengths");
		if (positions.size() > c_MaxVertices || indices.size() > c_MaxIndices)
			return fail("Mesh is too large");
		if (submeshes.empty())
			return fail("Mesh has no submeshes");

		for (const glm::vec3& position : positions)
		{
			if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
				return fail("Mesh contains non-finite vertex positions");
		}

		for (size_t submeshIndex = 0; submeshIndex < submeshes.size(); submeshIndex++)
		{
			Submesh& submesh = submeshes[submeshIndex];
			if (submesh.LODs.empty())
				return fail(fmt::format("Submesh {} has no index ranges", submeshIndex));
			if (static_cast<uint64_t>(submesh.BaseVertex) + submesh.VertexCount > positions.size())
				return fail(fmt::format("Submesh {} vertex range is out of bounds", submeshIndex));

			for (const MeshLOD& lod : submesh.LODs)
			{
				if (lod.IndexCount == 0 || lod.IndexCount % 3 != 0 || static_cast<uint64_t>(lod.IndexOffset) + lod.IndexCount > indices.size())
					return fail(fmt::format("Submesh {} has an invalid index range", submeshIndex));
				for (uint32_t index = lod.IndexOffset; index < lod.IndexOffset + lod.IndexCount; index++)
				{
					if (indices[index] >= submesh.VertexCount)
						return fail(fmt::format("Submesh {} references vertex {} outside its {} vertices", submeshIndex, indices[index], submesh.VertexCount));
				}
			}

			if (!submesh.Bounds.IsValid())
				submesh.Bounds = MeshUtils::ComputeBounds(std::span<const glm::vec3>(positions).subspan(submesh.BaseVertex, submesh.VertexCount));
		}

		Ref<Mesh> mesh(new Mesh());
		mesh->m_Bounds = MeshUtils::ComputeBounds(positions);
		mesh->m_Positions = std::move(positions);
		mesh->m_Attributes = std::move(attributes);
		mesh->m_Indices = std::move(indices);
		mesh->m_Submeshes = std::move(submeshes);
		return mesh;
	}

	std::vector<uint8_t> Mesh::Serialize() const
	{
		BinaryWriter writer;
		writer.Write(CookedMeshHeader { c_MeshMagic, c_CookedVersion, static_cast<uint32_t>(m_Submeshes.size()), 0 });
		writer.WriteArray(std::span<const glm::vec3>(m_Positions));
		writer.WriteArray(std::span<const MeshVertexAttributes>(m_Attributes));
		writer.WriteArray(std::span<const uint32_t>(m_Indices));
		for (const Submesh& submesh : m_Submeshes)
		{
			writer.Write(CookedSubmesh { submesh.BaseVertex, submesh.VertexCount, static_cast<uint32_t>(submesh.LODs.size()), 0,
				static_cast<uint64_t>(submesh.Material), submesh.Bounds.Min, submesh.Bounds.Max });
			writer.WriteString(submesh.Name);
			writer.WriteArray(std::span<const MeshLOD>(submesh.LODs));
		}
		return writer.TakeData();
	}

	Ref<Mesh> Mesh::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		auto fail = [outError](const std::string& message) -> Ref<Mesh>
		{
			if (outError)
				*outError = message;
			return nullptr;
		};

		BinaryReader reader(data);
		const CookedMeshHeader header = reader.Read<CookedMeshHeader>();
		if (!reader.IsValid() || header.Magic != c_MeshMagic)
			return fail("Not a cooked Strata mesh");
		if (header.Version != c_CookedVersion)
			return fail(fmt::format("Unsupported cooked mesh version {}", header.Version));
		if (header.SubmeshCount == 0 || header.SubmeshCount > 65536)
			return fail("Cooked mesh header is corrupt");

		std::vector<glm::vec3> positions;
		std::vector<MeshVertexAttributes> attributes;
		std::vector<uint32_t> indices;
		if (!reader.ReadArray(positions, c_MaxVertices) || !reader.ReadArray(attributes, c_MaxVertices) || !reader.ReadArray(indices, c_MaxIndices))
			return fail("Cooked mesh geometry is truncated");

		std::vector<Submesh> submeshes(header.SubmeshCount);
		for (Submesh& submesh : submeshes)
		{
			const CookedSubmesh cooked = reader.Read<CookedSubmesh>();
			submesh.Name = reader.ReadString(4096);
			if (!reader.IsValid() || cooked.LODCount == 0 || cooked.LODCount > 16 || !reader.ReadArray(submesh.LODs, 16) || submesh.LODs.size() != cooked.LODCount)
				return fail("Cooked mesh submesh table is corrupt");
			submesh.BaseVertex = cooked.BaseVertex;
			submesh.VertexCount = cooked.VertexCount;
			submesh.Material = UUID(cooked.Material);
			submesh.Bounds = AABB(cooked.BoundsMin, cooked.BoundsMax);
		}

		return Create(std::move(positions), std::move(attributes), std::move(indices), std::move(submeshes), outError);
	}

	bool Mesh::FinalizeOnMainThread(const AssetFinalizeContext& context)
	{
		nvrhi::ICommandList* commandList = context.CommandList;
		if (!commandList || m_IndexBuffer)
			return true;

		m_PositionBuffer = CreateStaticBuffer(commandList, m_Positions.data(), m_Positions.size() * sizeof(glm::vec3), "MeshPositions", true);
		m_AttributeBuffer = CreateStaticBuffer(commandList, m_Attributes.data(), m_Attributes.size() * sizeof(MeshVertexAttributes), "MeshAttributes", true);
		m_IndexBuffer = CreateStaticBuffer(commandList, m_Indices.data(), m_Indices.size() * sizeof(uint32_t), "MeshIndices", false);
		return m_PositionBuffer && m_AttributeBuffer && m_IndexBuffer;
	}

	AssetMemoryUsage Mesh::GetMemoryUsage() const
	{
		AssetMemoryUsage usage;
		usage.Cpu = m_Positions.size() * sizeof(glm::vec3) + m_Attributes.size() * sizeof(MeshVertexAttributes) + m_Indices.size() * sizeof(uint32_t);
		for (const Submesh& submesh : m_Submeshes)
			usage.Cpu += sizeof(Submesh) + submesh.Name.size() + submesh.LODs.size() * sizeof(MeshLOD);
		for (nvrhi::IBuffer* buffer : { m_PositionBuffer.Get(), m_AttributeBuffer.Get(), m_IndexBuffer.Get() })
		{
			if (buffer)
				usage.GpuBuffers += buffer->getDesc().byteSize;
		}
		return usage;
	}

	uint32_t Mesh::GetTriangleCount() const
	{
		uint32_t triangles = 0;
		for (const Submesh& submesh : m_Submeshes)
			triangles += submesh.LODs[0].IndexCount / 3;
		return triangles;
	}

	namespace MeshUtils
	{

		void GenerateNormals(std::span<const glm::vec3> positions, std::span<const uint32_t> indices, std::span<MeshVertexAttributes> attributes)
		{
			std::vector<glm::vec3> normals(positions.size(), glm::vec3(0.0f));
			for (size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3)
			{
				const uint32_t i0 = indices[triangle];
				const uint32_t i1 = indices[triangle + 1];
				const uint32_t i2 = indices[triangle + 2];
				if (i0 >= positions.size() || i1 >= positions.size() || i2 >= positions.size())
					continue;
				// Unnormalized cross product: larger triangles contribute more.
				const glm::vec3 faceNormal = glm::cross(positions[i1] - positions[i0], positions[i2] - positions[i0]);
				normals[i0] += faceNormal;
				normals[i1] += faceNormal;
				normals[i2] += faceNormal;
			}

			for (size_t vertex = 0; vertex < positions.size() && vertex < attributes.size(); vertex++)
			{
				const float length = glm::length(normals[vertex]);
				attributes[vertex].Normal = length > 1e-12f ? normals[vertex] / length : glm::vec3(0.0f, 1.0f, 0.0f);
			}
		}

		namespace
		{
			struct MikkContext
			{
				std::span<const glm::vec3> Positions;
				std::span<const uint32_t> Indices;
				std::span<MeshVertexAttributes> Attributes;
			};

			MikkContext& GetContext(const SMikkTSpaceContext* context)
			{
				return *static_cast<MikkContext*>(context->m_pUserData);
			}
		}

		bool GenerateTangents(std::span<const glm::vec3> positions, std::span<const uint32_t> indices, std::span<MeshVertexAttributes> attributes)
		{
			if (indices.size() < 3 || positions.size() != attributes.size())
				return false;
			for (uint32_t index : indices)
			{
				if (index >= positions.size())
					return false;
			}

			MikkContext mikkContext { positions, indices, attributes };
			SMikkTSpaceInterface callbacks = {};
			callbacks.m_getNumFaces = [](const SMikkTSpaceContext* context) { return static_cast<int>(GetContext(context).Indices.size() / 3); };
			callbacks.m_getNumVerticesOfFace = [](const SMikkTSpaceContext*, const int) { return 3; };
			callbacks.m_getPosition = [](const SMikkTSpaceContext* context, float output[], const int face, const int vertex)
			{
				const MikkContext& data = GetContext(context);
				const glm::vec3& position = data.Positions[data.Indices[static_cast<size_t>(face) * 3 + static_cast<size_t>(vertex)]];
				output[0] = position.x;
				output[1] = position.y;
				output[2] = position.z;
			};
			callbacks.m_getNormal = [](const SMikkTSpaceContext* context, float output[], const int face, const int vertex)
			{
				const MikkContext& data = GetContext(context);
				const glm::vec3& normal = data.Attributes[data.Indices[static_cast<size_t>(face) * 3 + static_cast<size_t>(vertex)]].Normal;
				output[0] = normal.x;
				output[1] = normal.y;
				output[2] = normal.z;
			};
			callbacks.m_getTexCoord = [](const SMikkTSpaceContext* context, float output[], const int face, const int vertex)
			{
				const MikkContext& data = GetContext(context);
				const glm::vec2& texCoord = data.Attributes[data.Indices[static_cast<size_t>(face) * 3 + static_cast<size_t>(vertex)]].TexCoord;
				output[0] = texCoord.x;
				output[1] = texCoord.y;
			};
			callbacks.m_setTSpaceBasic = [](const SMikkTSpaceContext* context, const float tangent[], const float sign, const int face, const int vertex)
			{
				MikkContext& data = GetContext(context);
				// Shared vertices receive the value of their last face (GenerateSeamTangents avoids sharing).
				data.Attributes[data.Indices[static_cast<size_t>(face) * 3 + static_cast<size_t>(vertex)]].Tangent = glm::vec4(tangent[0], tangent[1], tangent[2], sign);
			};

			SMikkTSpaceContext context = {};
			context.m_pInterface = &callbacks;
			context.m_pUserData = &mikkContext;
			return genTangSpaceDefault(&context) != 0;
		}

		bool GenerateSeamTangents(std::vector<glm::vec3>& positions, std::vector<MeshVertexAttributes>& attributes, std::vector<uint32_t>& indices, bool flatNormals)
		{
			if (indices.empty() || indices.size() % 3 != 0 || positions.size() != attributes.size())
				return false;
			for (uint32_t index : indices)
			{
				if (index >= positions.size())
					return false;
			}

			const size_t cornerCount = indices.size();
			std::vector<glm::vec3> cornerPositions(cornerCount);
			std::vector<MeshVertexAttributes> cornerAttributes(cornerCount);
			for (size_t corner = 0; corner < cornerCount; corner++)
			{
				cornerPositions[corner] = positions[indices[corner]];
				cornerAttributes[corner] = attributes[indices[corner]];
			}
			if (flatNormals)
			{
				for (size_t corner = 0; corner < cornerCount; corner += 3)
				{
					const glm::vec3 faceNormal = glm::cross(cornerPositions[corner + 1] - cornerPositions[corner], cornerPositions[corner + 2] - cornerPositions[corner]);
					const float length = glm::length(faceNormal);
					const glm::vec3 normal = length > 1e-12f ? faceNormal / length : glm::vec3(0.0f, 1.0f, 0.0f);
					for (size_t vertex = 0; vertex < 3; vertex++)
						cornerAttributes[corner + vertex].Normal = normal;
				}
			}

			std::vector<uint32_t> sequential(cornerCount);
			for (size_t corner = 0; corner < cornerCount; corner++)
				sequential[corner] = static_cast<uint32_t>(corner);
			if (!GenerateTangents(cornerPositions, sequential, cornerAttributes))
				return false;

			// Weld corners whose data is bit-identical (MeshVertexAttributes has no padding, so bytes compare values).
			const meshopt_Stream streams[] = {
				{ cornerPositions.data(), sizeof(glm::vec3), sizeof(glm::vec3) },
				{ cornerAttributes.data(), sizeof(MeshVertexAttributes), sizeof(MeshVertexAttributes) }
			};
			std::vector<uint32_t> remap(cornerCount);
			const size_t vertexCount = meshopt_generateVertexRemapMulti(remap.data(), nullptr, cornerCount, cornerCount, streams, std::size(streams));
			positions.resize(vertexCount);
			attributes.resize(vertexCount);
			meshopt_remapVertexBuffer(positions.data(), cornerPositions.data(), cornerCount, sizeof(glm::vec3), remap.data());
			meshopt_remapVertexBuffer(attributes.data(), cornerAttributes.data(), cornerCount, sizeof(MeshVertexAttributes), remap.data());
			indices = std::move(remap); // Corner i of the sequential index buffer is vertex remap[i]
			return true;
		}

		void OptimizeIndices(std::vector<uint32_t>& indices, size_t vertexCount)
		{
			if (indices.empty() || indices.size() % 3 != 0 || vertexCount == 0)
				return;
			for (uint32_t index : indices)
			{
				if (index >= vertexCount)
					return; // meshoptimizer only asserts in-range indices
			}
			meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), vertexCount);
		}

		std::vector<std::vector<uint32_t>> GenerateLODs(std::span<const glm::vec3> positions, std::span<const uint32_t> indices, uint32_t maxLevels)
		{
			std::vector<std::vector<uint32_t>> lods;
			if (positions.empty() || indices.size() < 3 || indices.size() % 3 != 0)
				return lods;
			for (uint32_t index : indices)
			{
				if (index >= positions.size())
					return lods;
			}
			size_t currentCount = indices.size();
			constexpr float targetError = 0.02f; // Relative to the mesh extent
			for (uint32_t level = 0; level < maxLevels; level++)
			{
				const size_t targetCount = (currentCount / 2) / 3 * 3;
				if (targetCount < 36)
					break;

				std::vector<uint32_t> simplified(indices.size());
				float resultError = 0.0f;
				const size_t count = meshopt_simplify(simplified.data(), indices.data(), indices.size(), &positions[0].x, positions.size(),
					sizeof(glm::vec3), targetCount, targetError, 0, &resultError);
				// Stop when simplification no longer makes meaningful progress.
				if (count == 0 || count > currentCount * 3 / 4)
					break;

				simplified.resize(count);
				meshopt_optimizeVertexCache(simplified.data(), simplified.data(), simplified.size(), positions.size());
				currentCount = count;
				lods.push_back(std::move(simplified));
			}
			return lods;
		}

		AABB ComputeBounds(std::span<const glm::vec3> positions)
		{
			AABB bounds;
			for (const glm::vec3& position : positions)
				bounds.Expand(position);
			return bounds;
		}

	}

}
