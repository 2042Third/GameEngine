#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Assert.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/RendererQuality.h"
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
		// The capture's cube is projected onto the irradiance coefficients from its 32-texel level (2 at 128, 3 at 256):
		// irradiance is smooth, and the projection runs in one work group.
		constexpr uint32_t c_ProbeIrradianceSize = 32;
		constexpr uint32_t c_NoSlot = std::numeric_limits<uint32_t>::max();

		uint32_t GetProbeIrradianceLevel(uint32_t size)
		{
			return RenderData::GetReflectionProbeMipChain(size) - RenderData::GetReflectionProbeMipChain(c_ProbeIrradianceSize);
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

	void SceneRenderer::ReleaseReflectionProbeState(std::optional<ReflectionProbeState>& state)
	{
		if (!state)
			return;
		// Frames still in flight may sample the array: released once the GPU is done with them.
		std::vector<nvrhi::ResourceHandle> released;
		released.emplace_back(state->Array.Get());
		released.emplace_back(state->Scratch.Get());
		Renderer::ReleaseDeferred(std::move(released));
		state.reset();
	}

	void SceneRenderer::ReleaseReflectionProbes()
	{
		ReleaseReflectionProbeState(m_ReflectionProbes);
		ReleaseReflectionProbeState(m_ReflectionProbesNext);
		m_ReflectionProbeHolds.clear();
		m_ReflectionProbeList.clear();
		m_ReflectionProbeConversions.clear();
	}

	void SceneRenderer::ReleaseStaleReflectionProbeHolds()
	{
		const uint64_t frame = Renderer::GetFrameNumber();
		std::erase_if(m_ReflectionProbeHolds, [frame](const auto& entry) { return entry.second.Frame + 1 < frame; });
	}

	bool SceneRenderer::EnsureReflectionProbeResources()
	{
		const uint32_t slots = std::min(m_Specification.MaxReflectionProbes, RenderData::c_MaxReflectionProbes);
		const uint32_t size = m_Specification.Quality.ReflectionProbeResolution;
		const auto hasSlots = [slots](const std::optional<ReflectionProbeState>& state) { return state && static_cast<uint32_t>(state->Slots.size()) == slots; };
		const auto fits = [&](const std::optional<ReflectionProbeState>& state) { return hasSlots(state) && state->Array->getDesc().width == size; };
		if (fits(m_ReflectionProbes))
		{
			// Back at the shown cubes' size before the new ones took over: those are dropped.
			ReleaseReflectionProbeState(m_ReflectionProbesNext);
			return true;
		}
		if (fits(m_ReflectionProbesNext))
			return true;
		ReleaseReflectionProbeState(m_ReflectionProbesNext); // Made for another size or slot count: of no use now
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
		arrayDesc.width = size;
		arrayDesc.height = size;
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
		scratchDesc.mipLevels = RenderData::GetReflectionProbeMipChain(size);
		scratchDesc.dimension = nvrhi::TextureDimension::TextureCube;
		scratchDesc.debugName = m_Specification.DebugName + ".ReflectionProbeCapture";
		nvrhi::TextureHandle array = m_Device->createTexture(arrayDesc);
		nvrhi::TextureHandle scratch = array ? m_Device->createTexture(scratchDesc) : nullptr;
		if (!array || !scratch)
		{
			if (hasSlots(m_ReflectionProbes))
				m_Errors.Report(fmt::format("failed to create the cube array of {} reflection probes at {} texels; the probes keep their cubes of {}", slots, size,
					m_ReflectionProbes->Array->getDesc().width));
			else
				m_Errors.Report(fmt::format("failed to create the cube array of {} reflection probes; the scene is lit without its probes", slots));
			return false;
		}
		ReflectionProbeState state;
		state.Array = std::move(array);
		state.Scratch = std::move(scratch);
		state.Slots.resize(slots);
		if (hasSlots(m_ReflectionProbes))
		{
			// A new resolution: the shown cubes keep shading while these are filled, each capture in the slot it has there.
			for (uint32_t index = 0; index < slots; index++)
				state.Slots[index] = ReflectionProbeSlot { m_ReflectionProbes->Slots[index].Capture, {}, m_ReflectionProbes->Slots[index].LastUsed };
			m_ReflectionProbesNext = std::move(state);
			return true;
		}
		ReleaseReflectionProbes();
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
			ReleaseStaleReflectionProbeHolds(); // Nothing in this view waits for its capture
			return true;
		}
		// Without cubes of the quality's size (reported: the frame is degraded) the shown ones keep shading when they have the slots.
		const bool resources = EnsureReflectionProbeResources();
		if (!m_ReflectionProbes || m_ReflectionProbes->Slots.size() != maxProbes)
		{
			m_Stats.ReflectionProbesPending = static_cast<uint32_t>(std::min<size_t>(candidates.size(), maxProbes));
			return false;
		}
		std::sort(candidates.begin(), candidates.end(), IsStronger);
		if (candidates.size() > maxProbes)
			candidates.resize(maxProbes);

		const uint64_t gather = ++m_ReflectionProbeGathers;
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		TextureStreamer* streamer = manager ? TextureStreamer::Find(*manager) : nullptr;
		uint32_t conversionsLeft = m_Specification.SpreadLightingUpdates ? c_ReflectionProbeConversionsPerFrame : std::numeric_limits<uint32_t>::max();
		// The slot holding a capture in a set of cubes, or c_NoSlot; and whether that slot was converted from the texture.
		const auto findSlot = [](const ReflectionProbeState& state, AssetHandle capture)
		{
			for (uint32_t index = 0; index < static_cast<uint32_t>(state.Slots.size()); index++)
			{
				if (state.Slots[index].Capture == capture)
					return index;
			}
			return c_NoSlot;
		};
		const auto holdsConverted = [&findSlot](const ReflectionProbeState& state, AssetHandle capture, const Ref<Texture>& texture)
		{
			const uint32_t slot = findSlot(state, capture);
			return texture && slot != c_NoSlot && state.Slots[slot].Source.lock() == texture;
		};
		std::vector<Ref<Texture>> textures;
		textures.reserve(candidates.size());
		for (const ProbeCandidate& candidate : candidates)
			textures.push_back(m_Assets.Resolve<Texture>(candidate.Capture, AssetPriority::High));

		// A capture's slot in a set of cubes: the one holding it, else a free one, else the least recently used one this gather
		// does not use.
		const auto acquireSlot = [gather](ReflectionProbeState& cubes, AssetHandle capture)
		{
			uint32_t replaceable = c_NoSlot;
			for (uint32_t index = 0; index < static_cast<uint32_t>(cubes.Slots.size()); index++)
			{
				const ReflectionProbeSlot& candidateSlot = cubes.Slots[index];
				if (candidateSlot.Capture == capture)
					return index;
				if (candidateSlot.LastUsed == gather)
					continue;
				if (replaceable == c_NoSlot || !candidateSlot.Capture.IsValid()
					|| (cubes.Slots[replaceable].Capture.IsValid() && candidateSlot.LastUsed < cubes.Slots[replaceable].LastUsed))
					replaceable = index;
			}
			// At most as many probes as slots are kept, so a slot this gather does not use is left.
			ST_CORE_ASSERT(replaceable != c_NoSlot, "No reflection probe slot left");
			cubes.Slots[replaceable] = ReflectionProbeSlot { capture, {}, 0 };
			return replaceable;
		};
		// The holds of captures still waiting, renewed for this frame.
		auto hold = [&](AssetHandle capture)
		{
			if (!streamer)
				return;
			ReflectionProbeHold& held = m_ReflectionProbeHolds[capture];
			if (!held.Hold)
				held.Hold = CreateRef<TextureHold>(TextureHold { capture });
			held.Frame = Renderer::GetFrameNumber();
			// Again in every frame it waits: a hold placed in another manager's streamer (a project opened since) lapsed with it.
			streamer->Hold(held.Hold);
		};

		// Holds a capture whole until it is converted, counted as pending for the frame's resolve cache while levels are missing.
		const auto holdWhole = [&](AssetHandle capture, const Ref<Texture>& texture)
		{
			hold(capture);
			if (texture && texture->GetResidentLevel() > 0)
				m_Assets.CountPending(capture);
		};
		// Whether a set of cubes' slot holds the capture converted, or the frame converts it: the conversion reads the whole
		// capture, so one not loaded with every level, or beyond the frame's conversions, waits.
		const auto convert = [&](const ReflectionProbeState& cubes, uint32_t slot, const Ref<Texture>& texture)
		{
			if (texture && cubes.Slots[slot].Source.lock() == texture)
				return true;
			if (std::any_of(m_ReflectionProbeConversions.begin(), m_ReflectionProbeConversions.end(),
				[slot](const ReflectionProbeConversion& conversion) { return conversion.Slot == slot; }))
				return true;
			if (!texture || !texture->GetGPUTexture() || texture->GetResidentLevel() > 0 || conversionsLeft == 0)
				return false;
			conversionsLeft--;
			m_ReflectionProbeConversions.push_back(ReflectionProbeConversion { slot, texture });
			return true;
		};
		const glm::dvec3 cameraPosition = glm::dvec3(camera.Position);
		const auto list = [&](const ProbeCandidate& candidate, uint32_t slot)
		{
			RenderData::ReflectionProbeData data;
			data.CenterBlend = glm::vec4(glm::vec3(candidate.Center - cameraPosition), candidate.BlendDistance);
			data.AxisX = glm::vec4(candidate.Axes[0], candidate.Extents.x);
			data.AxisY = glm::vec4(candidate.Axes[1], candidate.Extents.y);
			data.AxisZ = glm::vec4(candidate.Axes[2], candidate.Extents.z);
			data.Parameters = glm::vec4(candidate.Intensity, candidate.BoxProjection ? 1.0f : 0.0f, static_cast<float>(slot), 0.0f);
			m_ReflectionProbeList.push_back(data);
		};
		// A probe in view outside a change of resolution: shaded from its slot once converted (or in this frame), else the sky
		// lights its box until it is.
		const auto place = [&](size_t index)
		{
			ReflectionProbeState& cubes = *m_ReflectionProbes;
			const uint32_t slot = acquireSlot(cubes, candidates[index].Capture);
			cubes.Slots[slot].LastUsed = gather;
			if (convert(cubes, slot, textures[index]))
			{
				list(candidates[index], slot);
			}
			else
			{
				holdWhole(candidates[index].Capture, textures[index]);
				m_Stats.ReflectionProbesPending++;
			}
		};

		// After a new resolution, while its cubes are filled (m_ReflectionProbesNext), the shown cubes shade with what they hold,
		// and only those captures convert, into the same slots of the new cubes (the two share each slot's irradiance
		// coefficients in the probe buffer). The other probes in view wait. The new cubes take over in the frame that converts the
		// last of those captures, and the probes that waited are then placed as usual.
		const bool filling = m_ReflectionProbesNext.has_value();
		bool filled = true;
		std::vector<size_t> waiting;
		for (size_t index = 0; index < candidates.size(); index++)
		{
			const ProbeCandidate& candidate = candidates[index];
			const Ref<Texture>& texture = textures[index];
			// A capture that is not on its way (an unknown or failed handle, another type of asset) lights nothing: it takes no
			// slot and counts as pending for nobody, so captures and bakes do not wait for it.
			if (!texture && (!manager || manager->GetAssetState(candidate.Capture) != AssetState::Loading))
				continue;
			if (!filling)
			{
				place(index);
				continue;
			}
			ReflectionProbeState& shown = *m_ReflectionProbes;
			if (!holdsConverted(shown, candidate.Capture, texture))
			{
				waiting.push_back(index);
				continue;
			}
			const uint32_t slot = findSlot(shown, candidate.Capture);
			ReflectionProbeSlot& newSlot = m_ReflectionProbesNext->Slots[slot];
			ST_CORE_ASSERT(newSlot.Capture == candidate.Capture, "The cubes being filled lost the shown cubes' slots");
			shown.Slots[slot].LastUsed = gather;
			newSlot.LastUsed = gather;
			if (!convert(*m_ReflectionProbesNext, slot, texture))
			{
				holdWhole(candidate.Capture, texture);
				filled = false;
			}
			list(candidate, slot);
		}
		if (filling && filled)
		{
			// The new cubes hold what the shown ones shaded with in this view (the list's slots are the same in both): they take
			// over in this frame, its conversions going into them (ConvertReflectionProbes).
			std::optional<ReflectionProbeState> old = std::move(m_ReflectionProbes);
			m_ReflectionProbes = std::move(m_ReflectionProbesNext);
			m_ReflectionProbesNext.reset();
			ReleaseReflectionProbeState(old);
			for (size_t index : waiting)
				place(index);
		}
		else if (filling)
		{
			// Every probe in view waits for the new cubes: those shaded with the old ones too, so captures wait for the change.
			m_Stats.ReflectionProbesPending += static_cast<uint32_t>(m_ReflectionProbeList.size());
			for (size_t index : waiting)
			{
				holdWhole(candidates[index].Capture, textures[index]);
				m_Stats.ReflectionProbesPending++;
			}
		}
		// Captures converted (or no longer wanted) are held no more once no view of this frame or the previous one waited for
		// them: their levels may drop to their tail.
		ReleaseStaleReflectionProbeHolds();
		m_Stats.ReflectionProbes = static_cast<uint32_t>(m_ReflectionProbeList.size());
		m_Stats.ReflectionProbeConversions = static_cast<uint32_t>(m_ReflectionProbeConversions.size());
		return resources;
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
		// The gather's conversions go into the cubes being filled after a new resolution, else into the shown ones.
		ReflectionProbeState& state = m_ReflectionProbesNext ? *m_ReflectionProbesNext : *m_ReflectionProbes;
		const uint32_t size = state.Array->getDesc().width; // The row's size (EnsureReflectionProbeResources)
		const uint32_t scratchMips = RenderData::GetReflectionProbeMipChain(size);
		const uint32_t irradianceLevel = GetProbeIrradianceLevel(size);
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
					nvrhi::TextureDimension::Texture2DArray, RenderData::IBLParameters { mipSize, size, roughness, RendererQuality::GetProbePrefilterSamples(size, mip) }, 6,
					conversion.Slot * 6);
			}
			// Its irradiance coefficients, into the slot's part of the probe buffer.
			nvrhi::BindingSetDesc setDesc;
			setDesc.bindings = {
				nvrhi::BindingSetItem::Texture_SRV(0, state.Scratch, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(irradianceLevel, 1, 0, 6),
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
			parameters.SourceSize = std::max(1u, size >> irradianceLevel);
			parameters.Slot = conversion.Slot;
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->dispatch(1, 1, 1);
			// The slot holds the capture from now on (a frame that fails before this converts it again).
			state.Slots[conversion.Slot].Source = conversion.Source;
		}
		m_ReflectionProbeConversions.clear(); // The command list keeps the captures' GPU textures until it ran
	}

}
