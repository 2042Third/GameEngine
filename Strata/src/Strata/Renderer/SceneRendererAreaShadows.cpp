#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/RenderLights.h"
#include "Strata/Renderer/Renderer.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Rect lights' shadows beyond what the local shadows' views share with point lights (Docs/DeveloperGuide.md, "Area lights"):
// the atlas's depth ranges over the frame's drawn rect light tiles (Scene/LocalShadowRanges.comp, in GPU.Shadows after either
// path drew the views), which the filter (Include/AreaShadows.slang) classifies receivers with, and the opaque surfaces'
// screen-space masks (Effects/AreaScreenShadows.frag, then Effects/ScreenShadowsBlur.frag's two passes, in GPU.ScreenShadows
// after the sun's): per shadowed rect light in view, at most RenderData::c_MaxAreaShadowMasks, the filter at every prepass
// pixel within the light's reach on screen, blurred where its lattice's taps filtered into the light's layer of an R8 array.
// Transparent surfaces filter per pixel. Nothing exists or runs without a shadowed rect light.

namespace Strata
{

	namespace
	{

		// The masks' blur reaches this many pixels (Effects/AreaShadowsBlur.frag, twice the sun's: their penumbrae are wider): the
		// evaluation covers that much more than the light's reach on screen each way, the horizontal pass that much more vertically,
		// so the vertical pass's taps read what the light's pixels need.
		constexpr int32_t c_AreaShadowMaskMargin = 16;
		// The tiles of the rect lights' views a frame builds ranges for at most: every view of every rect light holding a shadow.
		constexpr uint32_t c_MaxLocalShadowRangeTiles = RenderData::c_MaxAreaShadowMasks * RenderData::c_LocalShadowFaces;

		// The pixels (x, y, width, height) a sphere covers on screen, conservatively: the bounding box of its box's corners
		// projected, the whole viewport where a corner lies behind the camera's near side.
		glm::ivec4 GetSphereScissor(const SceneCamera& camera, const glm::mat4& relativeViewProjection, const glm::vec3& relativeCenter, float radius,
			const glm::uvec2& viewport)
		{
			const glm::ivec4 whole(0, 0, static_cast<int32_t>(viewport.x), static_cast<int32_t>(viewport.y));
			glm::vec2 lowest(std::numeric_limits<float>::max());
			glm::vec2 highest(std::numeric_limits<float>::lowest());
			for (uint32_t corner = 0; corner < 8; corner++)
			{
				const glm::vec3 offset((corner & 1u) != 0 ? radius : -radius, (corner & 2u) != 0 ? radius : -radius, (corner & 4u) != 0 ? radius : -radius);
				const glm::vec4 clip = relativeViewProjection * glm::vec4(relativeCenter + offset, 1.0f);
				if (!camera.Orthographic && !(clip.w > 1e-4f))
					return whole;
				const glm::vec2 ndc = glm::vec2(clip) / clip.w;
				const glm::vec2 pixel((ndc.x * 0.5f + 0.5f) * static_cast<float>(viewport.x), (0.5f - ndc.y * 0.5f) * static_cast<float>(viewport.y));
				lowest = glm::min(lowest, pixel);
				highest = glm::max(highest, pixel);
			}
			if (!std::isfinite(lowest.x) || !std::isfinite(lowest.y) || !std::isfinite(highest.x) || !std::isfinite(highest.y))
				return whole;
			const glm::ivec2 first = glm::clamp(glm::ivec2(glm::floor(lowest)) - 1, glm::ivec2(0), glm::ivec2(viewport));
			const glm::ivec2 last = glm::clamp(glm::ivec2(glm::ceil(highest)) + 1, glm::ivec2(0), glm::ivec2(viewport));
			return glm::ivec4(first, last - first);
		}

		// A scissor grown by `x` and `y` pixels each way, within the viewport.
		nvrhi::Rect GetGrownScissor(const glm::ivec4& scissor, int32_t x, int32_t y, const glm::uvec2& viewport)
		{
			const int32_t left = std::max(scissor.x - x, 0);
			const int32_t top = std::max(scissor.y - y, 0);
			const int32_t right = std::min(scissor.x + scissor.z + x, static_cast<int32_t>(viewport.x));
			const int32_t bottom = std::min(scissor.y + scissor.w + y, static_cast<int32_t>(viewport.y));
			return nvrhi::Rect(left, std::max(right, left), top, std::max(bottom, top));
		}

	}

	void SceneRenderer::AssignAreaShadowMasks(const RenderLights& lights, const SceneCamera& camera)
	{
		m_AreaShadowMasks.clear();
		m_Stats.AreaShadowMasks = 0;
		const std::span<const RenderData::LightRecord> records = lights.GetRecords();
		const glm::dvec3 origin(camera.Position);
		const glm::mat4 relativeViewProjection = camera.Projection * GetRelativeView(camera);
		for (uint32_t slot = 0; slot < m_LocalShadowLights.size(); slot++)
		{
			LocalShadowLight& light = m_LocalShadowLights[slot];
			light.MaskLayer = 0;
			if (light.Type != RenderData::c_LightRect || !(light.Strength > 0.0f) || light.Record >= records.size())
				continue;
			// The ranking lets at most c_MaxAreaShadowMasks rect lights hold a shadow; one past them (never met) casts none rather
			// than another filter.
			if (m_AreaShadowMasks.size() >= RenderData::c_MaxAreaShadowMasks)
			{
				light.Strength = 0.0f;
				continue;
			}
			const RenderData::LightRecord& record = records[light.Record];
			AreaShadowMask mask;
			const glm::vec3 center(glm::dvec3(glm::vec3(record.PositionRange)) - origin);
			mask.Parameters.Center = glm::vec4(center, record.Source.y == RenderData::c_LightRecordTwoSided ? 1.0f : 0.0f);
			mask.Parameters.AxisX = glm::vec4(glm::vec3(record.AxisX) * record.AxisX.w, 0.0f);
			mask.Parameters.AxisY = glm::vec4(glm::vec3(record.AxisY) * record.AxisY.w, 0.0f);
			mask.Parameters.Normal = glm::vec4(glm::vec3(record.DirectionType), static_cast<float>(slot));
			const glm::vec3 boundsCenter(glm::dvec3(glm::vec3(record.Bounds)) - origin);
			mask.Scissor = GetSphereScissor(camera, relativeViewProjection, boundsCenter, record.Bounds.w, m_ViewportSize);
			m_AreaShadowMasks.push_back(mask);
			light.MaskLayer = static_cast<uint32_t>(m_AreaShadowMasks.size());
		}
		m_Stats.AreaShadowMasks = static_cast<uint32_t>(m_AreaShadowMasks.size());
	}

	bool SceneRenderer::EnsureAreaShadowMasks()
	{
		if (!m_AreaShadowMaskArray)
		{
			nvrhi::TextureDesc desc;
			desc.width = m_ViewportSize.x;
			desc.height = m_ViewportSize.y;
			desc.arraySize = RenderData::c_MaxAreaShadowMasks;
			desc.dimension = nvrhi::TextureDimension::Texture2DArray;
			desc.format = nvrhi::Format::R8_UNORM;
			desc.isRenderTarget = true;
			desc.debugName = m_Specification.DebugName + ".AreaShadowMasks";
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			nvrhi::TextureHandle masks = m_Device->createTexture(desc);
			if (!masks)
				return false;
			std::array<nvrhi::FramebufferHandle, RenderData::c_MaxAreaShadowMasks> layers;
			for (uint32_t layer = 0; layer < layers.size(); layer++)
			{
				layers[layer] = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(nvrhi::FramebufferAttachment().setTexture(masks).setArraySlice(layer)));
				if (!layers[layer])
					return false;
			}
			std::array<nvrhi::TextureHandle, 2> scratch;
			std::array<nvrhi::FramebufferHandle, 2> scratchFramebuffers;
			for (size_t index = 0; index < scratch.size(); index++)
			{
				scratch[index] = CreateRenderTarget(m_Device, m_ViewportSize.x, m_ViewportSize.y, nvrhi::Format::RG8_UNORM,
					m_Specification.DebugName + (index == 0 ? ".AreaShadowEvaluation" : ".AreaShadowBlur"), false);
				scratchFramebuffers[index] = scratch[index] ? m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(scratch[index])) : nullptr;
				if (!scratch[index] || !scratchFramebuffers[index])
					return false;
			}
			m_AreaShadowMaskArray = std::move(masks);
			m_AreaShadowMaskFramebuffers = std::move(layers);
			m_AreaShadowScratch = std::move(scratch);
			m_AreaShadowScratchFramebuffers = std::move(scratchFramebuffers);
			m_AreaShadowMaskSet = nullptr; // Name the new targets
			m_AreaShadowBlurSets = {};
		}
		if (!m_AreaShadowMaskPipeline)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(8),           // The local shadows' atlas
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(13), // The shadowed lights' views
				nvrhi::BindingLayoutItem::Texture_SRV(22),          // The prepass's depth
				nvrhi::BindingLayoutItem::Texture_SRV(23),          // Its normals
				nvrhi::BindingLayoutItem::Texture_SRV(33),          // The blue-noise tile
				nvrhi::BindingLayoutItem::Texture_SRV(57),          // The atlas's depth ranges
				nvrhi::BindingLayoutItem::Sampler(8),               // The filter's gathers of the atlas
				nvrhi::BindingLayoutItem::Sampler(9),
				// Slot 1: register b0 belongs to the frame constants.
				nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::AreaShadowMaskParameters))
			};
			m_AreaShadowMaskLayout = m_Device->createBindingLayout(layout);
			nvrhi::BindingLayoutDesc blurLayout;
			blurLayout.visibility = nvrhi::ShaderType::Pixel;
			blurLayout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(22),
				nvrhi::BindingLayoutItem::Texture_SRV(23),
				nvrhi::BindingLayoutItem::Texture_SRV(24), // The pass's source
				nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::ScreenShadowBlurParameters))
			};
			m_AreaShadowBlurLayout = m_Device->createBindingLayout(blurLayout);
			ShaderLibrary& shaders = Renderer::GetShaderLibrary();
			nvrhi::IShader* evaluate = shaders.Get("Effects/AreaScreenShadows.frag");
			nvrhi::IShader* blur = shaders.Get("Effects/AreaShadowsBlur.frag");
			ST_CORE_VERIFY(evaluate && blur && m_AreaShadowMaskLayout && m_AreaShadowBlurLayout, "SceneRenderer: the area shadow mask shaders are missing");
			nvrhi::GraphicsPipelineDesc desc;
			desc.primType = nvrhi::PrimitiveType::TriangleList;
			desc.VS = m_FullscreenVertexShader;
			desc.PS = evaluate;
			desc.bindingLayouts = { m_AreaShadowMaskLayout };
			desc.renderState.rasterState.setCullNone();
			desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
			m_AreaShadowMaskPipeline = m_Device->createGraphicsPipeline(desc, m_AreaShadowScratchFramebuffers[0]->getFramebufferInfo());
			desc.PS = blur;
			desc.bindingLayouts = { m_AreaShadowBlurLayout };
			m_AreaShadowBlurPipeline = m_Device->createGraphicsPipeline(desc, m_AreaShadowScratchFramebuffers[0]->getFramebufferInfo());
			// The vertical pass writes its red (the visibility) into a layer of the masks.
			m_AreaShadowBlurLayerPipeline = m_Device->createGraphicsPipeline(desc, m_AreaShadowMaskFramebuffers[0]->getFramebufferInfo());
			ST_CORE_VERIFY(m_AreaShadowMaskPipeline && m_AreaShadowBlurPipeline && m_AreaShadowBlurLayerPipeline,
				"SceneRenderer: failed to create the area shadow mask pipelines");
		}
		for (size_t pass = 0; pass < m_AreaShadowBlurSets.size(); pass++)
		{
			if (m_AreaShadowBlurSets[pass])
				continue;
			// The horizontal pass reads the evaluation, the vertical one the horizontal pass's result.
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(22, m_DepthTexture),
				nvrhi::BindingSetItem::Texture_SRV(23, m_NormalTexture),
				nvrhi::BindingSetItem::Texture_SRV(24, m_AreaShadowScratch[pass]),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::ScreenShadowBlurParameters))
			};
			m_AreaShadowBlurSets[pass] = m_Device->createBindingSet(desc, m_AreaShadowBlurLayout);
			if (!m_AreaShadowBlurSets[pass])
				return false;
		}
		return true;
	}

	void SceneRenderer::ReleaseAreaShadowMasks(std::vector<nvrhi::ResourceHandle>& released)
	{
		const auto release = [&released](auto& handle)
		{
			if (handle)
				released.emplace_back(handle.Get());
			handle = nullptr;
		};
		release(m_AreaShadowMaskArray);
		for (nvrhi::FramebufferHandle& framebuffer : m_AreaShadowMaskFramebuffers)
			release(framebuffer);
		for (size_t index = 0; index < m_AreaShadowScratch.size(); index++)
		{
			release(m_AreaShadowScratch[index]);
			release(m_AreaShadowScratchFramebuffers[index]);
			release(m_AreaShadowBlurSets[index]);
		}
		release(m_AreaShadowMaskSet);
		m_AreaShadowBoundAtlas = nullptr;
		m_AreaShadowBoundRanges = nullptr;
		m_AreaShadowBoundBuffer = nullptr;
		// The scene's binding sets name the masks: made again by the next frame's comparison.
		m_BoundAreaShadowMasks = nullptr;
	}

	void SceneRenderer::RenderAreaShadowMasks(nvrhi::ICommandList* commandList)
	{
		// The evaluation names the atlas, its ranges and the local shadows' buffer, which the frame's buffers may have replaced.
		if (!m_AreaShadowMaskSet || m_AreaShadowBoundAtlas != m_LocalShadowAtlas.Get() || m_AreaShadowBoundRanges != m_LocalShadowRanges.Get()
			|| m_AreaShadowBoundBuffer != m_LocalShadowBuffer.Get())
		{
			if (m_AreaShadowMaskSet)
				Renderer::ReleaseDeferred({ nvrhi::ResourceHandle(m_AreaShadowMaskSet.Get()) });
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(8, m_LocalShadowAtlas),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_LocalShadowBuffer),
				nvrhi::BindingSetItem::Texture_SRV(22, m_DepthTexture),
				nvrhi::BindingSetItem::Texture_SRV(23, m_NormalTexture),
				nvrhi::BindingSetItem::Texture_SRV(33, Renderer::GetBlueNoiseTexture()),
				nvrhi::BindingSetItem::Texture_SRV(57, m_LocalShadowRanges),
				nvrhi::BindingSetItem::Sampler(8, m_LinearClampSampler),
				nvrhi::BindingSetItem::Sampler(9, m_ShadowCompareSampler),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::AreaShadowMaskParameters))
			};
			m_AreaShadowMaskSet = m_Device->createBindingSet(desc, m_AreaShadowMaskLayout);
			m_AreaShadowBoundAtlas = m_LocalShadowAtlas.Get();
			m_AreaShadowBoundRanges = m_LocalShadowRanges.Get();
			m_AreaShadowBoundBuffer = m_LocalShadowBuffer.Get();
			if (!m_AreaShadowMaskSet)
			{
				m_Errors.Report("failed to create the rect lights' shadow mask binding set; their shadows are not drawn this frame");
				return;
			}
		}
		const nvrhi::Viewport viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y));
		for (uint32_t layer = 0; layer < m_AreaShadowMasks.size(); layer++)
		{
			const AreaShadowMask& mask = m_AreaShadowMasks[layer];
			// The evaluation into the first target over the light's pixels and the blur's reach, the horizontal blur into the
			// second over the light's pixels and the vertical blur's reach, the vertical one into the light's layer.
			nvrhi::GraphicsState state;
			state.pipeline = m_AreaShadowMaskPipeline;
			state.framebuffer = m_AreaShadowScratchFramebuffers[0];
			state.viewport.addViewport(viewport);
			state.viewport.addScissorRect(GetGrownScissor(mask.Scissor, c_AreaShadowMaskMargin, c_AreaShadowMaskMargin, m_ViewportSize));
			state.bindings = { m_AreaShadowMaskSet };
			commandList->setGraphicsState(state);
			commandList->setPushConstants(&mask.Parameters, sizeof(mask.Parameters));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
			for (size_t pass = 0; pass < m_AreaShadowBlurSets.size(); pass++)
			{
				RenderData::ScreenShadowBlurParameters blur = {};
				blur.DirectionX = pass == 0 ? 1 : 0;
				blur.DirectionY = pass == 0 ? 0 : 1;
				blur.PlaneTolerance = c_ScreenShadowPlaneTolerance;
				nvrhi::GraphicsState blurState;
				blurState.pipeline = pass == 0 ? m_AreaShadowBlurPipeline : m_AreaShadowBlurLayerPipeline;
				blurState.framebuffer = pass == 0 ? m_AreaShadowScratchFramebuffers[1] : m_AreaShadowMaskFramebuffers[layer];
				blurState.viewport.addViewport(viewport);
				blurState.viewport.addScissorRect(GetGrownScissor(mask.Scissor, 0, pass == 0 ? c_AreaShadowMaskMargin : 0, m_ViewportSize));
				blurState.bindings = { m_AreaShadowBlurSets[pass] };
				commandList->setGraphicsState(blurState);
				commandList->setPushConstants(&blur, sizeof(blur));
				commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
			}
		}
	}

	bool SceneRenderer::EnsureLocalShadowRanges()
	{
		// Levels of cells up to an eighth of the largest tile (at least the first three, for 128-texel tiles).
		const uint32_t atlasSize = m_LocalShadowAllocator.GetAtlasSize();
		const uint32_t maxTile = std::max(std::min(m_Specification.Quality.LocalShadowMaxTile, atlasSize), 128u);
		const uint32_t levels = static_cast<uint32_t>(std::bit_width(maxTile / (RenderData::c_ShadowRangeCellTexels * RenderData::c_LocalShadowRangeMinTileShare)));
		const uint32_t width = std::max(atlasSize / RenderData::c_ShadowRangeCellTexels, 1u);
		if (m_LocalShadowRanges && m_LocalShadowRangeLevels == levels && m_LocalShadowRanges->getDesc().width == width && !m_LocalShadowRangeSets.empty())
			return true;
		if (!m_LocalShadowAtlas)
			return false;
		std::vector<nvrhi::ResourceHandle> released;
		ReleaseLocalShadowRanges(released);
		Renderer::ReleaseDeferred(std::move(released));
		if (!m_LocalShadowRangePipeline)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.bindings = {
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
				nvrhi::BindingLayoutItem::Texture_UAV(0),
				nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::LocalShadowRangeParameters))
			};
			m_LocalShadowRangeLayout = m_Device->createBindingLayout(layout);
			ST_CORE_VERIFY(m_LocalShadowRangeLayout, "SceneRenderer: failed to create the local shadow ranges' layout");
			m_LocalShadowRangePipeline = CreateComputePipeline("Scene/LocalShadowRanges.comp", m_LocalShadowRangeLayout);
			m_LocalShadowRangeDownsamplePipeline = CreateComputePipeline("Scene/LocalShadowRangesDownsample.comp", m_LocalShadowRangeLayout);
			ST_CORE_VERIFY(m_LocalShadowRangePipeline && m_LocalShadowRangeDownsamplePipeline, "SceneRenderer: failed to create the local shadow ranges' pipelines");
		}
		if (!m_LocalShadowRangeTileBuffer)
		{
			m_LocalShadowRangeTileBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
				.setByteSize(c_MaxLocalShadowRangeTiles * sizeof(glm::uvec4))
				.setStructStride(sizeof(glm::uvec4))
				.setInitialState(nvrhi::ResourceStates::ShaderResource)
				.setKeepInitialState(true)
				.setDebugName(m_Specification.DebugName + ".LocalShadowRangeTiles"));
			if (!m_LocalShadowRangeTileBuffer)
				return false;
		}
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = width;
		desc.mipLevels = levels;
		desc.format = nvrhi::Format::R32_UINT;
		desc.isUAV = true;
		desc.debugName = m_Specification.DebugName + ".LocalShadowRanges";
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		nvrhi::TextureHandle ranges = m_Device->createTexture(desc);
		if (!ranges)
			return false;
		std::vector<nvrhi::BindingSetHandle> sets;
		for (uint32_t level = 0; level < levels; level++)
		{
			nvrhi::BindingSetDesc set;
			set.bindings = {
				level == 0 ? nvrhi::BindingSetItem::Texture_SRV(0, m_LocalShadowAtlas)
					: nvrhi::BindingSetItem::Texture_SRV(0, ranges, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(level - 1, 1, 0, 1)),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_LocalShadowRangeTileBuffer),
				nvrhi::BindingSetItem::Texture_UAV(0, ranges, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(level, 1, 0, 1)),
				nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::LocalShadowRangeParameters))
			};
			sets.push_back(m_Device->createBindingSet(set, m_LocalShadowRangeLayout));
			if (!sets.back())
				return false;
		}
		m_LocalShadowRanges = std::move(ranges);
		m_LocalShadowRangeSets = std::move(sets);
		m_LocalShadowRangeLevels = levels;
		return true;
	}

	void SceneRenderer::BuildLocalShadowRanges(nvrhi::ICommandList* commandList)
	{
		if (!m_LocalShadowRanges)
			return;
		// The rect lights' tiles drawn this frame, and those cleared for views no receiver needs (which a turn of the light may
		// make needed again before they are drawn: their ranges then say unshadowed).
		m_LocalShadowRangeTiles.clear();
		uint32_t largest = 0;
		const auto add = [&](const ShadowAtlasTile& tile)
		{
			if (!tile.IsValid() || m_LocalShadowRangeTiles.size() >= c_MaxLocalShadowRangeTiles)
				return;
			m_LocalShadowRangeTiles.emplace_back(tile.X, tile.Y, tile.Size, 0u);
			largest = std::max(largest, tile.Size);
		};
		for (const LocalShadowDrawView& drawView : m_LocalShadowViews)
		{
			const LocalShadowLight& light = m_LocalShadowLights[drawView.Light];
			if (light.Type == RenderData::c_LightRect)
				add(light.Views[drawView.Face].Key.Tile);
		}
		for (const ShadowAtlasTile& tile : m_LocalShadowClears)
			add(tile);
		if (m_LocalShadowRangeTiles.empty())
			return;
		commandList->beginMarker("LocalShadowRanges");
		commandList->writeBuffer(m_LocalShadowRangeTileBuffer, m_LocalShadowRangeTiles.data(), m_LocalShadowRangeTiles.size() * sizeof(glm::uvec4));
		// Level by level, every tile in one dispatch; NVRHI places the barriers between the levels (each reads the one below
		// through its own binding set).
		glm::uvec2 source(m_LocalShadowAllocator.GetAtlasSize());
		for (uint32_t level = 0; level < m_LocalShadowRangeLevels; level++)
		{
			const uint32_t cellTexels = RenderData::c_ShadowRangeCellTexels << level;
			nvrhi::ComputeState state;
			state.pipeline = level == 0 ? m_LocalShadowRangePipeline : m_LocalShadowRangeDownsamplePipeline;
			state.bindings = { m_LocalShadowRangeSets[level] };
			commandList->setComputeState(state);
			RenderData::LocalShadowRangeParameters parameters = {};
			parameters.SourceSize = source;
			parameters.Level = level;
			parameters.TileCount = static_cast<uint32_t>(m_LocalShadowRangeTiles.size());
			parameters.CellTexels = cellTexels;
			commandList->setPushConstants(&parameters, sizeof(parameters));
			const uint32_t cells = std::max(largest / cellTexels, 1u);
			commandList->dispatch((cells + 7) / 8, (cells + 7) / 8, parameters.TileCount);
			source = glm::max(glm::uvec2(m_LocalShadowAllocator.GetAtlasSize() / cellTexels), glm::uvec2(1));
		}
		commandList->endMarker();
	}

	void SceneRenderer::ReleaseLocalShadowRanges(std::vector<nvrhi::ResourceHandle>& released)
	{
		if (m_LocalShadowRanges)
			released.emplace_back(m_LocalShadowRanges.Get());
		for (nvrhi::BindingSetHandle& set : m_LocalShadowRangeSets)
		{
			if (set)
				released.emplace_back(set.Get());
		}
		m_LocalShadowRanges = nullptr;
		m_LocalShadowRangeSets.clear();
		m_LocalShadowRangeLevels = 0;
		// The scene's binding sets and the masks' evaluation name the ranges: made again by the next frame's comparison.
		m_BoundLocalShadowRanges = nullptr;
		m_AreaShadowBoundRanges = nullptr;
	}

}
