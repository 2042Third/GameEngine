#include "stpch.h"
#include "Strata/Renderer/Mesh.h"

#include "Strata/Core/BinaryStream.h"
#include "Strata/Renderer/GPUUploadQueue.h"
#include "Strata/Renderer/Renderer.h"

#include <meshoptimizer.h>
#include <mikktspace.h>

#include <array>
#include <bit>
#include <cmath>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_MeshMagic = 0x484D5453; // "STMH"
		constexpr uint64_t c_MaxVertices = 1ull << 28;
		constexpr uint64_t c_MaxIndices = 1ull << 30;
		// Vertex attributes from this size on are freed within the finalization deadline (freeing tens of megabytes takes
		// milliseconds), like textures' CPU copies.
		constexpr uint64_t c_TimedReleaseBytes = 1ull << 20;

		struct CookedMeshHeader
		{
			uint32_t Magic;
			uint32_t Version;
			uint32_t SubmeshCount;
			uint32_t Flags; // Version 2 (c_CookedMeshSkinned); version 1 wrote 0
		};

		constexpr uint32_t c_CookedMeshSkinned = 1u << 0;

		bool IsFiniteMatrix(const glm::mat4& matrix)
		{
			for (int column = 0; column < 4; column++)
			{
				for (int row = 0; row < 4; row++)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

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

		// A stream of the geometry and where in the pool's buffer it uploads to.
		struct UploadRange
		{
			nvrhi::IBuffer* Buffer = nullptr;
			uint64_t Offset = 0; // In the buffer
			const uint8_t* Data = nullptr;
			uint64_t Size = 0;
		};

	}

	Ref<Mesh> Mesh::Create(std::vector<glm::vec3> positions, std::vector<MeshVertexAttributes> attributes, std::vector<uint32_t> indices,
		std::vector<Submesh> submeshes, std::string* outError)
	{
		return Build(std::move(positions), std::move(attributes), std::move(indices), std::move(submeshes), {}, nullptr, outError);
	}

	Ref<Mesh> Mesh::CreateSkinned(std::vector<glm::vec3> positions, std::vector<MeshVertexAttributes> attributes, std::vector<uint32_t> indices,
		std::vector<Submesh> submeshes, std::vector<MeshSkinVertex> skinVertices, MeshSkinBinding binding, std::string* outError)
	{
		return Build(std::move(positions), std::move(attributes), std::move(indices), std::move(submeshes), std::move(skinVertices),
			CreateScope<MeshSkinBinding>(std::move(binding)), outError);
	}

	Ref<Mesh> Mesh::Build(std::vector<glm::vec3> positions, std::vector<MeshVertexAttributes> attributes, std::vector<uint32_t> indices,
		std::vector<Submesh> submeshes, std::vector<MeshSkinVertex> skinVertices, Scope<MeshSkinBinding> binding, std::string* outError)
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
			const MeshLOD& detail = submesh.LODs.front();
			submesh.UVDensity = MeshUtils::ComputeUVDensity(std::span<const glm::vec3>(positions).subspan(submesh.BaseVertex, submesh.VertexCount),
				std::span<const MeshVertexAttributes>(attributes).subspan(submesh.BaseVertex, submesh.VertexCount),
				std::span<const uint32_t>(indices).subspan(detail.IndexOffset, detail.IndexCount));
		}

		if (binding)
		{
			const size_t jointCount = binding->Joints.size();
			if (jointCount == 0 || jointCount > MeshSkinBinding::c_MaxJoints)
				return fail(fmt::format("A skinned mesh needs 1 to {} joints", MeshSkinBinding::c_MaxJoints));
			if (binding->InverseBindMatrices.size() != jointCount)
				return fail("A skinned mesh needs one inverse bind matrix per joint");
			if (skinVertices.size() != positions.size())
				return fail("The skin stream of a skinned mesh has another length than its vertices");
			for (const glm::mat4& matrix : binding->InverseBindMatrices)
			{
				// Affine (the fourth row is 0, 0, 0, 1), as the palette and the shaders treat them.
				if (!IsFiniteMatrix(matrix) || matrix[0][3] != 0.0f || matrix[1][3] != 0.0f || matrix[2][3] != 0.0f || matrix[3][3] != 1.0f)
					return fail("A skinned mesh has an inverse bind matrix that is not a finite affine transform");
			}

			// Each joint's bind-pose extent: the vertices it influences.
			binding->JointBounds.assign(jointCount, AABB());
			for (size_t vertex = 0; vertex < skinVertices.size(); vertex++)
			{
				const MeshSkinVertex& skin = skinVertices[vertex];
				uint32_t weightSum = 0;
				for (int influence = 0; influence < 4; influence++)
				{
					// The skinned vertex shaders read the palette entry of every influence, also of those without weight: each
					// must name a joint of the skin.
					if (skin.Joints[influence] >= jointCount)
						return fail(fmt::format("Vertex {} is influenced by joint {} of a skin with {} joints", vertex, skin.Joints[influence], jointCount));
					if (skin.Weights[influence] == 0)
						continue;
					weightSum += skin.Weights[influence];
					binding->JointBounds[skin.Joints[influence]].Expand(positions[vertex]);
				}
				if (weightSum != 65535)
					return fail(fmt::format("The skin weights of vertex {} add up to {} instead of 65535", vertex, weightSum));
			}
			// The distance is convex in the vertex: the farthest point of a joint's extent from its origin is a corner.
			binding->MaxJointExtent = 0.0f;
			for (size_t joint = 0; joint < jointCount; joint++)
			{
				const AABB& bounds = binding->JointBounds[joint];
				if (!bounds.IsValid())
					continue;
				for (int corner = 0; corner < 8; corner++)
				{
					const glm::vec3 point((corner & 1) ? bounds.Max.x : bounds.Min.x, (corner & 2) ? bounds.Max.y : bounds.Min.y, (corner & 4) ? bounds.Max.z : bounds.Min.z);
					binding->MaxJointExtent = std::max(binding->MaxJointExtent, glm::length(glm::vec3(binding->InverseBindMatrices[joint] * glm::vec4(point, 1.0f))));
				}
			}
			if (!std::isfinite(binding->MaxJointExtent))
				return fail("A skinned mesh's joints are too far from its vertices");
		}
		else if (!skinVertices.empty())
		{
			return fail("Skin vertices without a skin binding");
		}

		Ref<Mesh> mesh(new Mesh());
		mesh->m_Bounds = MeshUtils::ComputeBounds(positions);
		mesh->m_Positions = std::move(positions);
		mesh->m_Attributes = std::move(attributes);
		mesh->m_Indices = std::move(indices);
		mesh->m_Submeshes = std::move(submeshes);
		mesh->m_SkinVertices = std::move(skinVertices);
		if (binding)
			mesh->m_SkinBinding = Ref<const MeshSkinBinding>(binding.release());
		return mesh;
	}

	std::vector<uint8_t> Mesh::Serialize() const
	{
		ST_CORE_ASSERT(m_Attributes.size() == m_Positions.size(), "Mesh::Serialize after the upload released the vertex attributes");
		ST_CORE_ASSERT(!m_SkinBinding || m_SkinVertices.size() == m_Positions.size(), "Mesh::Serialize after the upload released the skin stream");
		BinaryWriter writer;
		writer.Write(CookedMeshHeader { c_MeshMagic, c_CookedVersion, static_cast<uint32_t>(m_Submeshes.size()), m_SkinBinding ? c_CookedMeshSkinned : 0u });
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
		if (m_SkinBinding)
		{
			writer.WriteArray(std::span<const MeshSkinVertex>(m_SkinVertices));
			writer.WriteArray(std::span<const uint16_t>(m_SkinBinding->Joints));
			writer.WriteArray(std::span<const glm::mat4>(m_SkinBinding->InverseBindMatrices));
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
		// Version 1 is version 2 without flags (its reserved word was always 0).
		if (header.Version != 1 && header.Version != c_CookedVersion)
			return fail(fmt::format("Unsupported cooked mesh version {}", header.Version));
		if (header.SubmeshCount == 0 || header.SubmeshCount > 65536 || (header.Flags & ~c_CookedMeshSkinned) != 0 || (header.Version == 1 && header.Flags != 0))
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

		if ((header.Flags & c_CookedMeshSkinned) == 0)
		{
			if (reader.GetRemaining() != 0)
				return fail("Cooked mesh has trailing data");
			return Create(std::move(positions), std::move(attributes), std::move(indices), std::move(submeshes), outError);
		}

		std::vector<MeshSkinVertex> skinVertices;
		MeshSkinBinding binding;
		if (!reader.ReadArray(skinVertices, c_MaxVertices) || !reader.ReadArray(binding.Joints, MeshSkinBinding::c_MaxJoints)
			|| !reader.ReadArray(binding.InverseBindMatrices, MeshSkinBinding::c_MaxJoints))
			return fail("Cooked mesh skin is truncated");
		if (reader.GetRemaining() != 0)
			return fail("Cooked mesh has trailing data");
		return CreateSkinned(std::move(positions), std::move(attributes), std::move(indices), std::move(submeshes), std::move(skinVertices), std::move(binding), outError);
	}

	Mesh::~Mesh()
	{
		// The pool returns the ranges once the GPU finished the frames that may still draw them.
		if (m_Geometry != c_InvalidGeometry)
		{
			if (const Ref<GeometryPool> pool = m_Pool.lock())
				pool->Free(m_Geometry);
		}
	}

	const GeometryAllocation* Mesh::GetGeometry() const
	{
		if (!m_Uploaded)
			return nullptr;
		const Ref<GeometryPool> pool = m_Pool.lock();
		return pool ? pool->Find(m_Geometry) : nullptr;
	}

	AssetFinalizeResult Mesh::FinalizeOnMainThread(const AssetFinalizeContext& context)
	{
		RendererUploadContext* upload = RendererUploadContext::From(context.Upload);
		if (!upload || !upload->GetQueue().IsBatchOpen() || m_Uploaded)
			return AssetFinalizeResult::Done;
		GPUUploadQueue& queue = upload->GetQueue();

		// Past the deadline only the frame's first finalization works (AssetFinalizeContext::FirstOfFrame), allocating too.
		if (!context.FirstOfFrame && std::chrono::steady_clock::now() >= context.Deadline)
			return AssetFinalizeResult::Pending;

		const uint32_t vertexCount = static_cast<uint32_t>(m_Positions.size());
		const uint32_t indexCount = static_cast<uint32_t>(m_Indices.size());
		Ref<GeometryPool> pool = m_Pool.lock();
		if (m_Geometry == c_InvalidGeometry)
		{
			pool = Renderer::GetGeometryPool();
			if (!pool)
				return AssetFinalizeResult::Failed;
			const GeometryPool::AllocateResult allocation = pool->Allocate(GetArena(), vertexCount, indexCount, queue.GetOpenBatch());
			if (allocation.Handle == c_InvalidGeometry)
			{
				// The pool grows at the next frame, between upload batches: the mesh uploads then.
				if (allocation.Retry)
					return AssetFinalizeResult::Pending;
				ST_CORE_ERROR("Mesh: its {} vertices and {} indices do not fit the geometry pool", vertexCount, indexCount);
				return AssetFinalizeResult::Failed;
			}
			m_Pool = pool;
			m_Geometry = allocation.Handle;
			m_UploadOffset = 0;
			// Its submeshes and levels of detail for GPU culling: each level a draw record.
			std::vector<RenderData::GeometrySubmesh> submeshes;
			std::vector<RenderData::GeometryLOD> lods;
			submeshes.reserve(m_Submeshes.size());
			for (const Submesh& submesh : m_Submeshes)
			{
				submeshes.push_back(RenderData::GeometrySubmesh { submesh.BaseVertex, static_cast<uint32_t>(lods.size()), static_cast<uint32_t>(submesh.LODs.size()), 0,
					glm::vec4(submesh.UVDensity, 0.0f) });
				for (const MeshLOD& lod : submesh.LODs)
					lods.push_back(RenderData::GeometryLOD { lod.IndexOffset, lod.IndexCount, submesh.BaseVertex, 0 });
			}
			if (!pool->SetLayout(m_Geometry, submeshes, lods))
			{
				ST_CORE_ERROR("Mesh: its {} submeshes do not fit the geometry pool's tables", m_Submeshes.size());
				return AssetFinalizeResult::Failed;
			}
		}
		const GeometryAllocation* geometry = pool ? pool->Find(m_Geometry) : nullptr;
		if (!geometry)
			return AssetFinalizeResult::Failed; // The renderer that held the pool went

		// The streams (the skin stream only of skinned meshes) upload one after the other, as one run of bytes cut into steps
		// through the renderer's upload queue, which sizes them to what is left of the frame's upload bytes and time (powers of
		// two from GPUUploadQueue::c_MinStepBytes, like the staging buffers they go through; the frame's first step goes
		// whatever). Steps wait for staging room like texture steps, so a mesh's upload never holds more staging than the budget
		// allows. The pool's buffers are looked up at every call: it may have grown into new ones since the last.
		const GeometryArena arena = GetArena();
		const size_t positionBytes = m_Positions.size() * sizeof(glm::vec3);
		const size_t attributeBytes = m_Attributes.size() * sizeof(MeshVertexAttributes);
		const size_t skinBytes = m_SkinVertices.size() * sizeof(MeshSkinVertex);
		const size_t indexBytes = m_Indices.size() * sizeof(uint32_t);
		const uint64_t firstVertex = geometry->FirstVertex;
		const std::array<UploadRange, 4> ranges = { {
			{ pool->GetPositionBuffer(arena), firstVertex * GeometryPool::c_PositionStride, reinterpret_cast<const uint8_t*>(m_Positions.data()), positionBytes },
			{ pool->GetAttributeBuffer(arena), firstVertex * GeometryPool::c_AttributeStride, reinterpret_cast<const uint8_t*>(m_Attributes.data()), attributeBytes },
			{ pool->GetSkinBuffer(), firstVertex * GeometryPool::c_SkinStride, reinterpret_cast<const uint8_t*>(m_SkinVertices.data()), skinBytes }, // Empty without a skin
			{ pool->GetIndexBuffer(), uint64_t(geometry->FirstIndex) * GeometryPool::c_IndexStride, reinterpret_cast<const uint8_t*>(m_Indices.data()), indexBytes }
		} };
		const uint64_t totalBytes = positionBytes + attributeBytes + skinBytes + indexBytes;
		uint64_t callBytes = 0;
		float callMs = 0.0f;
		std::array<GPUBufferRegion, 4> regions;
		while (m_UploadOffset < totalBytes)
		{
			const uint64_t remaining = totalBytes - m_UploadOffset;
			const bool framesFirst = GPUUploadQueue::IsFramesFirstStep(context, callBytes);
			const uint64_t limit = queue.PlanStep(context, callBytes, callMs, remaining, std::min(remaining, GPUUploadQueue::c_MinStepBytes));
			if (limit == 0)
				break;
			const uint64_t stepBytes = limit >= remaining ? remaining : std::bit_floor(limit);
			if (!queue.HasStagingRoom(context, StagingPool::GetBufferByteSize(stepBytes), framesFirst))
				break;

			// A step may span streams: one copy per stream it covers.
			const uint64_t stepEnd = m_UploadOffset + stepBytes;
			size_t regionCount = 0;
			uint64_t rangeStart = 0;
			for (const UploadRange& range : ranges)
			{
				const uint64_t rangeEnd = rangeStart + range.Size;
				const uint64_t writeBegin = std::max(m_UploadOffset, rangeStart);
				const uint64_t writeEnd = std::min(stepEnd, rangeEnd);
				if (writeBegin < writeEnd)
				{
					regions[regionCount++] = GPUBufferRegion { range.Buffer, range.Offset + (writeBegin - rangeStart), range.Data + (writeBegin - rangeStart),
						writeEnd - writeBegin };
				}
				rangeStart = rangeEnd;
			}
			pool->NoteUploadWrite(queue.GetOpenBatch());
			const GPUUploadStep step = queue.UploadBuffer(context, std::span<const GPUBufferRegion>(regions.data(), regionCount), GPUUploadQueueKind::Graphics);
			if (!step.Recorded)
			{
				ST_CORE_ERROR("Mesh: no staging memory for its upload");
				return AssetFinalizeResult::Failed;
			}
			m_UploadOffset = stepEnd;
			callBytes += step.Bytes;
			callMs += step.Milliseconds;
		}
		if (m_UploadOffset < totalBytes)
			return AssetFinalizeResult::Pending;

		// Only shading reads the vertex attributes and the skin stream, from the GPU: their CPU copies go (the terrain of the
		// stress world held 9.4 MB of attributes). Physics keeps reading positions and indices (AssetMeshProvider). Freeing a
		// large copy hands its pages back to the system, which takes a while: when the steps above ran past the deadline, it is
		// the next call's.
		if (callBytes > 0 && attributeBytes + skinBytes >= c_TimedReleaseBytes && std::chrono::steady_clock::now() >= context.Deadline)
			return AssetFinalizeResult::Pending;
		std::vector<MeshVertexAttributes>().swap(m_Attributes);
		std::vector<MeshSkinVertex>().swap(m_SkinVertices);
		// From now on compaction may move the geometry: the batch with its last copies is submitted before the next BeginFrame.
		pool->SetUploading(m_Geometry, false);
		m_Uploaded = true;
		return AssetFinalizeResult::Done;
	}

	AssetMemoryUsage Mesh::GetMemoryUsage() const
	{
		AssetMemoryUsage usage;
		usage.Cpu = m_Positions.size() * sizeof(glm::vec3) + m_Attributes.size() * sizeof(MeshVertexAttributes) + m_SkinVertices.size() * sizeof(MeshSkinVertex)
			+ m_Indices.size() * sizeof(uint32_t);
		for (const Submesh& submesh : m_Submeshes)
			usage.Cpu += sizeof(Submesh) + submesh.Name.size() + submesh.LODs.size() * sizeof(MeshLOD);
		if (m_SkinBinding)
		{
			usage.Cpu += sizeof(MeshSkinBinding) + m_SkinBinding->Joints.size() * (sizeof(uint16_t) + sizeof(glm::mat4) + sizeof(AABB));
		}
		// The pool's ranges it holds, from their allocation on (the pool's free space is the renderer's: GeometryPoolStats).
		if (m_Geometry != c_InvalidGeometry)
			usage.GpuBuffers += GeometryPool::GetAllocationBytes(GetArena(), static_cast<uint32_t>(m_Positions.size()), static_cast<uint32_t>(m_Indices.size()));
		return usage;
	}

	AssetMemoryUsage Mesh::GetFinalizedMemoryUsage() const
	{
		AssetMemoryUsage usage = GetMemoryUsage();
		if (Renderer::IsInitialized())
		{
			if (m_Geometry == c_InvalidGeometry)
				usage.GpuBuffers += GeometryPool::GetAllocationBytes(GetArena(), static_cast<uint32_t>(m_Positions.size()), static_cast<uint32_t>(m_Indices.size()));
			usage.Cpu -= m_Attributes.size() * sizeof(MeshVertexAttributes) + m_SkinVertices.size() * sizeof(MeshSkinVertex); // The upload releases them
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

		bool GenerateSeamTangents(std::vector<glm::vec3>& positions, std::vector<MeshVertexAttributes>& attributes, std::vector<uint32_t>& indices, bool flatNormals,
			std::vector<MeshSkinVertex>* skinVertices)
		{
			if (indices.empty() || indices.size() % 3 != 0 || positions.size() != attributes.size() || (skinVertices && skinVertices->size() != positions.size()))
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

			std::vector<MeshSkinVertex> cornerSkin;
			if (skinVertices)
			{
				cornerSkin.resize(cornerCount);
				for (size_t corner = 0; corner < cornerCount; corner++)
					cornerSkin[corner] = (*skinVertices)[indices[corner]];
			}

			// Weld corners whose data is bit-identical (MeshVertexAttributes and MeshSkinVertex have no padding, so bytes compare
			// values).
			const meshopt_Stream streams[] = {
				{ cornerPositions.data(), sizeof(glm::vec3), sizeof(glm::vec3) },
				{ cornerAttributes.data(), sizeof(MeshVertexAttributes), sizeof(MeshVertexAttributes) },
				{ cornerSkin.data(), sizeof(MeshSkinVertex), sizeof(MeshSkinVertex) }
			};
			std::vector<uint32_t> remap(cornerCount);
			const size_t vertexCount = meshopt_generateVertexRemapMulti(remap.data(), nullptr, cornerCount, cornerCount, streams, skinVertices ? 3 : 2);
			positions.resize(vertexCount);
			attributes.resize(vertexCount);
			meshopt_remapVertexBuffer(positions.data(), cornerPositions.data(), cornerCount, sizeof(glm::vec3), remap.data());
			meshopt_remapVertexBuffer(attributes.data(), cornerAttributes.data(), cornerCount, sizeof(MeshVertexAttributes), remap.data());
			if (skinVertices)
			{
				skinVertices->resize(vertexCount);
				meshopt_remapVertexBuffer(skinVertices->data(), cornerSkin.data(), cornerCount, sizeof(MeshSkinVertex), remap.data());
			}
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

		glm::vec3 ComputeUVDensity(std::span<const glm::vec3> positions, std::span<const MeshVertexAttributes> attributes, std::span<const uint32_t> indices)
		{
			// Twice the areas, per axis (both are halves of cross products, so the factor cancels).
			std::array<double, 3> area = {};
			std::array<double, 3> uvArea = {};
			for (size_t index = 0; index + 2 < indices.size(); index += 3)
			{
				const uint32_t a = indices[index];
				const uint32_t b = indices[index + 1];
				const uint32_t c = indices[index + 2];
				if (a >= positions.size() || b >= positions.size() || c >= positions.size() || positions.size() != attributes.size())
					continue;
				const glm::vec3 normal = glm::cross(positions[b] - positions[a], positions[c] - positions[a]);
				const glm::vec2 u = attributes[b].TexCoord - attributes[a].TexCoord;
				const glm::vec2 v = attributes[c].TexCoord - attributes[a].TexCoord;
				const double triangleUVArea = std::abs(static_cast<double>(u.x) * v.y - static_cast<double>(u.y) * v.x);
				const double triangleArea = static_cast<double>(glm::length(normal));
				if (!(triangleArea > 0.0) || !std::isfinite(triangleArea) || !std::isfinite(triangleUVArea))
					continue;
				const glm::vec3 facing = glm::abs(normal);
				const size_t axis = facing.x >= facing.y && facing.x >= facing.z ? 0 : (facing.y >= facing.z ? 1 : 2);
				area[axis] += triangleArea;
				uvArea[axis] += triangleUVArea;
			}

			const double total = area[0] + area[1] + area[2];
			glm::vec3 density(0.0f);
			for (size_t axis = 0; axis < 3; axis++)
			{
				if (area[axis] > 0.0 && area[axis] >= total * c_MinUVDensityAreaShare)
					density[static_cast<glm::length_t>(axis)] = static_cast<float>(std::sqrt(uvArea[axis] / area[axis]));
			}
			return density;
		}

	}

}
