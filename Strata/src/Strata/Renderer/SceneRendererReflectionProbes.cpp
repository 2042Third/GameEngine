#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Assert.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/ShaderLibrary.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Renderer/TextureStreamer.h"
#include "Strata/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <limits>

// Reflection probes (ReflectionProbeComponent; DeveloperGuide "Reflections"). Each frame the renderer gathers the probes in
// view, keeps the strongest SceneRendererSpecification::MaxReflectionProbes (higher priority, then the smaller box) and
// shades with those whose capture is in its cube array. A capture (an equirectangular HDR texture asset) is requested
// through the frame's resolve cache, held whole in the texture streamer until it is converted (TextureStreamer::Hold: nothing
// draws it, so nothing else would raise its levels), then converted on the GPU into a slot: a cube with its levels, filtered
// into the slot's prefiltered levels as the sky's environment is, and projected onto the slot's irradiance coefficients. The
// slot keeps it while the scene shows (the probe cache: a third cache besides the environment's and the glyph atlases' that
// keeps what assets became across frames), so the texture drops to its mip tail again.

namespace Strata
{

	namespace
	{

		constexpr nvrhi::Format c_ReflectionProbeFormat = nvrhi::Format::RGBA16_FLOAT;
		// The capture's cube is projected onto the irradiance coefficients from this level (32 texels across): irradiance is
		// smooth, and the projection runs in one work group.
		constexpr uint32_t c_ProbeIrradianceLevel = 2;
		// Half the sky's samples: a probe's cube is a quarter of the sky's size, and filtered importance sampling reads its levels.
		constexpr uint32_t c_ProbePrefilterSamples = 256;

		uint32_t ProbeMipCount(uint32_t size)
		{
			uint32_t count = 1;
			while (size > 1)
			{
				size >>= 1;
				count++;
			}
			return count;
		}

		// A probe in view, ranked for the frame's list.
		struct ProbeCandidate
		{
			int32_t Priority = 0;
			double Volume = 0.0;
			uint32_t Entity = 0;
			AssetHandle Capture = 0;
			glm::dvec3 Center = glm::dvec3(0.0);
			glm::vec3 Axes[3] = {};
			glm::vec3 Extents = glm::vec3(0.0f);
			float BlendDistance = 0.0f;
			float Intensity = 0.0f;
			bool BoxProjection = true;
		};

		// The strongest first: the higher priority, then the smaller box (the more specific place), then the entity.
		bool IsStronger(const ProbeCandidate& a, const ProbeCandidate& b)
		{
			if (a.Priority != b.Priority)
				return a.Priority > b.Priority;
			if (a.Volume != b.Volume)
				return a.Volume < b.Volume;
			return a.Entity < b.Entity;
		}

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		float FiniteAtLeast(float value, float minimum)
		{
			return std::isfinite(value) ? std::max(value, minimum) : minimum;
		}

	}

	nvrhi::ITexture* SceneRenderer::GetBoundReflectionProbes() const
	{
		return m_ReflectionProbes ? m_ReflectionProbes->Array.Get() : Renderer::GetBlackCubeArrayTexture();
	}

	void SceneRenderer::ReleaseReflectionProbes()
	{
		if (m_ReflectionProbes)
		{
			// Frames still in flight may sample the array: released once the GPU is done with them.
			std::vector<nvrhi::ResourceHandle> released;
			released.emplace_back(m_ReflectionProbes->Array.Get());
			released.emplace_back(m_ReflectionProbes->Scratch.Get());
			Renderer::ReleaseDeferred(std::move(released));
			m_ReflectionProbes.reset();
		}
		m_ReflectionProbeHolds.clear();
		m_ReflectionProbeList.clear();
		m_ReflectionProbeConversions.clear();
	}

	bool SceneRenderer::EnsureReflectionProbeResources()
	{
		const uint32_t slots = std::min(m_Specification.MaxReflectionProbes, RenderData::c_MaxReflectionProbes);
		if (m_ReflectionProbes && static_cast<uint32_t>(m_ReflectionProbes->Slots.size()) == slots)
			return true;
		if (!m_ProbeIrradiancePipeline)
		{
			nvrhi::BindingLayoutDesc layoutDesc;
			layoutDesc.visibility = nvrhi::ShaderType::Compute;
			layoutDesc.bindings = {
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
				nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::ProbeIrradianceParameters))
			};
			m_ProbeIrradianceLayout = m_Device->createBindingLayout(layoutDesc);
			nvrhi::ComputePipelineDesc desc;
			desc.CS = Renderer::GetShaderLibrary().Get("IBL/ProbeIrradiance.comp");
			desc.bindingLayouts = { m_ProbeIrradianceLayout };
			m_ProbeIrradiancePipeline = desc.CS && m_ProbeIrradianceLayout ? m_Device->createComputePipeline(desc) : nullptr;
			if (!m_ProbeIrradiancePipeline)
			{
				m_Errors.Report("failed to create the reflection probes' irradiance pipeline; the scene is lit without its probes");
				return false;
			}
		}

		nvrhi::TextureDesc arrayDesc;
		arrayDesc.width = RenderData::c_ReflectionProbeSize;
		arrayDesc.height = RenderData::c_ReflectionProbeSize;
		arrayDesc.arraySize = 6 * slots;
		arrayDesc.mipLevels = RenderData::c_ReflectionProbeMipCount;
		arrayDesc.dimension = nvrhi::TextureDimension::TextureCubeArray;
		arrayDesc.format = c_ReflectionProbeFormat;
		arrayDesc.isUAV = true;
		arrayDesc.debugName = m_Specification.DebugName + ".ReflectionProbes";
		arrayDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		arrayDesc.keepInitialState = true;
		nvrhi::TextureDesc scratchDesc = arrayDesc;
		scratchDesc.arraySize = 6;
		scratchDesc.mipLevels = ProbeMipCount(RenderData::c_ReflectionProbeSize);
		scratchDesc.dimension = nvrhi::TextureDimension::TextureCube;
		scratchDesc.debugName = m_Specification.DebugName + ".ReflectionProbeCapture";
		nvrhi::TextureHandle array = m_Device->createTexture(arrayDesc);
		nvrhi::TextureHandle scratch = array ? m_Device->createTexture(scratchDesc) : nullptr;
		if (!array || !scratch)
		{
			m_Errors.Report(fmt::format("failed to create the cube array of {} reflection probes; the scene is lit without its probes", slots));
			return false;
		}
		ReleaseReflectionProbes();
		ReflectionProbeState state;
		state.Array = std::move(array);
		state.Scratch = std::move(scratch);
		state.Slots.resize(slots);
		m_ReflectionProbes = std::move(state);
		return true;
	}

	bool SceneRenderer::GatherReflectionProbes(Scene& scene, const SceneCamera& camera)
	{
		ST_PROFILE_FUNCTION();
		m_ReflectionProbeList.clear();
		m_ReflectionProbeConversions.clear();
		const uint32_t maxProbes = std::min(m_Specification.MaxReflectionProbes, RenderData::c_MaxReflectionProbes);

		// The probes in view; any probe with a capture at all keeps the cache (its slots) for when it comes into view.
		std::vector<ProbeCandidate> candidates;
		bool sceneHasProbes = false;
		const Frustum frustum(camera.Projection * camera.View);
		for (auto [entity, probe, world] : scene.GetRegistry().view<ReflectionProbeComponent, WorldTransformComponent>().each())
		{
			if (maxProbes == 0 || !world.ActiveInHierarchy || !probe.Capture.IsValid())
				continue;
			sceneHasProbes = true;
			ProbeCandidate candidate;
			candidate.Priority = probe.Priority;
			candidate.Entity = static_cast<uint32_t>(entity);
			candidate.Capture = probe.Capture;
			// The box follows the entity: its axes are the world matrix's, its extents scaled with them.
			bool valid = true;
			for (int axis = 0; axis < 3; axis++)
			{
				const glm::vec3 column = glm::vec3(world.Matrix[axis]);
				const float length = glm::length(column);
				valid = valid && std::isfinite(length) && length > 1e-6f;
				candidate.Axes[axis] = valid ? column / length : glm::vec3(0.0f);
				candidate.Extents[axis] = FiniteAtLeast(probe.Extents[axis], ReflectionProbeComponent::c_MinExtent) * (valid ? length : 0.0f);
			}
			candidate.Center = glm::dvec3(glm::vec3(world.Matrix[3]));
			if (!valid || !IsFinite(glm::vec3(world.Matrix[3])))
				continue;
			candidate.Volume = static_cast<double>(candidate.Extents.x) * candidate.Extents.y * candidate.Extents.z;
			candidate.BlendDistance = FiniteAtLeast(probe.BlendDistance, 0.0f);
			candidate.Intensity = FiniteAtLeast(probe.Intensity, 0.0f);
			candidate.BoxProjection = probe.BoxProjection;
			if (!frustum.IsSphereVisible(glm::vec3(world.Matrix[3]), glm::length(candidate.Extents)))
				continue;
			candidates.push_back(candidate);
		}
		if (!sceneHasProbes)
		{
			ReleaseReflectionProbes();
			return true;
		}
		if (candidates.empty())
		{
			m_ReflectionProbeHolds.clear(); // Nothing in view waits for its capture
			return true;
		}
		if (!EnsureReflectionProbeResources())
		{
			m_Stats.ReflectionProbesPending = static_cast<uint32_t>(std::min<size_t>(candidates.size(), maxProbes));
			return false;
		}
		std::sort(candidates.begin(), candidates.end(), IsStronger);
		if (candidates.size() > maxProbes)
			candidates.resize(maxProbes);

		ReflectionProbeState& state = *m_ReflectionProbes;
		const uint64_t gather = ++m_ReflectionProbeGathers;
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		TextureStreamer* streamer = manager ? TextureStreamer::Find(*manager) : nullptr;
		uint32_t conversionsLeft = m_Specification.SpreadLightingUpdates ? c_ReflectionProbeConversionsPerFrame : std::numeric_limits<uint32_t>::max();
		// The holds the frame keeps: those of captures still waiting (the others end).
		std::vector<AssetHandle> waiting;
		auto hold = [&](AssetHandle capture)
		{
			waiting.push_back(capture);
			if (!streamer)
				return;
			Ref<TextureHold>& held = m_ReflectionProbeHolds[capture];
			if (!held)
				held = CreateRef<TextureHold>(TextureHold { capture });
			// Again in every frame it waits: a hold placed in another manager's streamer (a project opened since) lapsed with it.
			streamer->Hold(held);
		};

		const glm::dvec3 cameraPosition = glm::dvec3(camera.Position);
		for (const ProbeCandidate& candidate : candidates)
		{
			const Ref<Texture> texture = m_Assets.Resolve<Texture>(candidate.Capture, AssetPriority::High);
			// A capture that is not on its way (an unknown or failed handle, another type of asset) lights nothing: it takes no
			// slot and counts as pending for nobody, so captures and bakes do not wait for it.
			if (!texture && (!manager || manager->GetAssetState(candidate.Capture) != AssetState::Loading))
				continue;
			// The capture's slot: the one holding it, else a free one, else the least recently used one this gather does not use.
			uint32_t slot = std::numeric_limits<uint32_t>::max();
			uint32_t replaceable = std::numeric_limits<uint32_t>::max();
			for (uint32_t index = 0; index < state.Slots.size(); index++)
			{
				const ReflectionProbeSlot& candidateSlot = state.Slots[index];
				if (candidateSlot.Capture == candidate.Capture)
				{
					slot = index;
					break;
				}
				if (candidateSlot.LastUsed == gather)
					continue;
				if (replaceable == std::numeric_limits<uint32_t>::max() || !candidateSlot.Capture.IsValid()
					|| (state.Slots[replaceable].Capture.IsValid() && candidateSlot.LastUsed < state.Slots[replaceable].LastUsed))
					replaceable = index;
			}
			if (slot == std::numeric_limits<uint32_t>::max())
			{
				// At most as many probes as slots are kept, so a slot this gather does not use is left.
				ST_CORE_ASSERT(replaceable != std::numeric_limits<uint32_t>::max(), "No reflection probe slot left");
				slot = replaceable;
				state.Slots[slot] = ReflectionProbeSlot { candidate.Capture, {}, 0 };
			}
			ReflectionProbeSlot& probeSlot = state.Slots[slot];
			probeSlot.LastUsed = gather;

			const bool converted = texture && probeSlot.Source.lock() == texture;
			const bool convertingNow = std::any_of(m_ReflectionProbeConversions.begin(), m_ReflectionProbeConversions.end(),
				[slot](const ReflectionProbeConversion& conversion) { return conversion.Slot == slot; });
			if (!converted && !convertingNow)
			{
				// The conversion reads the whole capture: until it is loaded with every level, the sky lights the box.
				if (!texture || !texture->GetGPUTexture() || texture->GetResidentLevel() > 0 || conversionsLeft == 0)
				{
					hold(candidate.Capture);
					if (texture && texture->GetResidentLevel() > 0)
						m_Assets.CountPending(candidate.Capture);
					m_Stats.ReflectionProbesPending++;
					continue;
				}
				conversionsLeft--;
				m_ReflectionProbeConversions.push_back(ReflectionProbeConversion { slot, texture });
			}

			RenderData::ReflectionProbeData data;
			data.CenterBlend = glm::vec4(glm::vec3(candidate.Center - cameraPosition), candidate.BlendDistance);
			data.AxisX = glm::vec4(candidate.Axes[0], candidate.Extents.x);
			data.AxisY = glm::vec4(candidate.Axes[1], candidate.Extents.y);
			data.AxisZ = glm::vec4(candidate.Axes[2], candidate.Extents.z);
			data.Parameters = glm::vec4(candidate.Intensity, candidate.BoxProjection ? 1.0f : 0.0f, static_cast<float>(slot), 0.0f);
			m_ReflectionProbeList.push_back(data);
		}
		// Captures converted (or no longer wanted) are held no more: their levels may drop to their tail.
		std::erase_if(m_ReflectionProbeHolds, [&](const auto& entry) { return std::find(waiting.begin(), waiting.end(), entry.first) == waiting.end(); });
		m_Stats.ReflectionProbes = static_cast<uint32_t>(m_ReflectionProbeList.size());
		m_Stats.ReflectionProbeConversions = static_cast<uint32_t>(m_ReflectionProbeConversions.size());
		return true;
	}

	void SceneRenderer::UploadReflectionProbes(nvrhi::ICommandList* commandList)
	{
		const uint32_t count = static_cast<uint32_t>(m_ReflectionProbeList.size());
		if (count == 0 && m_UploadedReflectionProbes == 0)
			return; // The buffer lists none already
		// The CPU's part of the buffer (RenderData::ReflectionProbeBuffer from Counts on): the count, then the list.
		struct ProbeList
		{
			glm::uvec4 Counts;
			RenderData::ReflectionProbeData Probes[RenderData::c_MaxReflectionProbes];
		};
		static_assert(sizeof(ProbeList) == sizeof(RenderData::ReflectionProbeBuffer) - RenderData::c_ReflectionProbeListOffset);
		ProbeList list;
		list.Counts = glm::uvec4(count, 0u, 0u, 0u);
		std::copy(m_ReflectionProbeList.begin(), m_ReflectionProbeList.end(), list.Probes);
		commandList->writeBuffer(m_ReflectionProbeBuffer, &list, sizeof(glm::uvec4) + count * sizeof(RenderData::ReflectionProbeData),
			RenderData::c_ReflectionProbeListOffset);
		m_UploadedReflectionProbes = count;
	}

	void SceneRenderer::ConvertReflectionProbes(nvrhi::ICommandList* commandList)
	{
		ST_PROFILE_FUNCTION();
		if (!m_ReflectionProbes)
			return;
		ReflectionProbeState& state = *m_ReflectionProbes;
		const uint32_t size = RenderData::c_ReflectionProbeSize;
		const uint32_t scratchMips = ProbeMipCount(size);
		for (const ReflectionProbeConversion& conversion : m_ReflectionProbeConversions)
		{
			// The capture into the scratch cube's level 0, then its levels (what filtered importance sampling reads).
			DispatchIBL(commandList, m_EquirectToCubePipeline, conversion.Source->GetGPUTexture(), nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D,
				state.Scratch, 0, nvrhi::TextureDimension::Texture2DArray, RenderData::IBLParameters { size, conversion.Source->GetWidth(), 0.0f, 1 }, 6);
			for (uint32_t mip = 1; mip < scratchMips; mip++)
			{
				const uint32_t mipSize = std::max(1u, size >> mip);
				DispatchIBL(commandList, m_CubeDownsamplePipeline, state.Scratch, nvrhi::TextureSubresourceSet(mip - 1, 1, 0, 6),
					nvrhi::TextureDimension::Texture2DArray, state.Scratch, mip, nvrhi::TextureDimension::Texture2DArray,
					RenderData::IBLParameters { mipSize, mipSize * 2, 0.0f, 1 }, 6);
			}
			// The slot's levels, one per roughness step (level 0 a copy of the capture's cube), as the sky's prefiltered map.
			const nvrhi::TextureSubresourceSet wholeScratch(0, scratchMips, 0, 6);
			for (uint32_t mip = 0; mip < RenderData::c_ReflectionProbeMipCount; mip++)
			{
				const uint32_t mipSize = std::max(1u, size >> mip);
				const float roughness = static_cast<float>(mip) / static_cast<float>(RenderData::c_ReflectionProbeMipCount - 1);
				DispatchIBL(commandList, m_PrefilterPipeline, state.Scratch, wholeScratch, nvrhi::TextureDimension::TextureCube, state.Array, mip,
					nvrhi::TextureDimension::Texture2DArray, RenderData::IBLParameters { mipSize, size, roughness, c_ProbePrefilterSamples }, 6,
					conversion.Slot * 6);
			}
			// Its irradiance coefficients, into the slot's part of the probe buffer.
			nvrhi::BindingSetDesc setDesc;
			setDesc.bindings = {
				nvrhi::BindingSetItem::Texture_SRV(0, state.Scratch, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(c_ProbeIrradianceLevel, 1, 0, 6),
					nvrhi::TextureDimension::Texture2DArray),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_ReflectionProbeBuffer),
				nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::ProbeIrradianceParameters))
			};
			nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(setDesc, m_ProbeIrradianceLayout);
			ST_CORE_VERIFY(bindingSet, "SceneRenderer: failed to create a reflection probe irradiance binding set");
			nvrhi::ComputeState computeState;
			computeState.pipeline = m_ProbeIrradiancePipeline;
			computeState.bindings = { bindingSet };
			commandList->setComputeState(computeState);
			RenderData::ProbeIrradianceParameters parameters;
			parameters.SourceSize = std::max(1u, size >> c_ProbeIrradianceLevel);
			parameters.Slot = conversion.Slot;
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->dispatch(1, 1, 1);
			// The slot holds the capture from now on (a frame that fails before this converts it again).
			state.Slots[conversion.Slot].Source = conversion.Source;
		}
		m_ReflectionProbeConversions.clear(); // The command list keeps the captures' GPU textures until it ran
	}

}
