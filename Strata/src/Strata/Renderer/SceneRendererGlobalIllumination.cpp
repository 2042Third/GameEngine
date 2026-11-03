#include "stpch.h"
#include "Strata/Renderer/SceneRendererGlobalIllumination.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Renderer/GPUSceneData.h"
#include "Strata/Renderer/GeometryPool.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/RenderSceneGPU.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/ShaderLibrary.h"
#include "Strata/Renderer/Texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>

// Global illumination: world-space radiance cascades over a voxel clipmap (Docs/DeveloperGuide.md, "Global illumination").
// Stage 1 holds the voxels: each frame that wants GI, the clipmap (VoxelClipmap) follows the camera and the scene on the CPU and
// lists what changed; GPU.RadianceCascades voxelizes it batch by batch (GI/VoxelTriangles.comp, VoxelTiles.comp, VoxelFill.comp,
// VoxelFinalize.comp) into each level's textures, which the voxels view (GI/VoxelView.frag) shows. Nothing is made before the
// first frame that wants GI, and the first frame that does not releases everything.

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_GroupSize = 64;
		constexpr uint32_t c_MaxDispatchGroups = 65535;

		// The render scene's GPU copy and the scene's assets as the voxel clipmap reads them.
		class RendererVoxelSource final : public IVoxelSceneSource
		{
		public:
			RendererVoxelSource(const RenderScene& renderScene, const RenderSceneGPU& sceneGPU, const AssetManagerBase* assets)
				: m_RenderScene(renderScene), m_SceneGPU(sceneGPU), m_Assets(assets)
			{
			}

			bool IsResident(uint32_t proxy) const override { return m_SceneGPU.IsResident(proxy); }
			uint64_t GetTablesVersion() const override { return m_SceneGPU.GetTablesVersion(); }
			uint32_t GetMeshTableEntry(uint32_t meshSlot) const override { return m_SceneGPU.GetMeshTableEntry(meshSlot); }

			Ref<const Mesh> GetMesh(uint32_t meshSlot) const override
			{
				if (!m_Assets || meshSlot >= m_RenderScene.GetMeshSlotCapacity())
					return nullptr;
				return m_Assets->PeekLoadedAsset<Mesh>(m_RenderScene.GetMeshHandle(meshSlot));
			}

			// The material's diffuse albedo (BaseColor times 1 - Metallic: a metal bounces no diffuse light) and emission, each
			// times its map's mean once the map is loaded (the CPU's means: the voxelizer reads no texture).
			RenderData::GIMaterial GetMaterial(uint32_t materialSlot) const override
			{
				RenderData::GIMaterial material = {};
				material.Flags = m_SceneGPU.GetMaterialFlags(materialSlot);
				const RenderData::MaterialData* data = m_SceneGPU.GetMaterialData(materialSlot);
				if (!data || material.Flags == 0)
					return material;
				glm::vec3 albedo = glm::vec3(data->BaseColor) * (1.0f - std::clamp(data->Metallic, 0.0f, 1.0f));
				glm::vec3 emission = glm::vec3(data->Emissive);
				Ref<Material> loaded;
				if (m_Assets && materialSlot < m_RenderScene.GetMaterialSlotCapacity())
				{
					loaded = m_Assets->PeekLoadedAsset<Material>(m_RenderScene.GetMaterialHandle(materialSlot));
					if (!loaded)
						loaded = m_Assets->PeekLoadedAsset<Material>(BuiltinAssets::DefaultMaterial);
				}
				if (loaded)
				{
					const MaterialProperties& properties = loaded->GetProperties();
					if (const Ref<Texture> map = properties.BaseColorMap.IsValid() ? m_Assets->PeekLoadedAsset<Texture>(properties.BaseColorMap) : nullptr)
						albedo *= glm::vec3(map->GetLinearMean());
					if (const Ref<Texture> map = properties.EmissiveMap.IsValid() ? m_Assets->PeekLoadedAsset<Texture>(properties.EmissiveMap) : nullptr)
						emission *= glm::vec3(map->GetLinearMean());
				}
				material.Albedo = GlobalIllumination::PackAlbedo(albedo);
				material.Emission = GlobalIllumination::PackRGB9E5(emission);
				return material;
			}
		private:
			const RenderScene& m_RenderScene;
			const RenderSceneGPU& m_SceneGPU;
			const AssetManagerBase* m_Assets;
		};

		bool SameLayout(const GlobalIlluminationLayout& a, const GlobalIlluminationLayout& b)
		{
			if (a.Levels.size() != b.Levels.size() || a.BatchVoxels != b.BatchVoxels)
				return false;
			for (size_t level = 0; level < a.Levels.size(); level++)
			{
				if (a.Levels[level].VoxelSize != b.Levels[level].VoxelSize || a.Levels[level].Dimensions != b.Levels[level].Dimensions)
					return false;
			}
			return true;
		}

		uint32_t DivideUp(uint32_t value, uint32_t divisor)
		{
			return (value + divisor - 1) / divisor;
		}

		// A display color as RGBA8 (red in the low byte), as the lighting views pack theirs.
		uint32_t PackDisplayColor(const glm::vec4& color)
		{
			uint32_t packed = 0;
			for (int channel = 0; channel < 4; channel++)
			{
				const float value = std::isfinite(color[channel]) ? std::clamp(color[channel], 0.0f, 1.0f) : 0.0f;
				packed |= static_cast<uint32_t>(std::lround(value * 255.0f)) << (8 * channel);
			}
			return packed;
		}

	}

	bool SceneRenderer::IsGlobalIlluminationView(SceneLightingView view)
	{
		return view == SceneLightingView::Voxels;
	}

	const GlobalIlluminationLayout* SceneRenderer::GetGlobalIlluminationLayout() const
	{
		return m_GI ? &m_GI->Layout : nullptr;
	}

	const VoxelClipmap* SceneRenderer::GetVoxelClipmap() const
	{
		return m_GI ? m_GI->Clipmap.get() : nullptr;
	}

	uint64_t SceneRenderer::GetGlobalIlluminationMemoryBytes() const
	{
		return m_GI ? m_GI->MemoryBytes : 0;
	}

	nvrhi::ITexture* SceneRenderer::GetBoundGIVoxels(uint32_t level) const
	{
		if (m_GI && level < m_GI->Levels.size())
			return m_GI->Levels[level].Material;
		return Renderer::GetBlack3DTexture();
	}

	bool SceneRenderer::EnsureGlobalIlluminationResources(const GlobalIlluminationLayout& layout)
	{
		if (m_GI && SameLayout(m_GI->Layout, layout))
			return true;
		if (m_GI)
		{
			std::vector<nvrhi::ResourceHandle> released;
			ReleaseGlobalIllumination(released);
			Renderer::ReleaseDeferred(std::move(released));
		}

		Scope<GlobalIlluminationResources> gi = CreateScope<GlobalIlluminationResources>();
		gi->Layout = layout;
		gi->Clipmap = CreateScope<VoxelClipmap>(layout);
		const std::string name = m_Specification.DebugName + ".GI.";
		uint64_t bytes = 0;
		bool created = true;

		const auto createTexture = [&](nvrhi::Format format, const glm::uvec3& dimensions, const std::string& debugName)
		{
			nvrhi::TextureDesc desc;
			desc.dimension = nvrhi::TextureDimension::Texture3D;
			desc.width = dimensions.x;
			desc.height = dimensions.y;
			desc.depth = dimensions.z;
			desc.format = format;
			desc.isUAV = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = debugName;
			nvrhi::TextureHandle texture = m_Device->createTexture(desc);
			created = created && texture;
			bytes += static_cast<uint64_t>(dimensions.x) * dimensions.y * dimensions.z * nvrhi::getFormatInfo(format).bytesPerBlock;
			return texture;
		};
		for (uint32_t level = 0; level < layout.Levels.size(); level++)
		{
			const glm::uvec3 dimensions = layout.Levels[level].Dimensions;
			GlobalIlluminationResources::Level textures;
			textures.Material = createTexture(nvrhi::Format::RGBA8_UNORM, dimensions, name + "Material" + std::to_string(level));
			textures.Emission = createTexture(nvrhi::Format::R32_UINT, dimensions, name + "Emission" + std::to_string(level));
			textures.Normal = createTexture(nvrhi::Format::RGBA8_SNORM, dimensions, name + "Normal" + std::to_string(level));
			gi->Levels.push_back(textures);
		}

		// Buffers the voxelizer writes start and stay in UnorderedAccess; what the CPU writes is read as ShaderResource.
		const auto createBuffer = [&](uint64_t byteSize, uint32_t stride, bool raw, bool unorderedAccess, const char* debugName)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = byteSize;
			desc.structStride = stride;
			desc.canHaveRawViews = raw;
			desc.canHaveUAVs = unorderedAccess;
			desc.initialState = unorderedAccess ? nvrhi::ResourceStates::UnorderedAccess : nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = name + debugName;
			nvrhi::BufferHandle buffer = m_Device->createBuffer(desc);
			created = created && buffer;
			bytes += byteSize;
			return buffer;
		};
		gi->Scratch = createBuffer(static_cast<uint64_t>(layout.BatchVoxels) * 8 * sizeof(uint32_t), 0, true, true, "Scratch");
		gi->Tiles = createBuffer(static_cast<uint64_t>(RenderData::c_GIMaxTiles) * 8, 8, false, true, "Tiles");
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = 4 * sizeof(uint32_t);
			desc.canHaveRawViews = true;
			desc.canHaveUAVs = true;
			desc.isDrawIndirectArgs = true;
			desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			desc.keepInitialState = true;
			desc.debugName = name + "TileArguments";
			gi->TileArguments = m_Device->createBuffer(desc);
			created = created && gi->TileArguments;
		}
		gi->Counters = createBuffer(4 * sizeof(uint32_t), 0, true, true, "Counters");
		gi->Regions = createBuffer(static_cast<uint64_t>(RenderData::c_GIMaxRegions) * sizeof(RenderData::VoxelRegion), sizeof(RenderData::VoxelRegion), false, false,
			"Regions");
		gi->Items = createBuffer(static_cast<uint64_t>(RenderData::c_GIMaxWorkItems) * sizeof(RenderData::VoxelWorkItem), sizeof(RenderData::VoxelWorkItem), false,
			false, "Items");
		gi->Groups = createBuffer(static_cast<uint64_t>(RenderData::c_GIMaxTriangleGroups) * sizeof(RenderData::VoxelBatch), sizeof(RenderData::VoxelBatch), false,
			false, "Groups");
		gi->ConstantBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(sizeof(RenderData::GlobalIlluminationConstants))
			.setIsConstantBuffer(true)
			.setIsVolatile(true)
			.setMaxVersions(c_FrameConstantVersions)
			.setDebugName(name + "Constants"));
		created = created && gi->ConstantBuffer;
		gi->RepeatSampler = m_Device->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap));
		created = created && gi->RepeatSampler;
		for (GlobalIlluminationResources::CounterReadback& readback : gi->Readbacks)
		{
			readback.Readback = CreateScope<BufferReadback>(m_Device, 4 * sizeof(uint32_t), name + "CountersReadback");
			created = created && readback.Readback->IsValid();
		}

		// The voxelizer's layouts: the GI's constants (b1) and push constants (b2), the scene's instances and tables, the geometry
		// pool, the frame's work; the level's textures, the scratch, the tiles and (but in the tiles' layout) their arguments.
		const auto makeLayout = [&](bool tileArguments)
		{
			nvrhi::BindingLayoutDesc desc;
			desc.visibility = nvrhi::ShaderType::Compute;
			desc.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),
				nvrhi::BindingLayoutItem::PushConstants(2, sizeof(RenderData::VoxelPassParameters)),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(30),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(31),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(32),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(33),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(34),
				nvrhi::BindingLayoutItem::RawBuffer_SRV(35),
				nvrhi::BindingLayoutItem::RawBuffer_SRV(36),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(37),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(38),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(39),
				nvrhi::BindingLayoutItem::Texture_UAV(0),
				nvrhi::BindingLayoutItem::Texture_UAV(1),
				nvrhi::BindingLayoutItem::Texture_UAV(2),
				nvrhi::BindingLayoutItem::RawBuffer_UAV(3),
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4),
				nvrhi::BindingLayoutItem::RawBuffer_UAV(6)
			};
			if (tileArguments)
				desc.bindings.push_back(nvrhi::BindingLayoutItem::RawBuffer_UAV(5));
			nvrhi::BindingLayoutHandle handle = m_Device->createBindingLayout(desc);
			created = created && handle;
			return handle;
		};
		gi->VoxelLayout = makeLayout(true);
		gi->TilesLayout = makeLayout(false);
		if (created)
		{
			gi->TrianglesPipeline = CreateComputePipeline("GI/VoxelTriangles.comp", gi->VoxelLayout);
			gi->TilesPipeline = CreateComputePipeline("GI/VoxelTiles.comp", gi->TilesLayout);
			gi->FillPipeline = CreateComputePipeline("GI/VoxelFill.comp", gi->VoxelLayout);
			gi->FinalizePipeline = CreateComputePipeline("GI/VoxelFinalize.comp", gi->VoxelLayout);
			created = gi->TrianglesPipeline && gi->TilesPipeline && gi->FillPipeline && gi->FinalizePipeline;
		}
		gi->VoxelSets.resize(layout.Levels.size());
		gi->TilesSets.resize(layout.Levels.size());

		if (!created)
		{
			std::vector<nvrhi::ResourceHandle> released;
			m_GI = std::move(gi);
			ReleaseGlobalIllumination(released);
			Renderer::ReleaseDeferred(std::move(released));
			m_Errors.Report("failed to create global illumination's resources; rendering without it");
			return false;
		}
		gi->MemoryBytes = bytes;
		m_GI = std::move(gi);
		return true;
	}

	void SceneRenderer::ReleaseGlobalIllumination(std::vector<nvrhi::ResourceHandle>& released)
	{
		if (!m_GI)
			return;
		auto release = [&released](auto& handle)
		{
			if (handle)
				released.emplace_back(handle.Get());
			handle = nullptr;
		};
		GlobalIlluminationResources& gi = *m_GI;
		for (GlobalIlluminationResources::Level& level : gi.Levels)
		{
			release(level.Material);
			release(level.Emission);
			release(level.Normal);
		}
		release(gi.ConstantBuffer);
		release(gi.Scratch);
		release(gi.Tiles);
		release(gi.TileArguments);
		release(gi.Counters);
		release(gi.Regions);
		release(gi.Items);
		release(gi.Groups);
		release(gi.Materials);
		release(gi.RepeatSampler);
		for (nvrhi::BindingSetHandle& set : gi.VoxelSets)
			release(set);
		for (nvrhi::BindingSetHandle& set : gi.TilesSets)
			release(set);
		release(gi.ViewSet);
		release(gi.TrianglesPipeline);
		release(gi.TilesPipeline);
		release(gi.FillPipeline);
		release(gi.FinalizePipeline);
		release(gi.ViewPipeline);
		release(gi.VoxelLayout);
		release(gi.TilesLayout);
		release(gi.ViewLayout);
		m_GI.reset();
		m_GIFrame = false;
	}

	void SceneRenderer::PrepareGlobalIllumination(const RenderScene& renderScene, const Ref<AssetManagerBase>& assets, const SceneCamera& camera)
	{
		ST_PROFILE_FUNCTION();
		GlobalIlluminationResources& gi = *m_GI;
		const RendererVoxelSource source(renderScene, *m_SceneGPU, assets.get());
		const VoxelClipmapSettings settings { m_Specification.GlobalIllumination.VoxelizeTrianglesPerFrame, m_Specification.GlobalIllumination.VoxelizeTrianglesOnCut };
		const glm::dvec3 cameraPosition(camera.Position);
		if (!gi.Recorded)
			gi.Clipmap->Reset();
		gi.Recorded = false;
		gi.Clipmap->BuildFrame(renderScene, source, cameraPosition, settings, gi.Frame);

		// The levels as the frame leaves them, camera-relative (subtracted in double).
		RenderData::GlobalIlluminationConstants& constants = gi.Constants;
		constants = {};
		const uint32_t levels = static_cast<uint32_t>(gi.Layout.Levels.size());
		const glm::uvec3 dimensions = gi.Layout.Levels.front().Dimensions;
		constants.Dimensions = glm::uvec4(dimensions, levels);
		constants.Batch = glm::uvec4(gi.Layout.BatchVoxels, 0, 0, 0);
		for (uint32_t level = 0; level < levels; level++)
		{
			const VoxelClipmap::LevelState& state = gi.Clipmap->GetLevel(level);
			const double voxelSize = static_cast<double>(gi.Layout.Levels[level].VoxelSize);
			RenderData::GIVoxelLevelData& data = constants.Levels[level];
			const glm::vec3 origin = glm::vec3(glm::dvec3(state.Window) * voxelSize - cameraPosition);
			const bool valid = !state.Valid.IsEmpty();
			data.Origin = glm::vec4(origin, gi.Layout.Levels[level].VoxelSize);
			data.Texel = glm::ivec4(glm::ivec3(GlobalIllumination::GetTexel(state.Window, dimensions)), valid ? 1 : 0);
			data.ValidMinimum = glm::ivec4(glm::ivec3(state.Valid.Minimum - state.Window), 0);
			data.ValidMaximum = glm::ivec4(glm::ivec3(state.Valid.Maximum - state.Window), 0);
			data.Anchor = glm::vec4(glm::vec3(glm::dvec3(state.Anchor) * voxelSize), 0.0f);
		}

		const VoxelFrame& work = gi.Frame;
		m_Stats.GlobalIllumination = true;
		m_Stats.GlobalIlluminationPending = work.Pending;
		m_Stats.GlobalIlluminationSettled = !work.Pending && work.QueuedRegions == 0;
		m_Stats.GIVoxelizedTriangles = work.Triangles;
		m_Stats.GIQueuedRegions = work.QueuedRegions;
		m_Stats.GIWaitingProxies = work.WaitingProxies;
		m_Stats.GIMovingProxies = work.MovingProxies;
		m_Stats.GIUnvoxelizedSubmeshes = work.UnvoxelizedSubmeshes;
		if (work.UnvoxelizedSubmeshes > 0 && !gi.UnvoxelizedLogged)
		{
			ST_CORE_WARN("SceneRenderer '{}': global illumination leaves out submeshes past a mesh's {}th (or past a frame's limits): {} in "
				"this frame's regions (GIUnvoxelizedSubmeshes); a closed mesh missing some is voxelized open", m_Specification.DebugName,
				RenderData::c_VoxelItemMaxSubmesh + 1, work.UnvoxelizedSubmeshes);
			gi.UnvoxelizedLogged = true;
		}
	}

	void SceneRenderer::RecordGlobalIllumination(nvrhi::ICommandList* commandList)
	{
		ST_PROFILE_FUNCTION();
		GlobalIlluminationResources& gi = *m_GI;
		const VoxelFrame& frame = gi.Frame;
		commandList->writeBuffer(gi.ConstantBuffer, &gi.Constants, sizeof(gi.Constants));
		if (!gi.Cleared)
		{
			// Their first frame: no voxel holds garbage, also where no region reached yet.
			for (GlobalIlluminationResources::Level& level : gi.Levels)
			{
				commandList->clearTextureFloat(level.Material, nvrhi::AllSubresources, nvrhi::Color(0.0f));
				commandList->clearTextureUInt(level.Emission, nvrhi::AllSubresources, 0);
				commandList->clearTextureFloat(level.Normal, nvrhi::AllSubresources, nvrhi::Color(0.0f));
			}
			gi.Cleared = true;
		}
		commandList->clearBufferUInt(gi.Counters, 0);

		// The material table: whole, when it changed (a buffer that grows makes the binding sets again).
		const std::span<const RenderData::GIMaterial> materials = gi.Clipmap->GetMaterials();
		if (materials.size() > gi.MaterialCapacity || !gi.Materials)
		{
			uint32_t capacity = std::max<uint32_t>(gi.MaterialCapacity, 256);
			while (capacity < materials.size())
				capacity *= 2;
			nvrhi::BufferDesc desc;
			desc.byteSize = static_cast<uint64_t>(capacity) * sizeof(RenderData::GIMaterial);
			desc.structStride = sizeof(RenderData::GIMaterial);
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = m_Specification.DebugName + ".GI.Materials";
			nvrhi::BufferHandle buffer = m_Device->createBuffer(desc);
			if (!buffer)
			{
				m_Errors.Report("failed to create global illumination's material table; its voxels wait");
				return;
			}
			if (gi.Materials)
				Renderer::ReleaseDeferred({ nvrhi::ResourceHandle(gi.Materials.Get()) });
			gi.MemoryBytes += desc.byteSize - static_cast<uint64_t>(gi.MaterialCapacity) * sizeof(RenderData::GIMaterial);
			gi.Materials = buffer;
			gi.MaterialCapacity = capacity;
			if (!materials.empty())
				commandList->writeBuffer(gi.Materials, materials.data(), materials.size_bytes());
		}
		else if (frame.MaterialsChanged && !materials.empty())
		{
			commandList->writeBuffer(gi.Materials, materials.data(), materials.size_bytes());
		}
		if (frame.Batches.empty())
		{
			gi.Recorded = true;
			return;
		}

		// The geometry pool's tables, as the voxelizer reads them (the GPU-driven culling may not have run this frame).
		GeometryPool& pool = *Renderer::GetGeometryPool();
		if (!pool.RecordTableUploads(commandList))
		{
			m_Errors.Report("the geometry pool's tables could not be uploaded (see the previous error); global illumination's voxels wait");
			return;
		}
		if (gi.BoundScene != m_SceneGPU || gi.BoundSceneBuffers != m_SceneGPU->GetBufferVersion() || gi.BoundPoolBuffers != pool.GetBufferVersion()
			|| gi.BoundPoolTables != pool.GetTableVersion() || gi.BoundMaterials != gi.Materials.Get() || !gi.VoxelSets[0])
		{
			for (uint32_t level = 0; level < gi.Levels.size(); level++)
			{
				const GlobalIlluminationResources::Level& textures = gi.Levels[level];
				nvrhi::BindingSetDesc desc;
				desc.bindings = {
					nvrhi::BindingSetItem::ConstantBuffer(1, gi.ConstantBuffer),
					nvrhi::BindingSetItem::PushConstants(2, sizeof(RenderData::VoxelPassParameters)),
					nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_SceneGPU->GetInstanceBuffer()),
					nvrhi::BindingSetItem::StructuredBuffer_SRV(30, gi.Materials),
					nvrhi::BindingSetItem::StructuredBuffer_SRV(31, m_SceneGPU->GetMeshTableBuffer()),
					nvrhi::BindingSetItem::StructuredBuffer_SRV(32, pool.GetEntryBuffer()),
					nvrhi::BindingSetItem::StructuredBuffer_SRV(33, pool.GetSubmeshBuffer()),
					nvrhi::BindingSetItem::StructuredBuffer_SRV(34, pool.GetLODBuffer()),
					nvrhi::BindingSetItem::RawBuffer_SRV(35, pool.GetPositionBuffer(GeometryArena::Static)),
					nvrhi::BindingSetItem::RawBuffer_SRV(36, pool.GetIndexBuffer()),
					nvrhi::BindingSetItem::StructuredBuffer_SRV(37, gi.Regions),
					nvrhi::BindingSetItem::StructuredBuffer_SRV(38, gi.Items),
					nvrhi::BindingSetItem::StructuredBuffer_SRV(39, gi.Groups),
					nvrhi::BindingSetItem::Texture_UAV(0, textures.Material, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::Texture3D),
					nvrhi::BindingSetItem::Texture_UAV(1, textures.Emission, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::Texture3D),
					nvrhi::BindingSetItem::Texture_UAV(2, textures.Normal, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::Texture3D),
					nvrhi::BindingSetItem::RawBuffer_UAV(3, gi.Scratch),
					nvrhi::BindingSetItem::StructuredBuffer_UAV(4, gi.Tiles),
					nvrhi::BindingSetItem::RawBuffer_UAV(6, gi.Counters)
				};
				gi.TilesSets[level] = m_Device->createBindingSet(desc, gi.TilesLayout);
				desc.bindings.push_back(nvrhi::BindingSetItem::RawBuffer_UAV(5, gi.TileArguments));
				gi.VoxelSets[level] = m_Device->createBindingSet(desc, gi.VoxelLayout);
				if (!gi.VoxelSets[level] || !gi.TilesSets[level])
				{
					gi.VoxelSets[0] = nullptr; // Made again next frame
					m_Errors.Report("failed to create global illumination's voxelizer bindings; its voxels wait");
					return;
				}
			}
			gi.BoundScene = m_SceneGPU;
			gi.BoundSceneBuffers = m_SceneGPU->GetBufferVersion();
			gi.BoundPoolBuffers = pool.GetBufferVersion();
			gi.BoundPoolTables = pool.GetTableVersion();
			gi.BoundMaterials = gi.Materials.Get();
		}

		commandList->writeBuffer(gi.Regions, frame.Regions.data(), frame.Regions.size() * sizeof(RenderData::VoxelRegion));
		if (!frame.Items.empty())
		{
			commandList->writeBuffer(gi.Items, frame.Items.data(), frame.Items.size() * sizeof(RenderData::VoxelWorkItem));
			commandList->writeBuffer(gi.Groups, frame.Groups.data(), frame.Groups.size() * sizeof(RenderData::VoxelBatch));
		}

		// Consecutive passes through one binding set read each other's writes to the scratch: explicit barriers between them.
		const auto scratchBarrier = [&]()
		{
			commandList->setBufferState(gi.Scratch, nvrhi::ResourceStates::UnorderedAccess);
			commandList->setBufferState(gi.Tiles, nvrhi::ResourceStates::UnorderedAccess);
			commandList->setBufferState(gi.Counters, nvrhi::ResourceStates::UnorderedAccess);
			commandList->commitBarriers();
		};
		for (const VoxelFrameBatch& batch : frame.Batches)
		{
			RenderData::VoxelPassParameters parameters = {};
			parameters.Level = batch.Level;
			parameters.FirstRegion = batch.FirstRegion;
			parameters.RegionCount = batch.RegionCount;
			parameters.FirstGroup = batch.FirstGroup;
			parameters.Voxels = batch.Voxels;
			parameters.Columns = batch.Columns;
			parameters.GroupCount = batch.GroupCount;
			nvrhi::IBindingSet* voxelSet = gi.VoxelSets[batch.Level];

			commandList->clearBufferUInt(gi.Scratch, 0);
			const uint32_t arguments[4] = { 0, 1, 1, 0 };
			commandList->writeBuffer(gi.TileArguments, arguments, sizeof(arguments));

			nvrhi::ComputeState state;
			state.bindings = { voxelSet };
			// The triangles, in dispatches of at most c_MaxDispatchGroups groups.
			state.pipeline = gi.TrianglesPipeline;
			commandList->setComputeState(state);
			for (uint32_t first = 0; first < batch.GroupCount; first += c_MaxDispatchGroups)
			{
				RenderData::VoxelPassParameters part = parameters;
				part.FirstGroup = batch.FirstGroup + first;
				part.GroupCount = std::min(c_MaxDispatchGroups, batch.GroupCount - first);
				commandList->setPushConstants(&part, sizeof(part));
				commandList->dispatch(part.GroupCount);
			}
			scratchBarrier();

			// The large triangles' tiles, as many groups as VoxelTriangles listed.
			nvrhi::ComputeState tilesState;
			tilesState.pipeline = gi.TilesPipeline;
			tilesState.bindings = { gi.TilesSets[batch.Level] };
			tilesState.indirectParams = gi.TileArguments;
			commandList->setComputeState(tilesState);
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->dispatchIndirect(0);
			scratchBarrier();

			state.pipeline = gi.FillPipeline;
			commandList->setComputeState(state);
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->dispatch(DivideUp(batch.Columns, c_GroupSize));
			scratchBarrier();

			state.pipeline = gi.FinalizePipeline;
			commandList->setComputeState(state);
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->dispatch(DivideUp(batch.Voxels, c_GroupSize));
			scratchBarrier();
		}

		gi.Recorded = true;
		// The counters into the next free readback (a copy still under way leaves the frame unread).
		GlobalIlluminationResources::CounterReadback& readback = gi.Readbacks[gi.ReadbackIndex];
		if (!readback.Readback->IsPending() || readback.Readback->IsReady())
		{
			readback.Readback->Record(commandList, gi.Counters, 0, 4 * sizeof(uint32_t));
			readback.Render = m_RenderCount + 1;
			gi.ReadbackRecorded = true;
		}
	}

	void SceneRenderer::SubmitGlobalIlluminationReadback()
	{
		if (!m_GI || !m_GI->ReadbackRecorded)
			return;
		m_GI->Readbacks[m_GI->ReadbackIndex].Readback->Submit();
		m_GI->ReadbackIndex = (m_GI->ReadbackIndex + 1) % static_cast<uint32_t>(m_GI->Readbacks.size());
		m_GI->ReadbackRecorded = false;
	}

	void SceneRenderer::ResolveGlobalIlluminationCounters(bool wait)
	{
		if (!m_GI)
			return;
		GlobalIlluminationResources::CounterReadback* newest = nullptr;
		for (GlobalIlluminationResources::CounterReadback& entry : m_GI->Readbacks)
		{
			if (!entry.Readback->IsPending() || (m_GPUStats.HasGICounters && entry.Render <= m_GPUStats.GICountersRender))
				continue;
			if (wait)
				entry.Readback->Wait();
			if (entry.Readback->IsReady() && (!newest || entry.Render > newest->Render))
				newest = &entry;
		}
		uint32_t counters[4] = {};
		if (!newest || !newest->Readback->Read(counters, sizeof(counters)))
			return;
		m_GPUStats.HasGICounters = true;
		m_GPUStats.GICountersRender = newest->Render;
		m_GPUStats.GITiles = counters[0];
		m_GPUStats.GITileOverflow = counters[1];
	}

	bool SceneRenderer::DrawVoxelView(nvrhi::ICommandList* commandList, const SceneRenderOptions& options, const nvrhi::Viewport& viewport)
	{
		if (!m_GI)
			return false;
		GlobalIlluminationResources& gi = *m_GI;
		if (!gi.ViewPipeline)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.bindings = { nvrhi::BindingLayoutItem::VolatileConstantBuffer(0), nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),
				nvrhi::BindingLayoutItem::PushConstants(2, sizeof(RenderData::VoxelViewParameters)), nvrhi::BindingLayoutItem::Sampler(0) };
			for (uint32_t slot = 0; slot < 2 * RenderData::c_GIMaxVoxelLevels; slot++)
				layout.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(slot));
			gi.ViewLayout = m_Device->createBindingLayout(layout);
			nvrhi::IShader* pixelShader = Renderer::GetShaderLibrary().Get("GI/VoxelView.frag");
			ST_CORE_VERIFY(pixelShader, "SceneRenderer: the voxels view's shader is missing");
			if (gi.ViewLayout)
			{
				// Replaces the image without blending or depth.
				nvrhi::GraphicsPipelineDesc desc;
				desc.primType = nvrhi::PrimitiveType::TriangleList;
				desc.VS = m_FullscreenVertexShader;
				desc.PS = pixelShader;
				desc.bindingLayouts = { gi.ViewLayout };
				desc.renderState.rasterState.setCullNone();
				desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
				gi.ViewPipeline = m_Device->createGraphicsPipeline(desc, m_OutputFramebuffer->getFramebufferInfo());
			}
		}
		if (gi.ViewPipeline && !gi.ViewSet)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer), nvrhi::BindingSetItem::ConstantBuffer(1, gi.ConstantBuffer),
				nvrhi::BindingSetItem::PushConstants(2, sizeof(RenderData::VoxelViewParameters)), nvrhi::BindingSetItem::Sampler(0, gi.RepeatSampler) };
			for (uint32_t level = 0; level < RenderData::c_GIMaxVoxelLevels; level++)
			{
				const bool present = level < gi.Levels.size();
				desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(level, present ? gi.Levels[level].Material.Get() : Renderer::GetBlack3DTexture(),
					nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::Texture3D));
				desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(RenderData::c_GIMaxVoxelLevels + level,
					present ? gi.Levels[level].Normal.Get() : Renderer::GetBlack3DTexture(), nvrhi::Format::UNKNOWN, nvrhi::AllSubresources,
					nvrhi::TextureDimension::Texture3D));
			}
			gi.ViewSet = m_Device->createBindingSet(desc, gi.ViewLayout);
		}
		if (!gi.ViewPipeline || !gi.ViewSet)
		{
			m_Errors.Report("failed to create the voxels view's pipeline or bindings");
			return false;
		}
		nvrhi::GraphicsState state;
		state.pipeline = gi.ViewPipeline;
		state.framebuffer = m_OutputFramebuffer;
		state.viewport.addViewportAndScissorRect(viewport);
		state.bindings = { gi.ViewSet };
		commandList->setGraphicsState(state);
		const RenderData::VoxelViewParameters parameters = { PackDisplayColor(options.LightingColors.TwoSided), PackDisplayColor(options.LightingColors.Background), 0, 0 };
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		return true;
	}

	bool SceneRenderer::ReadGlobalIlluminationVoxels(uint32_t level, GlobalIlluminationTexture which, std::vector<uint8_t>& outTexels) const
	{
		outTexels.clear();
		if (!m_GI || level >= m_GI->Levels.size())
			return false;
		const GlobalIlluminationResources::Level& textures = m_GI->Levels[level];
		nvrhi::ITexture* texture = which == GlobalIlluminationTexture::Material ? textures.Material.Get()
			: which == GlobalIlluminationTexture::Emission ? textures.Emission.Get() : textures.Normal.Get();
		const nvrhi::TextureDesc& source = texture->getDesc();
		nvrhi::TextureDesc stagingDesc;
		stagingDesc.dimension = nvrhi::TextureDimension::Texture3D;
		stagingDesc.width = source.width;
		stagingDesc.height = source.height;
		stagingDesc.depth = source.depth;
		stagingDesc.format = source.format;
		stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
		stagingDesc.keepInitialState = true;
		stagingDesc.debugName = "GIVoxelReadback";
		nvrhi::StagingTextureHandle staging = m_Device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
		nvrhi::CommandListHandle commandList = m_Device->createCommandList();
		if (!staging || !commandList)
			return false;
		// One copy per depth slice: staging copies take a slice at a time.
		commandList->open();
		for (uint32_t z = 0; z < source.depth; z++)
		{
			const nvrhi::TextureSlice slice = nvrhi::TextureSlice().setOrigin(0, 0, static_cast<int>(z)).setSize(source.width, source.height, 1);
			commandList->copyTexture(staging, slice, texture, slice);
		}
		commandList->close();
		m_Device->executeCommandList(commandList);
		m_Device->waitForIdle();
		size_t rowPitch = 0;
		const uint8_t* mapped = static_cast<const uint8_t*>(m_Device->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
		if (!mapped)
			return false;
		const size_t texelBytes = nvrhi::getFormatInfo(source.format).bytesPerBlock;
		const size_t row = static_cast<size_t>(source.width) * texelBytes;
		outTexels.resize(row * source.height * source.depth);
		for (uint32_t z = 0; z < source.depth; z++)
		{
			for (uint32_t y = 0; y < source.height; y++)
				std::memcpy(outTexels.data() + (static_cast<size_t>(z) * source.height + y) * row, mapped + (static_cast<size_t>(z) * source.height + y) * rowPitch, row);
		}
		m_Device->unmapStagingTexture(staging);
		return true;
	}

}
