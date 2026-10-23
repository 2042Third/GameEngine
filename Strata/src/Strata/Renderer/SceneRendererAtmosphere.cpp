#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Core/Assert.h"
#include "Strata/Core/Log.h"
#include "Strata/Renderer/Atmosphere.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/SceneRenderData.h"
#include "Strata/Renderer/ShaderLibrary.h"

#include <algorithm>
#include <cmath>

// The atmosphere (SkyLightSource::Atmosphere): its tables, the sun's light through it, and its double-buffered environment.
// The passes run in GPU.Setup, where UpdateEnvironment records them.

namespace Strata
{

	namespace
	{

		constexpr nvrhi::Format c_AtmosphereTableFormat = nvrhi::Format::RGBA16_FLOAT;
		// The sky-view table is computed for the camera's height clamped to this (the atmosphere is seen from near the ground:
		// higher views are deferred).
		constexpr float c_MaxAtmosphereViewHeight = 20000.0f;

		uint32_t AtmosphereGroups(uint32_t size)
		{
			return (size + 7) / 8;
		}

		float FiniteAtLeastZero(float value, float fallback)
		{
			return std::isfinite(value) ? std::max(value, 0.0f) : fallback;
		}

		// The angle between two unit vectors in degrees, precise for small angles (from the chord, not the dot product).
		float AngleBetweenDegrees(const glm::vec3& a, const glm::vec3& b)
		{
			const float chord = glm::length(a - b);
			return glm::degrees(2.0f * std::asin(std::min(chord * 0.5f, 1.0f)));
		}

	}

	bool SceneRenderer::EnsureAtmosphereResources()
	{
		if (m_Atmosphere)
			return true;

		Scope<AtmosphereState> atmosphere = CreateScope<AtmosphereState>();
		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Compute;
		layoutDesc.bindings = {
			nvrhi::BindingLayoutItem::ConstantBuffer(0),
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Texture_SRV(2),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::Texture_UAV(0),
			nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::AtmospherePassParameters))
		};
		atmosphere->Layout = m_Device->createBindingLayout(layoutDesc);

		ShaderLibrary& shaders = Renderer::GetShaderLibrary();
		const auto createPipeline = [&](const char* name)
		{
			nvrhi::ShaderHandle shader = shaders.Get(name);
			if (!shader || !atmosphere->Layout)
				return nvrhi::ComputePipelineHandle();
			nvrhi::ComputePipelineDesc desc;
			desc.CS = shader;
			desc.bindingLayouts = { atmosphere->Layout };
			return m_Device->createComputePipeline(desc);
		};
		atmosphere->TransmittancePipeline = createPipeline("IBL/AtmosphereTransmittance.comp");
		atmosphere->MultipleScatteringPipeline = createPipeline("IBL/AtmosphereMultipleScattering.comp");
		atmosphere->SkyViewPipeline = createPipeline("IBL/AtmosphereSkyView.comp");
		atmosphere->CubePipeline = createPipeline("IBL/AtmosphereCube.comp");

		// The sky view's azimuth wraps around; its elevation and the other tables' coordinates stay within their texels.
		atmosphere->Sampler = m_Device->createSampler(nvrhi::SamplerDesc()
			.setAllFilters(true)
			.setAddressU(nvrhi::SamplerAddressMode::Wrap)
			.setAddressV(nvrhi::SamplerAddressMode::Clamp)
			.setAddressW(nvrhi::SamplerAddressMode::Clamp));
		atmosphere->Constants = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(sizeof(RenderData::AtmosphereConstants))
			.setIsConstantBuffer(true)
			.setInitialState(nvrhi::ResourceStates::ConstantBuffer)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".AtmosphereConstants"));

		const auto createTable = [&](uint32_t width, uint32_t height, const char* name)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = c_AtmosphereTableFormat;
			desc.isUAV = true;
			desc.debugName = m_Specification.DebugName + "." + name;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			return m_Device->createTexture(desc);
		};
		atmosphere->Transmittance = createTable(Atmosphere::c_TransmittanceWidth, Atmosphere::c_TransmittanceHeight, "AtmosphereTransmittance");
		atmosphere->MultipleScattering = createTable(Atmosphere::c_MultipleScatteringSize, Atmosphere::c_MultipleScatteringSize, "AtmosphereMultipleScattering");
		atmosphere->SkyView = createTable(Atmosphere::c_SkyViewWidth, Atmosphere::c_SkyViewHeight, "AtmosphereSkyView");

		if (!atmosphere->Layout || !atmosphere->TransmittancePipeline || !atmosphere->MultipleScatteringPipeline || !atmosphere->SkyViewPipeline
			|| !atmosphere->CubePipeline || !atmosphere->Sampler || !atmosphere->Constants || !atmosphere->Transmittance || !atmosphere->MultipleScattering
			|| !atmosphere->SkyView)
			return false;

		// Each pass reads the tables computed before it and a black texture where one is still to be written.
		nvrhi::ITexture* black = Renderer::GetBlackTexture();
		const auto createSet = [&](nvrhi::ITexture* transmittance, nvrhi::ITexture* multipleScattering, nvrhi::ITexture* output)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, atmosphere->Constants),
				nvrhi::BindingSetItem::Texture_SRV(0, transmittance),
				nvrhi::BindingSetItem::Texture_SRV(1, multipleScattering),
				nvrhi::BindingSetItem::Texture_SRV(2, black),
				nvrhi::BindingSetItem::Sampler(0, atmosphere->Sampler),
				nvrhi::BindingSetItem::Texture_UAV(0, output),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::AtmospherePassParameters))
			};
			return m_Device->createBindingSet(desc, atmosphere->Layout);
		};
		atmosphere->TransmittanceSet = createSet(black, black, atmosphere->Transmittance);
		atmosphere->MultipleScatteringSet = createSet(atmosphere->Transmittance, black, atmosphere->MultipleScattering);
		atmosphere->SkyViewSet = createSet(atmosphere->Transmittance, atmosphere->MultipleScattering, atmosphere->SkyView);
		if (!atmosphere->TransmittanceSet || !atmosphere->MultipleScatteringSet || !atmosphere->SkyViewSet)
			return false;
		m_Atmosphere = std::move(atmosphere);
		return true;
	}

	nvrhi::ITexture* SceneRenderer::GetBoundSkyView() const
	{
		return m_AtmosphereFrame && m_Atmosphere ? m_Atmosphere->SkyView.Get() : Renderer::GetBlackTexture();
	}

	void SceneRenderer::WriteAtmosphereConstants(nvrhi::ICommandList* commandList, const AtmosphereSettings& settings, const glm::vec3& towardSun, int32_t band)
	{
		const Atmosphere::Medium medium = Atmosphere::GetMedium(settings.Density, settings.Haze);
		RenderData::AtmosphereConstants constants = {};
		constants.RayleighScattering = glm::vec4(glm::vec3(medium.RayleighScattering), 0.0f);
		constants.MieScattering = glm::vec4(glm::vec3(medium.MieScattering), 0.0f);
		constants.MieExtinction = glm::vec4(glm::vec3(medium.MieExtinction), 0.0f);
		constants.OzoneAbsorption = glm::vec4(glm::vec3(medium.OzoneAbsorption), 0.0f);
		constants.GroundAlbedo = glm::vec4(glm::vec3(settings.GroundAlbedo), 0.0f);
		constants.SunDirection = glm::vec4(towardSun, 0.0f);
		constants.SunColor = glm::vec4(settings.SunColor, 0.0f);
		// The viewer at the middle of its height band.
		const double height = (static_cast<double>(band) + 0.5) * static_cast<double>(c_AtmosphereHeightBand) / 1000.0;
		constants.Viewer = glm::vec4(static_cast<float>(Atmosphere::c_BottomRadius + height), 0.0f, 0.0f, 0.0f);
		commandList->writeBuffer(m_Atmosphere->Constants, &constants, sizeof(constants));
	}

	void SceneRenderer::RecordAtmosphereEnvironmentStep(nvrhi::ICommandList* commandList, const EnvironmentMaps& maps, uint32_t step)
	{
		ST_CORE_ASSERT(step < c_AtmosphereRefreshSteps, "Atmosphere environment step {} out of range", step);
		if (step >= 6)
		{
			FilterEnvironmentStep(commandList, maps, step - 6);
			return;
		}
		const AtmosphereState& atmosphere = *m_Atmosphere;
		const nvrhi::TextureDesc& cubeDesc = maps.Cube->getDesc();
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, atmosphere.Constants),
			nvrhi::BindingSetItem::Texture_SRV(0, atmosphere.Transmittance),
			nvrhi::BindingSetItem::Texture_SRV(1, atmosphere.MultipleScattering),
			nvrhi::BindingSetItem::Texture_SRV(2, atmosphere.SkyView),
			nvrhi::BindingSetItem::Sampler(0, atmosphere.Sampler),
			nvrhi::BindingSetItem::Texture_UAV(0, maps.Cube, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 6), nvrhi::TextureDimension::Texture2DArray),
			nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::AtmospherePassParameters))
		};
		nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(desc, atmosphere.Layout);
		ST_CORE_VERIFY(bindingSet, "SceneRenderer: failed to create the atmosphere's environment binding set");
		nvrhi::ComputeState state;
		state.pipeline = atmosphere.CubePipeline;
		state.bindings = { bindingSet };
		commandList->setComputeState(state);
		const RenderData::AtmospherePassParameters parameters = { step, cubeDesc.width, 0, 0 };
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch(AtmosphereGroups(cubeDesc.width), AtmosphereGroups(cubeDesc.height), 1);
	}

	bool SceneRenderer::UpdateAtmosphere(const SkyLightComponent& sky, RenderData::FrameConstants& frame, nvrhi::ICommandList* commandList)
	{
		if (!EnsureAtmosphereResources())
		{
			m_Errors.Report("the atmosphere's tables could not be created; the sky has no environment");
			return false;
		}
		AtmosphereState& atmosphere = *m_Atmosphere;

		// The sun: the directional light the scene is lit by, whose color times intensity is its illuminance above the
		// atmosphere, or without one a sun of 1 lux where the procedural sky would put it.
		const bool hasSun = frame.DirectionalLightDirection.w > 0.0f;
		const glm::vec3 towardSun = hasSun ? -glm::vec3(frame.DirectionalLightDirection) : GetDefaultSunDirection();
		glm::vec3 light = hasSun ? glm::vec3(frame.DirectionalLightColor) : glm::vec3(1.0f);
		if (!std::isfinite(light.r + light.g + light.b))
			light = glm::vec3(0.0f);
		light = glm::max(light, glm::vec3(0.0f));
		const float largest = std::max({ light.r, light.g, light.b });
		m_AtmosphereSunIlluminance = largest;
		// The tables' sun color comes from the light's color alone, so that its intensity changes none of them: the color
		// times the intensity over its largest component differs from the color's own in the last bit for some intensities,
		// and an intensity of 0 has no largest component.
		glm::vec3 color = hasSun ? m_SunColor : glm::vec3(1.0f);
		if (!std::isfinite(color.r + color.g + color.b))
			color = glm::vec3(0.0f);
		color = glm::max(color, glm::vec3(0.0f));
		const float colorLargest = std::max({ color.r, color.g, color.b });

		AtmosphereSettings settings;
		settings.Density = FiniteAtLeastZero(sky.AtmosphereDensity, 1.0f);
		settings.Haze = FiniteAtLeastZero(sky.AtmosphereHaze, 1.0f);
		settings.GroundAlbedo = std::min(FiniteAtLeastZero(sky.GroundAlbedo, 0.3f), 1.0f);
		settings.SunColor = colorLargest > 0.0f ? color / colorLargest : glm::vec3(1.0f);

		const float height = std::isfinite(frame.CameraPosition.y) ? std::clamp(frame.CameraPosition.y, 0.0f, c_MaxAtmosphereViewHeight) : 0.0f;
		const int32_t band = static_cast<int32_t>(height / c_AtmosphereHeightBand);

		commandList->beginMarker("Atmosphere");
		const bool tables = atmosphere.TableSettings != settings;
		const bool skyView = tables || !atmosphere.SkyViewValid || atmosphere.SkyViewSun != towardSun || atmosphere.SkyViewBand != band;
		if (skyView)
			WriteAtmosphereConstants(commandList, settings, towardSun, band);
		const auto dispatch = [&](nvrhi::IComputePipeline* pipeline, nvrhi::IBindingSet* bindingSet, uint32_t width, uint32_t height)
		{
			nvrhi::ComputeState state;
			state.pipeline = pipeline;
			state.bindings = { bindingSet };
			commandList->setComputeState(state);
			const RenderData::AtmospherePassParameters parameters = {};
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->dispatch(AtmosphereGroups(width), AtmosphereGroups(height), 1);
		};
		if (tables)
		{
			dispatch(atmosphere.TransmittancePipeline, atmosphere.TransmittanceSet, Atmosphere::c_TransmittanceWidth, Atmosphere::c_TransmittanceHeight);
			dispatch(atmosphere.MultipleScatteringPipeline, atmosphere.MultipleScatteringSet, Atmosphere::c_MultipleScatteringSize,
				Atmosphere::c_MultipleScatteringSize);
			atmosphere.TableSettings = settings;
			m_Stats.AtmosphereTableUpdates++;
		}
		if (skyView)
		{
			dispatch(atmosphere.SkyViewPipeline, atmosphere.SkyViewSet, Atmosphere::c_SkyViewWidth, Atmosphere::c_SkyViewHeight);
			atmosphere.SkyViewSun = towardSun;
			atmosphere.SkyViewBand = band;
			atmosphere.SkyViewValid = true;
			m_Stats.AtmosphereSkyViewUpdates++;
		}

		// The environment. The first one of this atmosphere is computed at once (nothing would light the scene meanwhile);
		// later ones, once the sun moved far enough or the settings changed, into the spare maps, a step per frame while
		// lighting updates spread over frames, and take over when complete.
		const auto takeMaps = [&]()
		{
			EnvironmentMaps maps = atmosphere.Spare ? std::move(*atmosphere.Spare) : CreateEnvironmentMaps();
			atmosphere.Spare.reset();
			maps.Source = nullptr;
			maps.Procedural.reset();
			maps.Atmosphere = settings;
			maps.AtmosphereSun = towardSun;
			return maps;
		};
		if (!m_Environment || !m_Environment->Atmosphere)
		{
			atmosphere.Refresh.reset();
			EnvironmentMaps maps = takeMaps();
			for (uint32_t step = 0; step < c_AtmosphereRefreshSteps; step++)
				RecordAtmosphereEnvironmentStep(commandList, maps, step);
			m_Environment = CreateRef<const EnvironmentMaps>(std::move(maps));
			m_Stats.EnvironmentUpdates++;
		}
		else
		{
			if (atmosphere.Refresh && atmosphere.Refresh->Atmosphere != settings)
			{
				// What it computed so far is out of date: from its first step again.
				atmosphere.Refresh->Atmosphere = settings;
				atmosphere.Refresh->AtmosphereSun = towardSun;
				atmosphere.RefreshStep = 0;
			}
			if (!atmosphere.Refresh && (m_Environment->Atmosphere != settings || AngleBetweenDegrees(m_Environment->AtmosphereSun, towardSun) > c_AtmosphereRefreshAngle))
			{
				atmosphere.Refresh = takeMaps();
				atmosphere.RefreshStep = 0;
			}
			if (atmosphere.Refresh)
			{
				const uint32_t end = m_Specification.SpreadLightingUpdates ? atmosphere.RefreshStep + 1 : c_AtmosphereRefreshSteps;
				for (; atmosphere.RefreshStep < end; atmosphere.RefreshStep++)
				{
					RecordAtmosphereEnvironmentStep(commandList, *atmosphere.Refresh, atmosphere.RefreshStep);
					m_Stats.EnvironmentRefreshSteps++;
				}
				if (atmosphere.RefreshStep == c_AtmosphereRefreshSteps)
				{
					// The maps it replaces become the spare: commands that still read them were recorded earlier on the same queue,
					// and the state tracking orders the next refresh's writes after them.
					EnvironmentMaps previous = *m_Environment;
					m_Environment = CreateRef<const EnvironmentMaps>(std::move(*atmosphere.Refresh));
					atmosphere.Refresh.reset();
					previous.Atmosphere.reset();
					atmosphere.Spare = std::move(previous);
					m_Stats.EnvironmentUpdates++;
				}
			}
		}
		commandList->endMarker();
		m_Stats.EnvironmentRefreshPending = atmosphere.Refresh.has_value();

		// The sun's light at the ground: dimmed and reddened by the air it crossed (the same transmittance as the tables',
		// evaluated on the CPU once per change), so the lighting, its shadows and the sky agree without a readback.
		const float sunSize = std::isfinite(sky.SunSize) ? std::clamp(sky.SunSize, 0.0f, 180.0f) : 0.0f;
		const float radius = glm::radians(sunSize) * 0.5f;
		if (atmosphere.SunSettings != settings || atmosphere.SunDirection != towardSun || atmosphere.SunRadius != radius)
		{
			const Atmosphere::Medium medium = Atmosphere::GetMedium(settings.Density, settings.Haze);
			atmosphere.SunTransmittance = glm::vec3(Atmosphere::ComputeSunTransmittance(medium, glm::dvec3(towardSun), static_cast<double>(radius)));
			atmosphere.SunSettings = settings;
			atmosphere.SunDirection = towardSun;
			atmosphere.SunRadius = radius;
		}
		const glm::vec3 transmittance = atmosphere.SunTransmittance;
		if (hasSun)
		{
			frame.DirectionalLightColor = glm::vec4(glm::vec3(frame.DirectionalLightColor) * transmittance, frame.DirectionalLightColor.w);
			m_SunColor *= transmittance;
		}
		frame.SunDiskDirection = glm::vec4(towardSun, radius);
		frame.SunDiskColor = glm::vec4(light * transmittance, 0.0f);
		m_Stats.Atmosphere = true;
		m_Stats.SunTransmittance = transmittance;
		return true;
	}

}
