#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Math/Frustum.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace Strata
{

	namespace
	{

		constexpr nvrhi::Format c_DepthFormat = nvrhi::Format::D32;
		constexpr nvrhi::Format c_NormalFormat = nvrhi::Format::RG16_FLOAT;
		constexpr nvrhi::Format c_EntityIDFormat = nvrhi::Format::R32_UINT;
		constexpr nvrhi::Format c_HDRFormat = nvrhi::Format::RGBA16_FLOAT;
		constexpr nvrhi::Format c_OutputFormat = nvrhi::Format::RGBA8_UNORM;
		constexpr nvrhi::Format c_EnvironmentFormat = nvrhi::Format::RGBA16_FLOAT;
		constexpr nvrhi::Format c_AOFormat = nvrhi::Format::R32_FLOAT; // r32f storage needs no optional device feature

		constexpr uint32_t c_EnvironmentCubeSize = 512;
		constexpr uint32_t c_IrradianceSize = 32;
		constexpr uint32_t c_PrefilteredSize = 256;
		constexpr uint32_t c_PrefilteredMipCount = 6; // Roughness 0, 0.2, ... 1.0
		constexpr uint32_t c_IrradianceSamples = 256;
		constexpr uint32_t c_PrefilterSamples = 512;
		constexpr uint32_t c_BRDFLutSize = 128;
		constexpr uint32_t c_BRDFLutSamples = 512;

		// Light clusters: grid limits and the largest cluster buffer the renderer allocates.
		constexpr uint32_t c_MaxClusterGridSize = 64;
		constexpr uint32_t c_MaxLightsPerCluster = 1024;
		constexpr uint64_t c_MaxClusterBufferSize = 256ull * 1024 * 1024;
		constexpr uint32_t c_ClusterThreadGroupSize = 64; // Scene/LightClusters.comp
		// View depth where the last depth slice ends: it also holds everything beyond the camera's far distance.
		constexpr float c_ClusterDepthEnd = 1.0e30f;

		// Shadow casters further than this toward the light than a cascade's bounding sphere are flattened onto the
		// cascade's near plane (ShadowMesh.vert) instead of extending its depth range.
		constexpr float c_MaxShadowCasterDistance = 100000.0f;
		// Rasterizer depth bias of the shadow pass (reversed-Z: negative values push depth away from the light). The
		// constant part is in units of the depth format's resolution, the slope part scales the depth slope per texel.
		constexpr int c_ShadowDepthBias = -16;
		constexpr float c_ShadowSlopeDepthBias = -1.5f;

		uint32_t MipCountFor(uint32_t size)
		{
			uint32_t count = 1;
			while (size > 1)
			{
				size >>= 1;
				count++;
			}
			return count;
		}

		uint32_t DispatchGroups(uint32_t size)
		{
			return (size + 7) / 8;
		}

		float SRGBToLinear(float value)
		{
			return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
		}

		uint32_t PackTextureSlot(uint32_t slot, uint32_t samplerIndex)
		{
			return (slot & 0xFFFFFFu) | (samplerIndex << 24);
		}

		uint32_t GetSamplerIndex(const TextureSpecification& specification)
		{
			return static_cast<uint32_t>(specification.Filter) * 3 + static_cast<uint32_t>(specification.Wrap);
		}

		nvrhi::TextureHandle CreateRenderTarget(nvrhi::IDevice* device, uint32_t width, uint32_t height, nvrhi::Format format, const std::string& name, bool depth)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = format;
			desc.debugName = name;
			desc.isRenderTarget = true;
			desc.initialState = depth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			return device->createTexture(desc);
		}

		// Smallest sphere around the region a spot light reaches: the part of its range sphere inside the cone.
		glm::vec4 SpotLightBounds(const glm::vec3& position, const glm::vec3& direction, float range, float cosOuter, float sinOuter)
		{
			if (cosOuter >= 0.70710678f)
			{
				// Up to 45 degrees the apex and the rim of the spherical cap lie on the bounding sphere.
				const float radius = range / (2.0f * cosOuter);
				return glm::vec4(position + direction * radius, radius);
			}
			// Wider cones are bounded by the sphere through the rim of the cap.
			return glm::vec4(position + direction * (range * cosOuter), range * sinOuter);
		}

		bool IsFinite(const glm::mat4& matrix)
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

	}

	SceneCamera SceneCamera::FromEntity(const Scene& scene, Entity entity, float aspectRatio)
	{
		SceneCamera camera;
		if (!entity || !entity.HasComponent<CameraComponent>())
			return camera;

		const CameraComponent& component = entity.GetComponent<CameraComponent>();
		glm::vec3 translation(0.0f);
		glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 scale(1.0f);
		Math::DecomposeTransform(scene.GetWorldTransform(entity), translation, rotation, scale);

		// Scale never distorts the view: the camera uses only the position and orientation of its entity.
		camera.View = glm::inverse(glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation));
		camera.Projection = component.GetProjection(aspectRatio);
		camera.Position = translation;
		camera.Orthographic = component.Projection == ProjectionType::Orthographic;
		camera.Near = component.GetNearClip();
		camera.Far = camera.Orthographic ? component.OrthographicFar : component.PerspectiveFar;
		camera.VerticalFOV = camera.Orthographic ? component.OrthographicSize : glm::radians(component.PerspectiveFOV);
		camera.ClearColor = component.ClearColor;
		return camera;
	}

	SceneRenderer::SceneRenderer(const SceneRendererSpecification& specification)
		: m_Specification(specification)
	{
		ST_CORE_VERIFY(Renderer::IsInitialized(), "SceneRenderer requires an initialized renderer");
		m_Device = Renderer::GetDevice();
		CreateResources();
	}

	SceneRenderer::~SceneRenderer()
	{
		Renderer::GetGraphicsDevice().WaitForIdle();
	}

	void SceneRenderer::CreateResources()
	{
		ShaderLibrary& shaders = Renderer::GetShaderLibrary();
		m_MeshVertexShader = shaders.Get("Scene/Mesh.vert");
		m_PrepassPixelShader = shaders.Get("Scene/DepthPrepass.frag");
		m_ForwardPixelShader = shaders.Get("Scene/Forward.frag");
		m_ForwardTransparentPixelShader = shaders.Get("Scene/ForwardTransparent.frag");
		m_FullscreenVertexShader = shaders.Get("PostProcess/Fullscreen.vert");
		m_TonemapPixelShader = shaders.Get("PostProcess/Tonemap.frag");
		m_SkyboxPixelShader = shaders.Get("Scene/Skybox.frag");
		m_ShadowVertexShader = shaders.Get("Scene/ShadowMesh.vert");
		m_ShadowMaskPixelShader = shaders.Get("Scene/ShadowMask.frag");
		m_FXAAPixelShader = shaders.Get("PostProcess/FXAA.frag");
		ST_CORE_VERIFY(m_MeshVertexShader && m_PrepassPixelShader && m_ForwardPixelShader && m_ForwardTransparentPixelShader && m_FullscreenVertexShader
			&& m_TonemapPixelShader && m_SkyboxPixelShader && m_ShadowVertexShader && m_ShadowMaskPixelShader && m_FXAAPixelShader, "SceneRenderer shaders are missing");

		m_Specification.ShadowCascades = std::clamp<uint32_t>(m_Specification.ShadowCascades, 1, RenderData::c_MaxCascades);
		m_Specification.ShadowMapSize = std::clamp<uint32_t>(m_Specification.ShadowMapSize, 256, 8192);
		m_Specification.MaxLights = std::max(m_Specification.MaxLights, 1u);
		glm::uvec3& grid = m_Specification.LightClusterGrid;
		grid = glm::clamp(grid, glm::uvec3(1), glm::uvec3(c_MaxClusterGridSize));
		m_Specification.MaxLightsPerCluster = std::clamp(m_Specification.MaxLightsPerCluster, 1u, c_MaxLightsPerCluster);
		const uint64_t clusterCount = static_cast<uint64_t>(grid.x) * grid.y * grid.z;
		const uint64_t maxLightsPerCluster = c_MaxClusterBufferSize / (clusterCount * sizeof(uint32_t)) - 1;
		if (m_Specification.MaxLightsPerCluster > maxLightsPerCluster)
		{
			ST_CORE_WARN("SceneRenderer '{}': {} lights per cluster exceed the cluster memory limit; using {}", m_Specification.DebugName,
				m_Specification.MaxLightsPerCluster, maxLightsPerCluster);
			m_Specification.MaxLightsPerCluster = static_cast<uint32_t>(maxLightsPerCluster);
		}

		const nvrhi::VertexAttributeDesc attributes[] = {
			nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0).setOffset(0).setElementStride(sizeof(glm::vec3)),
			nvrhi::VertexAttributeDesc().setName("NORMAL").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(1)
				.setOffset(offsetof(MeshVertexAttributes, Normal)).setElementStride(sizeof(MeshVertexAttributes)),
			nvrhi::VertexAttributeDesc().setName("TANGENT").setFormat(nvrhi::Format::RGBA32_FLOAT).setBufferIndex(1)
				.setOffset(offsetof(MeshVertexAttributes, Tangent)).setElementStride(sizeof(MeshVertexAttributes)),
			nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setBufferIndex(1)
				.setOffset(offsetof(MeshVertexAttributes, TexCoord)).setElementStride(sizeof(MeshVertexAttributes))
		};
		m_InputLayout = m_Device->createInputLayout(attributes, static_cast<uint32_t>(std::size(attributes)), m_MeshVertexShader);

		nvrhi::BindingLayoutDesc sceneLayout;
		sceneLayout.visibility = nvrhi::ShaderType::All;
		sceneLayout.bindings = {
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
			nvrhi::BindingLayoutItem::Texture_SRV(3),
			nvrhi::BindingLayoutItem::Texture_SRV(4),
			nvrhi::BindingLayoutItem::Texture_SRV(5),
			nvrhi::BindingLayoutItem::Texture_SRV(6),
			nvrhi::BindingLayoutItem::Texture_SRV(7),
			nvrhi::BindingLayoutItem::Texture_SRV(11),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(12),
			nvrhi::BindingLayoutItem::Sampler(0).setSize(RenderData::c_SamplerCount),
			nvrhi::BindingLayoutItem::Sampler(8),
			nvrhi::BindingLayoutItem::Sampler(9),
			nvrhi::BindingLayoutItem::Sampler(10)
		};
		m_SceneBindingLayout = m_Device->createBindingLayout(sceneLayout);

		nvrhi::BindingLayoutDesc shadowPushLayout;
		shadowPushLayout.visibility = nvrhi::ShaderType::Vertex;
		// Slot 1: register b0 belongs to the frame constants of the scene layout.
		shadowPushLayout.bindings = { nvrhi::BindingLayoutItem::PushConstants(1, sizeof(uint32_t)) };
		m_ShadowPushLayout = m_Device->createBindingLayout(shadowPushLayout);
		nvrhi::BindingSetDesc shadowPushSet;
		shadowPushSet.bindings = { nvrhi::BindingSetItem::PushConstants(1, sizeof(uint32_t)) };
		m_ShadowPushBindingSet = m_Device->createBindingSet(shadowPushSet, m_ShadowPushLayout);

		nvrhi::BindingLayoutDesc tonemapLayout;
		tonemapLayout.visibility = nvrhi::ShaderType::Pixel;
		tonemapLayout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::Texture_SRV(1),
			nvrhi::BindingLayoutItem::Sampler(1),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::TonemapParameters))
		};
		m_TonemapBindingLayout = m_Device->createBindingLayout(tonemapLayout);

		// Material samplers, indexed by TextureFilter * 3 + TextureWrap.
		for (TextureFilter filter : { TextureFilter::Linear, TextureFilter::Nearest })
		{
			for (nvrhi::SamplerAddressMode wrap : { nvrhi::SamplerAddressMode::Wrap, nvrhi::SamplerAddressMode::Clamp, nvrhi::SamplerAddressMode::Mirror })
			{
				nvrhi::SamplerDesc desc;
				desc.setAllAddressModes(wrap);
				desc.setAllFilters(filter == TextureFilter::Linear);
				if (filter == TextureFilter::Linear)
					desc.setMaxAnisotropy(16.0f);
				m_MaterialSamplers.push_back(m_Device->createSampler(desc));
				ST_CORE_VERIFY(m_MaterialSamplers.back(), "SceneRenderer: failed to create a material sampler");
			}
		}
		nvrhi::SamplerDesc pointDesc;
		pointDesc.setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_PointClampSampler = m_Device->createSampler(pointDesc);
		nvrhi::SamplerDesc linearClampDesc;
		linearClampDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		m_LinearClampSampler = m_Device->createSampler(linearClampDesc);
		// Equirectangular maps wrap around horizontally but not across the poles.
		nvrhi::SamplerDesc equirectDesc;
		equirectDesc.setAllFilters(true).setAddressU(nvrhi::SamplerAddressMode::Wrap).setAddressV(nvrhi::SamplerAddressMode::Clamp)
			.setAddressW(nvrhi::SamplerAddressMode::Clamp);
		m_EquirectSampler = m_Device->createSampler(equirectDesc);
		nvrhi::SamplerDesc compareDesc;
		compareDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp).setReductionType(nvrhi::SamplerReductionType::Comparison);
		m_ShadowCompareSampler = m_Device->createSampler(compareDesc);

		nvrhi::TextureDesc dummyShadowDesc;
		dummyShadowDesc.width = 1;
		dummyShadowDesc.height = 1;
		dummyShadowDesc.arraySize = RenderData::c_MaxCascades;
		dummyShadowDesc.dimension = nvrhi::TextureDimension::Texture2DArray;
		dummyShadowDesc.format = nvrhi::Format::D32;
		dummyShadowDesc.isRenderTarget = true;
		dummyShadowDesc.debugName = m_Specification.DebugName + ".NoShadows";
		dummyShadowDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		dummyShadowDesc.keepInitialState = true;
		m_DummyShadowMap = m_Device->createTexture(dummyShadowDesc);

		m_FrameConstantBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(sizeof(RenderData::FrameConstants))
			.setIsConstantBuffer(true)
			.setIsVolatile(true)
			.setMaxVersions(16)
			.setDebugName(m_Specification.DebugName + ".FrameConstants"));

		bool recreated = false;
		const bool buffersCreated = EnsureBufferCapacity(m_InstanceBuffer, 1024, sizeof(RenderData::InstanceData), "Instances", recreated)
			&& EnsureBufferCapacity(m_MaterialBuffer, 256, sizeof(RenderData::MaterialData), "Materials", recreated)
			&& EnsureBufferCapacity(m_LightBuffer, 64, sizeof(RenderData::LightData), "Lights", recreated)
			&& EnsureBufferCapacity(m_LightBoundsBuffer, 64, sizeof(RenderData::LightBounds), "LightBounds", recreated)
			&& EnsureBufferCapacity(m_ShadowInstanceBuffer, 1024, sizeof(RenderData::InstanceData), "ShadowInstances", recreated);
		ST_CORE_VERIFY(buffersCreated, "SceneRenderer: failed to create the scene buffers");

		m_ClusterBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(clusterCount * (m_Specification.MaxLightsPerCluster + 1) * sizeof(uint32_t))
			.setStructStride(sizeof(uint32_t))
			.setCanHaveUAVs(true)
			.setInitialState(nvrhi::ResourceStates::ShaderResource)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".LightClusters"));
		nvrhi::BindingLayoutDesc clusterLayout;
		clusterLayout.visibility = nvrhi::ShaderType::Compute;
		clusterLayout.bindings = {
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(13),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0)
		};
		m_ClusterBindingLayout = m_Device->createBindingLayout(clusterLayout);

		m_CommandList = m_Device->createCommandList();
		nvrhi::BindingLayoutDesc aoLayout;
		aoLayout.visibility = nvrhi::ShaderType::Compute;
		aoLayout.bindings = {
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::Texture_SRV(8),
			nvrhi::BindingLayoutItem::Texture_SRV(9),
			nvrhi::BindingLayoutItem::Texture_SRV(10),
			nvrhi::BindingLayoutItem::Texture_UAV(0),
			nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::AOParameters))
		};
		m_AOBindingLayout = m_Device->createBindingLayout(aoLayout);

		auto createComputePipeline = [&](const char* name, nvrhi::IBindingLayout* layout)
		{
			nvrhi::ComputePipelineDesc desc;
			desc.CS = shaders.Get(name);
			desc.bindingLayouts = { layout };
			ST_CORE_VERIFY(desc.CS, "SceneRenderer: shader {} is missing", name);
			return m_Device->createComputePipeline(desc);
		};
		m_GTAOPipeline = createComputePipeline("Effects/GTAO.comp", m_AOBindingLayout);
		m_AOBlurPipeline = createComputePipeline("Effects/AOBlur.comp", m_AOBindingLayout);
		m_ClusterPipeline = createComputePipeline("Scene/LightClusters.comp", m_ClusterBindingLayout);

		nvrhi::BindingLayoutDesc exposureLayout;
		exposureLayout.visibility = nvrhi::ShaderType::Compute;
		exposureLayout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::ExposureParameters))
		};
		m_ExposureBindingLayout = m_Device->createBindingLayout(exposureLayout);
		m_HistogramPipeline = createComputePipeline("PostProcess/Histogram.comp", m_ExposureBindingLayout);
		m_ExposurePipeline = createComputePipeline("PostProcess/Exposure.comp", m_ExposureBindingLayout);
		m_HistogramBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(RenderData::c_HistogramBins * sizeof(uint32_t))
			.setStructStride(sizeof(uint32_t))
			.setCanHaveUAVs(true)
			.setInitialState(nvrhi::ResourceStates::UnorderedAccess)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".LuminanceHistogram"));
		m_ExposureBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(4 * sizeof(float))
			.setStructStride(sizeof(float))
			.setCanHaveUAVs(true)
			.setInitialState(nvrhi::ResourceStates::ShaderResource)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".Exposure"));

		nvrhi::BindingLayoutDesc bloomLayout;
		bloomLayout.visibility = nvrhi::ShaderType::Compute;
		bloomLayout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::Texture_UAV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::BloomParameters))
		};
		m_BloomBindingLayout = m_Device->createBindingLayout(bloomLayout);
		m_BloomPipeline = createComputePipeline("PostProcess/Bloom.comp", m_BloomBindingLayout);

		nvrhi::BindingLayoutDesc fxaaLayout;
		fxaaLayout.visibility = nvrhi::ShaderType::Pixel;
		fxaaLayout.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0) };
		m_FXAABindingLayout = m_Device->createBindingLayout(fxaaLayout);

		ST_CORE_VERIFY(m_InputLayout && m_SceneBindingLayout && m_TonemapBindingLayout && m_FrameConstantBuffer && m_CommandList && m_ShadowPushBindingSet
			&& m_PointClampSampler && m_LinearClampSampler && m_EquirectSampler && m_ShadowCompareSampler && m_DummyShadowMap && m_ClusterBuffer
			&& m_ClusterBindingLayout && m_ClusterPipeline && m_AOBindingLayout && m_GTAOPipeline && m_AOBlurPipeline && m_ExposureBindingLayout
			&& m_HistogramPipeline && m_ExposurePipeline && m_HistogramBuffer && m_ExposureBuffer && m_BloomBindingLayout && m_BloomPipeline && m_FXAABindingLayout,
			"SceneRenderer: failed to create GPU resources");
		CreateIBLResources();
		CreateOverlayResources();
	}

	void SceneRenderer::CreateIBLResources()
	{
		ShaderLibrary& shaders = Renderer::GetShaderLibrary();
		nvrhi::BindingLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::Compute;
		layoutDesc.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
			nvrhi::BindingLayoutItem::Texture_UAV(0),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::IBLParameters))
		};
		m_IBLBindingLayout = m_Device->createBindingLayout(layoutDesc);

		auto createPipeline = [&](const char* name)
		{
			nvrhi::ShaderHandle shader = shaders.Get(name);
			ST_CORE_VERIFY(shader, "SceneRenderer: shader {} is missing", name);
			nvrhi::ComputePipelineDesc desc;
			desc.CS = shader;
			desc.bindingLayouts = { m_IBLBindingLayout };
			nvrhi::ComputePipelineHandle pipeline = m_Device->createComputePipeline(desc);
			ST_CORE_VERIFY(pipeline, "SceneRenderer: failed to create the {} pipeline", name);
			return pipeline;
		};
		m_EquirectToCubePipeline = createPipeline("IBL/EquirectToCube.comp");
		m_CubeDownsamplePipeline = createPipeline("IBL/CubeDownsample.comp");
		m_IrradiancePipeline = createPipeline("IBL/Irradiance.comp");
		m_PrefilterPipeline = createPipeline("IBL/Prefilter.comp");
		nvrhi::ComputePipelineHandle brdfPipeline = createPipeline("IBL/BRDFLut.comp");

		// The environment BRDF depends on nothing but the shading model: compute it once.
		nvrhi::TextureDesc lutDesc;
		lutDesc.width = c_BRDFLutSize;
		lutDesc.height = c_BRDFLutSize;
		lutDesc.format = nvrhi::Format::RG16_FLOAT;
		lutDesc.isUAV = true;
		lutDesc.debugName = m_Specification.DebugName + ".BRDFLut";
		lutDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		lutDesc.keepInitialState = true;
		m_BRDFLut = m_Device->createTexture(lutDesc);
		ST_CORE_VERIFY(m_BRDFLut, "SceneRenderer: failed to create the BRDF lookup table");

		m_CommandList->open();
		DispatchIBL(m_CommandList, brdfPipeline, Renderer::GetWhiteTexture(), nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D, m_BRDFLut, 0,
			nvrhi::TextureDimension::Texture2D, RenderData::IBLParameters { c_BRDFLutSize, 0, 0.0f, c_BRDFLutSamples }, 1);
		m_CommandList->close();
		m_Device->executeCommandList(m_CommandList);
	}

	void SceneRenderer::DispatchIBL(nvrhi::ICommandList* commandList, nvrhi::IComputePipeline* pipeline, nvrhi::ITexture* source,
		const nvrhi::TextureSubresourceSet& sourceSubresources, nvrhi::TextureDimension sourceDimension, nvrhi::ITexture* output, uint32_t outputMip,
		nvrhi::TextureDimension outputDimension, const RenderData::IBLParameters& parameters, uint32_t layers)
	{
		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, source, nvrhi::Format::UNKNOWN, sourceSubresources, sourceDimension),
			nvrhi::BindingSetItem::Sampler(0, sourceDimension == nvrhi::TextureDimension::Texture2D ? m_EquirectSampler.Get() : m_LinearClampSampler.Get()),
			nvrhi::BindingSetItem::Texture_UAV(0, output, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(outputMip, 1, 0, layers), outputDimension),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::IBLParameters))
		};
		nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(setDesc, m_IBLBindingLayout);
		ST_CORE_VERIFY(bindingSet, "SceneRenderer: failed to create an image-based lighting binding set");

		nvrhi::ComputeState state;
		state.pipeline = pipeline;
		state.bindings = { bindingSet };
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch(DispatchGroups(parameters.OutputSize), DispatchGroups(parameters.OutputSize), layers);
	}

	void SceneRenderer::ProcessEnvironment(nvrhi::ICommandList* commandList, const Ref<Texture>& source)
	{
		ST_PROFILE_FUNCTION();
		auto createCube = [&](uint32_t size, uint32_t mips, const char* name)
		{
			nvrhi::TextureDesc desc;
			desc.width = size;
			desc.height = size;
			desc.arraySize = 6;
			desc.mipLevels = mips;
			desc.dimension = nvrhi::TextureDimension::TextureCube;
			desc.format = c_EnvironmentFormat;
			desc.isUAV = true;
			desc.debugName = m_Specification.DebugName + "." + name;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			nvrhi::TextureHandle texture = m_Device->createTexture(desc);
			ST_CORE_VERIFY(texture, "SceneRenderer: failed to create the {} cubemap", name);
			return texture;
		};

		EnvironmentMaps maps;
		maps.Source = source;
		const uint32_t cubeMips = MipCountFor(c_EnvironmentCubeSize);
		maps.Cube = createCube(c_EnvironmentCubeSize, cubeMips, "EnvironmentCube");
		maps.Irradiance = createCube(c_IrradianceSize, 1, "Irradiance");
		maps.Prefiltered = createCube(c_PrefilteredSize, c_PrefilteredMipCount, "Prefiltered");

		commandList->beginMarker("Environment");
		const uint32_t sourceWidth = source->GetWidth();
		DispatchIBL(commandList, m_EquirectToCubePipeline, source->GetGPUTexture(), nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D, maps.Cube, 0,
			nvrhi::TextureDimension::Texture2DArray, RenderData::IBLParameters { c_EnvironmentCubeSize, sourceWidth, 0.0f, 1 }, 6);
		for (uint32_t mip = 1; mip < cubeMips; mip++)
		{
			const uint32_t size = std::max(1u, c_EnvironmentCubeSize >> mip);
			DispatchIBL(commandList, m_CubeDownsamplePipeline, maps.Cube, nvrhi::TextureSubresourceSet(mip - 1, 1, 0, 6), nvrhi::TextureDimension::Texture2DArray,
				maps.Cube, mip, nvrhi::TextureDimension::Texture2DArray, RenderData::IBLParameters { size, size * 2, 0.0f, 1 }, 6);
		}

		const nvrhi::TextureSubresourceSet wholeCube(0, cubeMips, 0, 6);
		DispatchIBL(commandList, m_IrradiancePipeline, maps.Cube, wholeCube, nvrhi::TextureDimension::TextureCube, maps.Irradiance, 0,
			nvrhi::TextureDimension::Texture2DArray, RenderData::IBLParameters { c_IrradianceSize, c_EnvironmentCubeSize, 0.0f, c_IrradianceSamples }, 6);
		for (uint32_t mip = 0; mip < c_PrefilteredMipCount; mip++)
		{
			const uint32_t size = std::max(1u, c_PrefilteredSize >> mip);
			const float roughness = static_cast<float>(mip) / static_cast<float>(c_PrefilteredMipCount - 1);
			DispatchIBL(commandList, m_PrefilterPipeline, maps.Cube, wholeCube, nvrhi::TextureDimension::TextureCube, maps.Prefiltered, mip,
				nvrhi::TextureDimension::Texture2DArray, RenderData::IBLParameters { size, c_EnvironmentCubeSize, roughness, c_PrefilterSamples }, 6);
		}
		commandList->endMarker();
		m_Environment = std::move(maps);
	}

	void SceneRenderer::UpdateEnvironment(Scene& scene, RenderData::FrameConstants& frame, nvrhi::ICommandList* commandList)
	{
		frame.EnvironmentParams = glm::vec4(0.0f);
		frame.SkyParams = glm::vec4(0.0f);

		const SkyLightComponent* skyLight = nullptr;
		for (auto [entity, sky, world] : scene.GetRegistry().view<SkyLightComponent, WorldTransformComponent>().each())
		{
			if (world.ActiveInHierarchy)
			{
				skyLight = &sky;
				break;
			}
		}
		if (!skyLight || !skyLight->EnvironmentMap.IsValid())
		{
			m_Environment = {}; // Release the maps of a removed environment
			return;
		}

		Ref<Texture> texture = AssetManager::GetAsset<Texture>(skyLight->EnvironmentMap, AssetPriority::High);
		if (!texture || !texture->GetGPUTexture())
		{
			CountPendingAsset(skyLight->EnvironmentMap);
			return; // Constant ambient until the environment arrives
		}
		if (texture != m_Environment.Source)
			ProcessEnvironment(commandList, texture);

		frame.EnvironmentParams = glm::vec4(std::max(skyLight->Intensity, 0.0f), glm::radians(skyLight->Rotation), static_cast<float>(c_PrefilteredMipCount - 1), 1.0f);
		const float backgroundMips = static_cast<float>(MipCountFor(c_EnvironmentCubeSize) - 1);
		frame.SkyParams = glm::vec4(std::clamp(skyLight->BackgroundBlur, 0.0f, 1.0f) * backgroundMips, skyLight->ShowBackground ? 1.0f : 0.0f, 0.0f, 0.0f);
		m_Stats.EnvironmentLighting = true;
	}

	bool SceneRenderer::CreateSceneBindingSets()
	{
		nvrhi::ITexture* blackCube = Renderer::GetBlackCubeTexture();
		const bool hasEnvironment = m_Environment.Cube != nullptr;
		nvrhi::ITexture* shadowMap = m_ShadowMap ? m_ShadowMap.Get() : m_DummyShadowMap.Get();

		// The shadow pass reads the same resources except for its own instance list (and never the shadow map).
		auto create = [&](nvrhi::IBuffer* instances, nvrhi::ITexture* boundShadowMap)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(0, instances),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_MaterialBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_LightBuffer),
				nvrhi::BindingSetItem::Texture_SRV(3, hasEnvironment ? m_Environment.Irradiance.Get() : blackCube),
				nvrhi::BindingSetItem::Texture_SRV(4, hasEnvironment ? m_Environment.Prefiltered.Get() : blackCube),
				nvrhi::BindingSetItem::Texture_SRV(5, m_BRDFLut),
				nvrhi::BindingSetItem::Texture_SRV(6, hasEnvironment ? m_Environment.Cube.Get() : blackCube),
				nvrhi::BindingSetItem::Texture_SRV(7, boundShadowMap),
				nvrhi::BindingSetItem::Texture_SRV(11, m_AOTexture),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(12, m_ClusterBuffer),
				nvrhi::BindingSetItem::Sampler(8, m_LinearClampSampler),
				nvrhi::BindingSetItem::Sampler(9, m_ShadowCompareSampler),
				nvrhi::BindingSetItem::Sampler(10, m_PointClampSampler)
			};
			for (uint32_t index = 0; index < RenderData::c_SamplerCount; index++)
				desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(0, m_MaterialSamplers[index]).setArrayElement(index));
			return m_Device->createBindingSet(desc, m_SceneBindingLayout);
		};
		m_SceneBindingSet = create(m_InstanceBuffer, shadowMap);
		m_ShadowBindingSet = create(m_ShadowInstanceBuffer, m_DummyShadowMap);

		nvrhi::BindingSetDesc clusterSet;
		clusterSet.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_LightBoundsBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_ClusterBuffer)
		};
		m_ClusterBindingSet = m_Device->createBindingSet(clusterSet, m_ClusterBindingLayout);

		m_BoundEnvironment = m_Environment.Cube;
		m_BoundShadowMap = shadowMap;
		if (!m_SceneBindingSet || !m_ShadowBindingSet || !m_ClusterBindingSet)
		{
			m_SceneBindingSet = nullptr; // Recreated next frame
			return false;
		}
		return true;
	}

	PostProcessComponent SceneRenderer::GetPostProcessSettings(Scene& scene)
	{
		for (auto [entity, postProcess, world] : scene.GetRegistry().view<PostProcessComponent, WorldTransformComponent>().each())
		{
			if (world.ActiveInHierarchy)
				return postProcess;
		}
		return PostProcessComponent();
	}

	void SceneRenderer::RenderAmbientOcclusion(nvrhi::ICommandList* commandList, const PostProcessComponent& settings)
	{
		const RenderData::AOParameters parameters = { std::clamp(settings.AmbientOcclusionRadius, 0.01f, 10.0f),
			std::clamp(settings.AmbientOcclusionIntensity, 0.0f, 8.0f), 2, 6 };
		const uint32_t groupsX = DispatchGroups(m_ViewportSize.x);
		const uint32_t groupsY = DispatchGroups(m_ViewportSize.y);

		nvrhi::ComputeState state;
		state.pipeline = m_GTAOPipeline;
		state.bindings = { m_GTAOBindingSet };
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch(groupsX, groupsY, 1);

		state.pipeline = m_AOBlurPipeline;
		state.bindings = { m_AOBlurBindingSet };
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch(groupsX, groupsY, 1);
	}

	void SceneRenderer::UpdateExposure(nvrhi::ICommandList* commandList, const PostProcessComponent& settings)
	{
		const auto now = std::chrono::steady_clock::now();
		// Rendering pauses (breakpoints, minimized windows) must not turn into one huge adaptation step.
		const float deltaTime = m_LastExposureTime ? std::clamp(std::chrono::duration<float>(now - *m_LastExposureTime).count(), 0.0f, 0.25f) : 0.0f;
		m_LastExposureTime = now;
		const float compensation = std::exp2(std::clamp(settings.Exposure, -20.0f, 20.0f));

		if (!settings.AutoExposure)
		{
			const float values[4] = { 0.0f, compensation, 0.0f, 0.0f };
			commandList->writeBuffer(m_ExposureBuffer, values, sizeof(values));
			m_ResetExposure = true; // Automatic exposure starts from the scene's brightness when turned on
			return;
		}

		RenderData::ExposureParameters parameters = {};
		parameters.MinLog2 = std::clamp(settings.AutoExposureMinEV, RenderData::c_HistogramMinLog2, RenderData::c_HistogramMaxLog2);
		parameters.MaxLog2 = std::clamp(settings.AutoExposureMaxEV, parameters.MinLog2, RenderData::c_HistogramMaxLog2);
		parameters.AdaptFactor = m_ResetExposure || settings.AutoExposureSpeed <= 0.0f ? 1.0f : 1.0f - std::exp(-deltaTime * settings.AutoExposureSpeed);
		parameters.Compensation = compensation;
		// The darkest and brightest 10% of the pixels (deep shadows, light sources) do not move the exposure.
		parameters.LowPercentile = 0.1f;
		parameters.HighPercentile = 0.9f;
		m_ResetExposure = false;

		commandList->clearBufferUInt(m_HistogramBuffer, 0);
		nvrhi::ComputeState state;
		state.pipeline = m_HistogramPipeline;
		state.bindings = { m_ExposureBindingSet };
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch((m_ViewportSize.x + 15) / 16, (m_ViewportSize.y + 15) / 16, 1);

		// NVRHI places barriers only when the binding sets change: the histogram must be complete before it is read.
		commandList->setBufferState(m_HistogramBuffer, nvrhi::ResourceStates::UnorderedAccess);
		commandList->commitBarriers();
		state.pipeline = m_ExposurePipeline;
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch(1, 1, 1);
	}

	void SceneRenderer::RenderBloom(nvrhi::ICommandList* commandList, const PostProcessComponent& settings)
	{
		RenderData::BloomParameters parameters = {};
		parameters.Threshold = std::max(settings.BloomThreshold, 0.0f);
		parameters.Knee = parameters.Threshold * 0.5f;

		const nvrhi::TextureDesc& desc = m_BloomTexture->getDesc();
		auto dispatch = [&](nvrhi::IBindingSet* bindingSet, uint32_t mode, uint32_t level)
		{
			nvrhi::ComputeState state;
			state.pipeline = m_BloomPipeline;
			state.bindings = { bindingSet };
			commandList->setComputeState(state);
			parameters.Mode = mode;
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->dispatch(DispatchGroups(std::max(1u, desc.width >> level)), DispatchGroups(std::max(1u, desc.height >> level)), 1);
		};

		dispatch(m_BloomDownsampleSets[0], RenderData::c_BloomPrefilter, 0);
		for (uint32_t level = 1; level < m_BloomLevels; level++)
			dispatch(m_BloomDownsampleSets[level], RenderData::c_BloomDownsample, level);
		for (uint32_t level = m_BloomLevels - 1; level-- > 0;)
			dispatch(m_BloomUpsampleSets[level], RenderData::c_BloomUpsample, level);
	}

	bool SceneRenderer::EnsureShadowMap()
	{
		if (m_ShadowMap)
			return true;

		nvrhi::TextureDesc desc;
		desc.width = m_Specification.ShadowMapSize;
		desc.height = m_Specification.ShadowMapSize;
		desc.arraySize = RenderData::c_MaxCascades;
		desc.dimension = nvrhi::TextureDimension::Texture2DArray;
		desc.format = nvrhi::Format::D32;
		desc.isRenderTarget = true;
		desc.debugName = m_Specification.DebugName + ".ShadowMap";
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		nvrhi::TextureHandle shadowMap = m_Device->createTexture(desc);
		if (!shadowMap)
			return false;

		std::vector<nvrhi::FramebufferHandle> framebuffers;
		for (uint32_t cascade = 0; cascade < RenderData::c_MaxCascades; cascade++)
		{
			nvrhi::FramebufferDesc framebuffer;
			framebuffer.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(shadowMap).setArraySlice(cascade));
			framebuffers.push_back(m_Device->createFramebuffer(framebuffer));
			if (!framebuffers.back())
				return false;
		}
		m_ShadowMap = shadowMap;
		m_ShadowFramebuffers = std::move(framebuffers);
		return true;
	}

	uint32_t SceneRenderer::SelectLOD(const Submesh& submesh, float screenFraction) const
	{
		uint32_t lod = 0;
		if (screenFraction < m_Specification.LODThreshold && screenFraction > 0.0f)
			lod = 1 + static_cast<uint32_t>(std::floor(std::log2(m_Specification.LODThreshold / screenFraction)));
		return std::min<uint32_t>(lod, static_cast<uint32_t>(submesh.LODs.size()) - 1);
	}

	float SceneRenderer::GetScreenFraction(const SceneCamera& camera, const glm::vec3& center, float radius)
	{
		// Projected diameter of the bounding sphere as a fraction of the viewport height.
		if (camera.Orthographic)
			return (2.0f * radius) / std::max(camera.VerticalFOV, 1e-4f);
		const float halfFOVTangent = std::tan(std::max(camera.VerticalFOV, 1e-3f) * 0.5f);
		const float distance = glm::length(center - camera.Position);
		return radius / (std::max(distance, camera.Near) * halfFOVTangent);
	}

	void SceneRenderer::BuildBatches(std::vector<DrawRecord>& records, const std::vector<RenderData::InstanceData>& payloads, std::vector<DrawBatch>& outBatches,
		std::vector<DrawBatch>* outTransparentBatches, std::vector<RenderData::InstanceData>& outInstances, bool shadows)
	{
		outBatches.clear();
		if (outTransparentBatches)
			outTransparentBatches->clear();
		outInstances.clear();
		outInstances.reserve(records.size());

		// Opaque surfaces and shadow casters are grouped into instanced draws (one pipeline, mesh, LOD and material per
		// batch); transparent surfaces follow, back to front, one draw each. The payload index (registry order) breaks
		// ties, so the order never depends on the sort algorithm.
		std::sort(records.begin(), records.end(), [](const DrawRecord& a, const DrawRecord& b)
		{
			if (a.Transparent != b.Transparent)
				return b.Transparent;
			if (a.Transparent)
				return a.ViewDepth != b.ViewDepth ? a.ViewDepth > b.ViewDepth : a.PayloadIndex < b.PayloadIndex;
			return std::make_tuple(a.Cascade, a.AlphaMask, a.Mirrored, a.DoubleSided, reinterpret_cast<uintptr_t>(a.MeshAsset), a.SubmeshIndex, a.LOD, a.MaterialIndex,
				a.PayloadIndex)
				< std::make_tuple(b.Cascade, b.AlphaMask, b.Mirrored, b.DoubleSided, reinterpret_cast<uintptr_t>(b.MeshAsset), b.SubmeshIndex, b.LOD, b.MaterialIndex,
					b.PayloadIndex);
		});

		for (const DrawRecord& record : records)
		{
			ST_CORE_ASSERT(!record.Transparent || outTransparentBatches, "SceneRenderer: transparent records need a batch list");
			std::vector<DrawBatch>& batches = record.Transparent ? *outTransparentBatches : outBatches;
			DrawBatch* last = batches.empty() ? nullptr : &batches.back();
			// Opaque shadow casters batch regardless of material; everything else needs its own material.
			const bool extendsBatch = last && !record.Transparent && last->Cascade == record.Cascade && last->MeshAsset == record.MeshAsset
				&& last->SubmeshIndex == record.SubmeshIndex && last->LOD == record.LOD && last->DoubleSided == record.DoubleSided
				&& last->Mirrored == record.Mirrored && last->AlphaMask == record.AlphaMask
				&& ((shadows && !record.AlphaMask) || last->MaterialIndex == record.MaterialIndex);
			if (extendsBatch)
			{
				last->InstanceCount++;
			}
			else
			{
				DrawBatch batch;
				batch.MeshAsset = record.MeshAsset;
				batch.SubmeshIndex = record.SubmeshIndex;
				batch.LOD = record.LOD;
				batch.MaterialIndex = record.MaterialIndex;
				batch.DoubleSided = record.DoubleSided;
				batch.Mirrored = record.Mirrored;
				batch.AlphaMask = record.AlphaMask;
				batch.Cascade = record.Cascade;
				batch.FirstInstance = static_cast<uint32_t>(outInstances.size());
				batch.InstanceCount = 1;
				batches.push_back(batch);
			}
			outInstances.push_back(payloads[record.PayloadIndex]);
		}
	}

	void SceneRenderer::CollectShadowCasters(Scene& scene, const SceneCamera& camera, RenderData::FrameConstants& frame)
	{
		m_ShadowInstances.clear();
		m_ShadowBatches.clear();
		m_ShadowRecords.clear();
		m_ShadowPayloads.clear();
		frame.ShadowParams = glm::vec4(0.0f);
		if (!m_ShadowLight.Enabled)
			return;

		const uint32_t cascadeCount = m_Specification.ShadowCascades;
		const float nearClip = std::max(camera.Near, 0.01f);
		const float farClip = std::clamp(m_ShadowLight.Distance, nearClip + 0.1f, std::max(camera.Far, nearClip + 0.1f));
		const float aspect = static_cast<float>(m_ViewportSize.x) / static_cast<float>(m_ViewportSize.y);
		const float tanHalfFOV = camera.Orthographic ? 0.0f : std::tan(camera.VerticalFOV * 0.5f);
		const glm::mat4 inverseView = glm::inverse(camera.View);
		const glm::vec3 lightDirection = m_ShadowLight.Direction;
		const glm::vec3 up = std::abs(lightDirection.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		const glm::mat4 lightView = glm::lookAt(glm::vec3(0.0f), lightDirection, up);
		const float mapSize = static_cast<float>(m_Specification.ShadowMapSize);

		// Each cascade is a box in light view space (looking along the light: -Z points away from it). Its square covers
		// the bounding sphere of a view frustum slice; its depth range is fitted to the casters below.
		struct CascadeBox
		{
			glm::vec2 Min;
			glm::vec2 Max;
			float FarZ;        // Far side of the slice's bounding sphere
			float SphereNearZ; // Near side (toward the light)
			float CasterNearZ; // Nearest caster toward the light
			float WorldPerTexel;
			float Radius;
		};
		std::array<CascadeBox, RenderData::c_MaxCascades> cascades = {};
		float sliceNear = nearClip;
		for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
		{
			// Practical split scheme: a blend of logarithmic and uniform splits.
			const float fraction = static_cast<float>(cascade + 1) / static_cast<float>(cascadeCount);
			const float logarithmic = nearClip * std::pow(farClip / nearClip, fraction);
			const float uniform = nearClip + (farClip - nearClip) * fraction;
			const float sliceFar = 0.8f * logarithmic + 0.2f * uniform;

			// Bounding sphere of the view frustum slice: its size does not change when the camera rotates, which keeps
			// the shadow texel footprint (and thus the edges) stable.
			std::array<glm::vec3, 8> corners;
			size_t cornerIndex = 0;
			for (float distance : { sliceNear, sliceFar })
			{
				const float halfHeight = camera.Orthographic ? camera.VerticalFOV * 0.5f : distance * tanHalfFOV;
				const float halfWidth = halfHeight * aspect;
				for (float x : { -1.0f, 1.0f })
				{
					for (float y : { -1.0f, 1.0f })
						corners[cornerIndex++] = glm::vec3(inverseView * glm::vec4(x * halfWidth, y * halfHeight, -distance, 1.0f));
				}
			}
			glm::vec3 center(0.0f);
			for (const glm::vec3& corner : corners)
				center += corner;
			center /= 8.0f;
			float radius = 0.0f;
			for (const glm::vec3& corner : corners)
				radius = std::max(radius, glm::length(corner - center));
			radius = std::ceil(radius * 16.0f) / 16.0f;

			// Snap the cascade origin to whole texels so edges do not shimmer while the camera moves.
			const float worldPerTexel = 2.0f * radius / mapSize;
			glm::vec3 lightCenter = glm::vec3(lightView * glm::vec4(center, 1.0f));
			lightCenter.x = std::floor(lightCenter.x / worldPerTexel) * worldPerTexel;
			lightCenter.y = std::floor(lightCenter.y / worldPerTexel) * worldPerTexel;

			CascadeBox& box = cascades[cascade];
			box.Min = glm::vec2(lightCenter) - radius;
			box.Max = glm::vec2(lightCenter) + radius;
			box.FarZ = lightCenter.z - radius;
			box.SphereNearZ = lightCenter.z + radius;
			box.CasterNearZ = box.SphereNearZ;
			box.WorldPerTexel = worldPerTexel;
			box.Radius = radius;
			frame.CascadeSplits[cascade] = sliceFar;
			sliceNear = sliceFar;
		}

		Ref<Material> defaultMaterial = AssetManager::GetAsset<Material>(BuiltinAssets::DefaultMaterial);
		for (auto [entity, meshRenderer, world] : scene.GetRegistry().view<MeshRendererComponent, WorldTransformComponent>().each())
		{
			if (!world.ActiveInHierarchy || !meshRenderer.CastShadows || !meshRenderer.Mesh.IsValid())
				continue;
			Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(meshRenderer.Mesh);
			if (!mesh || !mesh->GetIndexBuffer())
				continue;
			const float determinant = glm::determinant(glm::mat3(world.Matrix));
			if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f || !IsFinite(world.Matrix))
				continue;

			// A caster shadows the cascades whose square it overlaps and whose far side it does not lie beyond. Casters
			// closer to the light than the cascade's sphere are not culled: the depth range grows to include them.
			const AABB lightBounds = mesh->GetBounds().Transformed(lightView * world.Matrix);
			uint32_t cascadeMask = 0;
			for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
			{
				CascadeBox& box = cascades[cascade];
				if (lightBounds.Max.x < box.Min.x || lightBounds.Min.x > box.Max.x || lightBounds.Max.y < box.Min.y || lightBounds.Min.y > box.Max.y
					|| lightBounds.Max.z < box.FarZ)
					continue;
				cascadeMask |= 1u << cascade;
				box.CasterNearZ = std::max(box.CasterNearZ, lightBounds.Max.z);
			}
			if (cascadeMask == 0)
				continue;

			const AABB bounds = mesh->GetBounds().Transformed(world.Matrix);
			const float screenFraction = GetScreenFraction(camera, bounds.GetCenter(), glm::length(bounds.GetExtents()));
			const bool mirrored = determinant < 0.0f;

			// Materials are resolved once per submesh, not per cascade.
			const std::vector<Submesh>& submeshes = mesh->GetSubmeshes();
			m_CasterMaterials.assign(submeshes.size(), CasterMaterial());
			for (uint32_t submeshIndex = 0; submeshIndex < submeshes.size(); submeshIndex++)
			{
				const Submesh& submesh = submeshes[submeshIndex];
				const AssetHandle materialHandle = meshRenderer.Material.IsValid() ? meshRenderer.Material
					: (submesh.Material.IsValid() ? submesh.Material : BuiltinAssets::DefaultMaterial);
				Ref<Material> material = AssetManager::GetAsset<Material>(materialHandle);
				if (!material)
					material = defaultMaterial;
				if (!material || material->GetProperties().AlphaMode == MaterialAlphaMode::Blend)
					continue; // Transparent surfaces cast no shadows

				CasterMaterial& casterMaterial = m_CasterMaterials[submeshIndex];
				casterMaterial.MaterialIndex = GetMaterialIndex(material);
				casterMaterial.DoubleSided = material->GetProperties().DoubleSided;
				casterMaterial.AlphaMask = material->GetProperties().AlphaMode == MaterialAlphaMode::Mask;
				casterMaterial.Casts = true;
			}

			for (uint32_t submeshIndex = 0; submeshIndex < submeshes.size(); submeshIndex++)
			{
				const CasterMaterial& casterMaterial = m_CasterMaterials[submeshIndex];
				if (!casterMaterial.Casts)
					continue;

				// Shadow casters need no normal matrix: ShadowMesh.vert transforms positions only.
				RenderData::InstanceData payload = {};
				payload.World = world.Matrix;
				payload.MaterialIndex = casterMaterial.MaterialIndex;
				payload.EntityID = static_cast<uint32_t>(entity) + 1;
				payload.Flags = mirrored ? RenderData::c_InstanceMirrored : 0u;
				const uint32_t payloadIndex = static_cast<uint32_t>(m_ShadowPayloads.size());
				m_ShadowPayloads.push_back(payload);

				DrawRecord record;
				record.MeshAsset = mesh.get();
				record.SubmeshIndex = submeshIndex;
				record.LOD = SelectLOD(submeshes[submeshIndex], screenFraction);
				record.MaterialIndex = casterMaterial.MaterialIndex;
				record.PayloadIndex = payloadIndex;
				record.DoubleSided = casterMaterial.DoubleSided;
				record.Mirrored = mirrored;
				record.AlphaMask = casterMaterial.AlphaMask;
				for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
				{
					if ((cascadeMask & (1u << cascade)) == 0)
						continue;
					record.Cascade = cascade;
					m_ShadowRecords.push_back(record);
				}
			}
			m_FrameMeshes.push_back(std::move(mesh));
		}

		for (uint32_t cascade = 0; cascade < cascadeCount; cascade++)
		{
			const CascadeBox& box = cascades[cascade];
			// The depth range reaches from the far side of the slice's sphere to the nearest caster (up to a limit beyond
			// which ShadowMesh.vert flattens casters onto the near plane).
			const float nearZ = std::clamp(box.CasterNearZ, box.SphereNearZ, box.SphereNearZ + c_MaxShadowCasterDistance);
			const float nearPlane = -nearZ;
			const float farPlane = -box.FarZ;
			const glm::mat4 projection = Math::OrthographicReverseZ(box.Min.x, box.Max.x, box.Min.y, box.Max.y, nearPlane, farPlane);
			frame.CascadeViewProjection[cascade] = projection * lightView;
			// Penumbrae are searched for blockers up to this distance from the receiver (world units).
			const float searchDistance = 0.2f * box.Radius + 5.0f;
			frame.CascadeData[cascade] = glm::vec4(box.WorldPerTexel, farPlane - nearPlane, searchDistance, 0.0f);
		}

		const float halfAngle = glm::radians(std::clamp(m_ShadowLight.Softness, 0.0f, 10.0f)) * 0.5f;
		frame.ShadowParams = glm::vec4(2.0f * std::tan(halfAngle), 1.0f / mapSize, 0.0f, static_cast<float>(cascadeCount));
		frame.ShadowBias = glm::vec4(m_ShadowLight.Bias, m_ShadowLight.NormalBias, 0.0f, 0.0f);

		BuildBatches(m_ShadowRecords, m_ShadowPayloads, m_ShadowBatches, nullptr, m_ShadowInstances, true);
		m_Stats.ShadowCasters = static_cast<uint32_t>(m_ShadowInstances.size());
	}

	void SceneRenderer::DrawShadows(nvrhi::ICommandList* commandList, uint32_t cascadeCount)
	{
		commandList->clearDepthStencilTexture(m_ShadowMap, nvrhi::AllSubresources, true, 0.0f, false, 0);
		const float mapSize = static_cast<float>(m_Specification.ShadowMapSize);
		for (const DrawBatch& batch : m_ShadowBatches)
		{
			if (batch.Cascade >= cascadeCount)
				continue;
			const Submesh& submesh = batch.MeshAsset->GetSubmeshes()[batch.SubmeshIndex];
			const MeshLOD& lod = submesh.LODs[batch.LOD];

			nvrhi::GraphicsState state;
			state.pipeline = GetScenePipeline(PipelineKey { batch.AlphaMask ? Pass::ShadowMasked : Pass::Shadow, batch.DoubleSided, batch.Mirrored });
			state.framebuffer = m_ShadowFramebuffers[batch.Cascade];
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(mapSize, mapSize));
			state.bindings = { m_ShadowBindingSet, Renderer::GetBindlessTextures().GetTable(), m_ShadowPushBindingSet };
			state.vertexBuffers = {
				nvrhi::VertexBufferBinding { batch.MeshAsset->GetPositionBuffer(), 0, 0 },
				nvrhi::VertexBufferBinding { batch.MeshAsset->GetAttributeBuffer(), 1, 0 }
			};
			state.indexBuffer = nvrhi::IndexBufferBinding { batch.MeshAsset->GetIndexBuffer(), nvrhi::Format::R32_UINT, 0 };
			commandList->setGraphicsState(state);
			const uint32_t cascade = batch.Cascade;
			commandList->setPushConstants(&cascade, sizeof(cascade));

			nvrhi::DrawArguments arguments;
			arguments.vertexCount = lod.IndexCount;
			arguments.instanceCount = batch.InstanceCount;
			arguments.startIndexLocation = lod.IndexOffset;
			arguments.startVertexLocation = submesh.BaseVertex;
			arguments.startInstanceLocation = batch.FirstInstance;
			commandList->drawIndexed(arguments);
		}
	}

	bool SceneRenderer::EnsureBufferCapacity(nvrhi::BufferHandle& buffer, size_t elementCount, size_t elementSize, const char* name, bool& outRecreated, bool vertexBuffer)
	{
		const uint64_t required = std::max<uint64_t>(elementCount, 1) * elementSize;
		if (buffer && buffer->getDesc().byteSize >= required)
			return true;

		// Grow geometrically so a slowly growing scene does not reallocate every frame.
		uint64_t capacity = buffer ? buffer->getDesc().byteSize : elementSize * 64;
		while (capacity < required)
			capacity *= 2;

		nvrhi::BufferDesc desc;
		desc.byteSize = capacity;
		desc.debugName = m_Specification.DebugName + "." + name;
		if (vertexBuffer)
		{
			desc.isVertexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
		}
		else
		{
			desc.structStride = static_cast<uint32_t>(elementSize);
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
		}
		desc.keepInitialState = true;
		nvrhi::BufferHandle created = m_Device->createBuffer(desc);
		if (!created)
		{
			ReportError(fmt::format("failed to allocate the {} buffer ({} bytes)", name, capacity));
			return false;
		}
		buffer = created;
		outRecreated = true;
		return true;
	}

	void SceneRenderer::ReportError(const std::string& message)
	{
		if (message == m_LastError)
			return;
		m_LastError = message;
		ST_CORE_ERROR("SceneRenderer '{}': {}", m_Specification.DebugName, message);
	}

	void SceneRenderer::SetViewportSize(uint32_t width, uint32_t height)
	{
		if (width == m_ViewportSize.x && height == m_ViewportSize.y)
			return;
		m_ViewportSize = { width, height };
		m_TargetsValid = false;
		m_HasRenderedFrame = false;
		if (width == 0 || height == 0)
		{
			ReleaseRenderTargets();
			return;
		}

		const uint32_t limit = Renderer::GetGraphicsDevice().GetInfo().MaxTextureDimension2D;
		if (width > limit || height > limit)
		{
			ReleaseRenderTargets();
			ReportError(fmt::format("viewport size {}x{} exceeds the device's texture size limit ({})", width, height, limit));
			return;
		}
		m_TargetsValid = CreateRenderTargets();
	}

	void SceneRenderer::ReleaseRenderTargets()
	{
		// Targets may still be used by frames in flight; NVRHI keeps them alive until those complete.
		m_DepthTexture = nullptr;
		m_NormalTexture = nullptr;
		m_EntityIDTexture = nullptr;
		m_HDRTexture = nullptr;
		m_OutputTexture = nullptr;
		m_AORawTexture = nullptr;
		m_AOTexture = nullptr;
		m_GTAOBindingSet = nullptr;
		m_AOBlurBindingSet = nullptr;
		m_PrepassFramebuffer = nullptr;
		m_ForwardFramebuffer = nullptr;
		m_OutputFramebuffer = nullptr;
		m_ExposureBindingSet = nullptr;
		m_BloomTexture = nullptr;
		m_BloomLevels = 0;
		m_BloomDownsampleSets.clear();
		m_BloomUpsampleSets.clear();
		m_TonemapBindingSet = nullptr;
		m_LDRTexture = nullptr;
		m_LDRFramebuffer = nullptr;
		m_FXAABindingSet = nullptr;
		m_SceneBindingSet = nullptr; // References the ambient occlusion texture
		m_ShadowBindingSet = nullptr;
		m_OverlayFramebuffer = nullptr;
		m_CopyBindingSet = nullptr;
		m_OutlineBindingSet = nullptr; // References the entity-ID texture
	}

	bool SceneRenderer::CreateRenderTargets()
	{
		ReleaseRenderTargets();
		const uint32_t width = m_ViewportSize.x;
		const uint32_t height = m_ViewportSize.y;
		const std::string& name = m_Specification.DebugName;
		auto fail = [&](const char* what)
		{
			ReleaseRenderTargets();
			ReportError(fmt::format("failed to create the {} for a {}x{} viewport", what, width, height));
			return false;
		};

		m_DepthTexture = CreateRenderTarget(m_Device, width, height, c_DepthFormat, name + ".Depth", true);
		m_NormalTexture = CreateRenderTarget(m_Device, width, height, c_NormalFormat, name + ".Normals", false);
		m_EntityIDTexture = CreateRenderTarget(m_Device, width, height, c_EntityIDFormat, name + ".EntityIDs", false);
		m_HDRTexture = CreateRenderTarget(m_Device, width, height, c_HDRFormat, name + ".HDR", false);
		m_OutputTexture = CreateRenderTarget(m_Device, width, height, c_OutputFormat, name + ".Output", false);
		// Tone mapping writes here when FXAA follows.
		m_LDRTexture = CreateRenderTarget(m_Device, width, height, c_OutputFormat, name + ".LDR", false);
		auto createStorage = [&](const std::string& debugName)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = c_AOFormat;
			desc.isUAV = true;
			desc.debugName = debugName;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			return m_Device->createTexture(desc);
		};
		m_AORawTexture = createStorage(name + ".AORaw");
		m_AOTexture = createStorage(name + ".AO");
		if (!m_DepthTexture || !m_NormalTexture || !m_EntityIDTexture || !m_HDRTexture || !m_OutputTexture || !m_LDRTexture || !m_AORawTexture || !m_AOTexture)
			return fail("render targets");

		auto createAOSet = [&](nvrhi::ITexture* input, nvrhi::ITexture* output)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(8, m_DepthTexture),
				nvrhi::BindingSetItem::Texture_SRV(9, m_NormalTexture),
				nvrhi::BindingSetItem::Texture_SRV(10, input),
				nvrhi::BindingSetItem::Texture_UAV(0, output),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::AOParameters))
			};
			return m_Device->createBindingSet(desc, m_AOBindingLayout);
		};
		// The GTAO pass reads nothing through slot 10; it binds the blurred texture there, which it does not write.
		m_GTAOBindingSet = createAOSet(m_AOTexture, m_AORawTexture);
		m_AOBlurBindingSet = createAOSet(m_AORawTexture, m_AOTexture);

		m_PrepassFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
			.addColorAttachment(m_NormalTexture)
			.addColorAttachment(m_EntityIDTexture)
			.setDepthAttachment(m_DepthTexture));
		m_ForwardFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
			.addColorAttachment(m_HDRTexture)
			.setDepthAttachment(m_DepthTexture));
		m_OutputFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_OutputTexture));
		m_LDRFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_LDRTexture));
		if (!m_GTAOBindingSet || !m_AOBlurBindingSet || !m_PrepassFramebuffer || !m_ForwardFramebuffer || !m_OutputFramebuffer || !m_LDRFramebuffer)
			return fail("framebuffers");

		// Automatic exposure meters the HDR image.
		nvrhi::BindingSetDesc exposureSet;
		exposureSet.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, m_HDRTexture),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_HistogramBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_ExposureBuffer),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::ExposureParameters))
		};
		m_ExposureBindingSet = m_Device->createBindingSet(exposureSet, m_ExposureBindingLayout);

		// Bloom chain at half resolution, down to levels of a few pixels.
		const uint32_t bloomWidth = std::max(1u, (width + 1) / 2);
		const uint32_t bloomHeight = std::max(1u, (height + 1) / 2);
		uint32_t bloomLevels = 1;
		while (bloomLevels < RenderData::c_MaxBloomLevels && (std::min(bloomWidth, bloomHeight) >> bloomLevels) >= 4)
			bloomLevels++;
		nvrhi::TextureDesc bloomDesc;
		bloomDesc.width = bloomWidth;
		bloomDesc.height = bloomHeight;
		bloomDesc.mipLevels = bloomLevels;
		bloomDesc.format = c_HDRFormat;
		bloomDesc.isUAV = true;
		bloomDesc.debugName = name + ".Bloom";
		bloomDesc.initialState = nvrhi::ResourceStates::ShaderResource;
		bloomDesc.keepInitialState = true;
		m_BloomTexture = m_Device->createTexture(bloomDesc);
		if (!m_ExposureBindingSet || !m_BloomTexture)
			return fail("post-processing targets");
		m_BloomLevels = bloomLevels;

		auto createBloomSet = [&](nvrhi::ITexture* source, uint32_t sourceLevel, uint32_t destinationLevel)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::Texture_SRV(0, source, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(sourceLevel, 1, 0, 1)),
				nvrhi::BindingSetItem::Sampler(0, m_LinearClampSampler),
				nvrhi::BindingSetItem::Texture_UAV(0, m_BloomTexture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(destinationLevel, 1, 0, 1)),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_ExposureBuffer),
				nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::BloomParameters))
			};
			return m_Device->createBindingSet(desc, m_BloomBindingLayout);
		};
		m_BloomDownsampleSets.push_back(createBloomSet(m_HDRTexture, 0, 0));
		for (uint32_t level = 1; level < m_BloomLevels; level++)
			m_BloomDownsampleSets.push_back(createBloomSet(m_BloomTexture, level - 1, level));
		for (uint32_t level = 0; level + 1 < m_BloomLevels; level++)
			m_BloomUpsampleSets.push_back(createBloomSet(m_BloomTexture, level + 1, level));
		for (const std::vector<nvrhi::BindingSetHandle>* sets : { &m_BloomDownsampleSets, &m_BloomUpsampleSets })
		{
			for (const nvrhi::BindingSetHandle& set : *sets)
			{
				if (!set)
					return fail("bloom bindings");
			}
		}

		nvrhi::BindingSetDesc tonemapSet;
		tonemapSet.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, m_HDRTexture),
			nvrhi::BindingSetItem::Sampler(0, m_PointClampSampler),
			nvrhi::BindingSetItem::Texture_SRV(1, m_BloomTexture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
			nvrhi::BindingSetItem::Sampler(1, m_LinearClampSampler),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_ExposureBuffer),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::TonemapParameters))
		};
		m_TonemapBindingSet = m_Device->createBindingSet(tonemapSet, m_TonemapBindingLayout);
		nvrhi::BindingSetDesc fxaaSet;
		fxaaSet.bindings = { nvrhi::BindingSetItem::Texture_SRV(0, m_LDRTexture), nvrhi::BindingSetItem::Sampler(0, m_LinearClampSampler) };
		m_FXAABindingSet = m_Device->createBindingSet(fxaaSet, m_FXAABindingLayout);
		if (!m_TonemapBindingSet || !m_FXAABindingSet)
			return fail("post-processing bindings");

		// Overlays are drawn into the output texture, tested against the scene depth (which they never change).
		m_OverlayFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
			.addColorAttachment(m_OutputTexture)
			.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(m_DepthTexture).setReadOnly(true)));
		nvrhi::BindingSetDesc copySet;
		copySet.bindings = { nvrhi::BindingSetItem::Texture_SRV(0, m_OutputTexture), nvrhi::BindingSetItem::Sampler(0, m_PointClampSampler) };
		m_CopyBindingSet = m_Device->createBindingSet(copySet, m_FXAABindingLayout);
		if (!m_OverlayFramebuffer || !m_CopyBindingSet)
			return fail("overlay targets");
		return true;
	}

	nvrhi::IGraphicsPipeline* SceneRenderer::GetScenePipeline(const PipelineKey& key)
	{
		for (const auto& [cachedKey, pipeline] : m_ScenePipelines)
		{
			if (cachedKey == key)
				return pipeline;
		}

		const bool shadowPass = key.PassType == Pass::Shadow || key.PassType == Pass::ShadowMasked;
		nvrhi::GraphicsPipelineDesc desc;
		desc.primType = nvrhi::PrimitiveType::TriangleList;
		desc.inputLayout = m_InputLayout;
		desc.VS = shadowPass ? m_ShadowVertexShader : m_MeshVertexShader;
		switch (key.PassType)
		{
			case Pass::Prepass:      desc.PS = m_PrepassPixelShader; break;
			case Pass::ShadowMasked: desc.PS = m_ShadowMaskPixelShader; break;
			case Pass::Shadow:       desc.PS = nullptr; break; // Depth only
			case Pass::Transparent:  desc.PS = m_ForwardTransparentPixelShader; break;
			case Pass::Opaque:       desc.PS = m_ForwardPixelShader; break;
		}
		desc.bindingLayouts = { m_SceneBindingLayout, Renderer::GetBindlessTextures().GetLayout() };
		if (shadowPass)
			desc.bindingLayouts.push_back(m_ShadowPushLayout);
		// Front faces are counter-clockwise; a mirroring transform reverses the winding on screen.
		desc.renderState.rasterState.setFrontCounterClockwise(!key.Mirrored);
		if (key.DoubleSided)
			desc.renderState.rasterState.setCullNone();
		else
			desc.renderState.rasterState.setCullBack();

		// Reversed-Z: nearer surfaces have larger depth values.
		nvrhi::DepthStencilState& depth = desc.renderState.depthStencilState;
		depth.setDepthTestEnable(true);
		switch (key.PassType)
		{
			case Pass::Prepass:
				depth.setDepthWriteEnable(true).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
				break;
			case Pass::Shadow:
			case Pass::ShadowMasked:
				depth.setDepthWriteEnable(true).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
				// Push stored depth away from the light against shadow acne. NVRHI enables the depth bias only with a
				// non-zero constant part, which also keeps the slope part working.
				desc.renderState.rasterState.setDepthBias(c_ShadowDepthBias).setSlopeScaleDepthBias(c_ShadowSlopeDepthBias);
				break;
			case Pass::Opaque:
				depth.setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::Equal);
				break;
			case Pass::Transparent:
			{
				depth.setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
				nvrhi::BlendState::RenderTarget blend;
				blend.setBlendEnable(true)
					.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
					.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
					.setSrcBlendAlpha(nvrhi::BlendFactor::One)
					.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
				desc.renderState.blendState.setRenderTarget(0, blend);
				break;
			}
		}

		nvrhi::IFramebuffer* framebuffer = shadowPass ? m_ShadowFramebuffers[0].Get()
			: (key.PassType == Pass::Prepass ? m_PrepassFramebuffer.Get() : m_ForwardFramebuffer.Get());
		nvrhi::GraphicsPipelineHandle pipeline = m_Device->createGraphicsPipeline(desc, framebuffer->getFramebufferInfo());
		ST_CORE_VERIFY(pipeline, "SceneRenderer: failed to create a scene pipeline");
		m_ScenePipelines.emplace_back(key, pipeline);
		return pipeline;
	}

	nvrhi::IGraphicsPipeline* SceneRenderer::GetFullscreenPipeline(std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>>& cache,
		nvrhi::IShader* pixelShader, nvrhi::IBindingLayout* layout, const nvrhi::FramebufferInfo& framebufferInfo)
	{
		for (const auto& [info, pipeline] : cache)
		{
			if (info == framebufferInfo)
				return pipeline;
		}

		nvrhi::GraphicsPipelineDesc desc;
		desc.primType = nvrhi::PrimitiveType::TriangleList;
		desc.VS = m_FullscreenVertexShader;
		desc.PS = pixelShader;
		desc.bindingLayouts = { layout };
		desc.renderState.rasterState.setCullNone();
		desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
		nvrhi::GraphicsPipelineHandle pipeline = m_Device->createGraphicsPipeline(desc, framebufferInfo);
		ST_CORE_VERIFY(pipeline, "SceneRenderer: failed to create a fullscreen pipeline");
		cache.emplace_back(framebufferInfo, pipeline);
		return pipeline;
	}

	void SceneRenderer::CountPendingAsset(AssetHandle handle)
	{
		// Only registered assets that are still on their way count; unknown and failed ones never arrive.
		const AssetState state = AssetManager::GetAssetState(handle);
		if (AssetManager::IsHandleValid(handle) && state != AssetState::Failed && state != AssetState::Ready)
			m_Stats.PendingAssets++;
	}

	uint32_t SceneRenderer::ResolveTexture(AssetHandle handle, uint32_t missingSlot, uint32_t loadingSlot)
	{
		if (!handle.IsValid())
			return PackTextureSlot(missingSlot, 0);

		Ref<Texture> texture = AssetManager::GetAsset<Texture>(handle);
		if (!texture || texture->GetBindlessSlot() == BindlessTextureTable::c_InvalidSlot)
		{
			CountPendingAsset(handle);
			return PackTextureSlot(loadingSlot, 0);
		}
		return PackTextureSlot(texture->GetBindlessSlot(), GetSamplerIndex(texture->GetSpecification()));
	}

	uint32_t SceneRenderer::GetMaterialIndex(const Ref<Material>& material)
	{
		auto it = m_MaterialIndices.find(material.get());
		if (it != m_MaterialIndices.end())
			return it->second;

		const MaterialProperties& properties = material->GetProperties();
		RenderData::MaterialData data = {};
		data.BaseColor = properties.BaseColor;
		data.Emissive = glm::vec4(properties.EmissiveColor * properties.EmissiveIntensity, 0.0f);
		data.Metallic = properties.Metallic;
		data.Roughness = properties.Roughness;
		data.NormalScale = properties.NormalScale;
		data.OcclusionStrength = properties.OcclusionStrength;
		data.AlphaCutoff = properties.AlphaCutoff;
		data.Flags = (properties.AlphaMode == MaterialAlphaMode::Mask ? RenderData::c_MaterialAlphaMask : 0u)
			| (properties.AlphaMode == MaterialAlphaMode::Blend ? RenderData::c_MaterialAlphaBlend : 0u)
			| (properties.Unlit ? RenderData::c_MaterialUnlit : 0u)
			| (properties.DoubleSided ? RenderData::c_MaterialDoubleSided : 0u);
		data.UVTransform = glm::vec4(properties.UVTiling, properties.UVOffset);

		// Missing maps use neutral textures; maps still loading use neutral ones too, except emission (black until the
		// map arrives, so surfaces do not flash at full emissive strength).
		data.BaseColorMap = ResolveTexture(properties.BaseColorMap, BindlessTextureTable::c_WhiteSlot, BindlessTextureTable::c_WhiteSlot);
		data.MetallicRoughnessMap = ResolveTexture(properties.MetallicRoughnessMap, BindlessTextureTable::c_WhiteSlot, BindlessTextureTable::c_WhiteSlot);
		data.NormalMap = ResolveTexture(properties.NormalMap, BindlessTextureTable::c_FlatNormalSlot, BindlessTextureTable::c_FlatNormalSlot);
		data.OcclusionMap = ResolveTexture(properties.OcclusionMap, BindlessTextureTable::c_WhiteSlot, BindlessTextureTable::c_WhiteSlot);
		data.EmissiveMap = ResolveTexture(properties.EmissiveMap, BindlessTextureTable::c_WhiteSlot, BindlessTextureTable::c_BlackSlot);

		const uint32_t index = static_cast<uint32_t>(m_Materials.size());
		m_Materials.push_back(data);
		m_MaterialIndices.emplace(material.get(), index);
		m_FrameMaterials.push_back(material);
		return index;
	}

	void SceneRenderer::CollectLights(Scene& scene, const SceneCamera& camera, RenderData::FrameConstants& frame)
	{
		entt::registry& registry = scene.GetRegistry();

		frame.DirectionalLightDirection = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
		frame.DirectionalLightColor = glm::vec4(0.0f);
		m_ShadowLight = {};
		float brightest = -1.0f;
		for (auto [entity, light, world] : registry.view<DirectionalLightComponent, WorldTransformComponent>().each())
		{
			if (!world.ActiveInHierarchy || light.Intensity <= brightest)
				continue;
			// Lights shine along their entity's -Z axis.
			const glm::vec3 direction = glm::vec3(world.Matrix * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
			const float length = glm::length(direction);
			if (!(length > 1e-6f))
				continue;
			brightest = light.Intensity;
			frame.DirectionalLightDirection = glm::vec4(direction / length, 1.0f);
			frame.DirectionalLightColor = glm::vec4(light.Color * light.Intensity, 1.0f);
			m_ShadowLight.Enabled = light.CastShadows && light.Intensity > 0.0f;
			m_ShadowLight.Direction = direction / length;
			m_ShadowLight.Distance = light.ShadowDistance;
			m_ShadowLight.Softness = light.ShadowSoftness;
			m_ShadowLight.Bias = light.ShadowBias;
			m_ShadowLight.NormalBias = light.ShadowNormalBias;
		}

		frame.AmbientColor = glm::vec4(0.03f, 0.03f, 0.04f, 0.0f);
		for (auto [entity, sky, world] : registry.view<SkyLightComponent, WorldTransformComponent>().each())
		{
			if (!world.ActiveInHierarchy)
				continue;
			frame.AmbientColor = glm::vec4(sky.AmbientColor * sky.Intensity, 0.0f);
			break;
		}

		// Point and spot lights: frustum culled by the bounding sphere of the region they reach, then ranked by
		// relevance (lights containing the camera first, then by the projected size of that sphere).
		const Frustum frustum(camera.Projection * camera.View);
		m_LightCandidates.clear();
		auto addLight = [&](const glm::mat4& matrix, const glm::vec3& color, float intensity, float range, uint32_t type, float innerDegrees, float outerDegrees)
		{
			if (!(intensity > 0.0f) || !(range > 0.0f) || !std::isfinite(intensity) || !std::isfinite(range) || !IsFinite(matrix))
				return;
			const glm::vec3 position = glm::vec3(matrix[3]);
			glm::vec3 direction = glm::vec3(matrix * glm::vec4(0.0f, 0.0f, -1.0f, 0.0f));
			const float length = glm::length(direction);
			direction = length > 1e-6f ? direction / length : glm::vec3(0.0f, 0.0f, -1.0f);
			const float cosInner = std::cos(glm::radians(innerDegrees));
			const float cosOuter = std::cos(glm::radians(outerDegrees));

			LightCandidate candidate;
			candidate.Bounds = type == RenderData::c_LightSpot ? SpotLightBounds(position, direction, range, cosOuter, std::sin(glm::radians(outerDegrees)))
				: glm::vec4(position, range);
			if (!frustum.IsSphereVisible(glm::vec3(candidate.Bounds), candidate.Bounds.w))
			{
				m_Stats.CulledLights++;
				return;
			}

			candidate.Data.PositionRange = glm::vec4(position, range);
			candidate.Data.Color = glm::vec4(color * intensity, 1.0f / (range * range));
			candidate.Data.DirectionType = glm::vec4(direction, static_cast<float>(type));
			candidate.Data.SpotAngles = glm::vec4(cosInner, cosOuter, 0.0f, 0.0f);
			const float distance = glm::length(glm::vec3(candidate.Bounds) - camera.Position);
			if (camera.Orthographic)
				candidate.Relevance = candidate.Bounds.w; // The projected size does not depend on the distance
			else if (distance <= candidate.Bounds.w)
				candidate.Relevance = std::numeric_limits<float>::max();
			else
				candidate.Relevance = candidate.Bounds.w / (distance - candidate.Bounds.w);
			candidate.Order = static_cast<uint32_t>(m_LightCandidates.size());
			m_LightCandidates.push_back(candidate);
		};

		for (auto [entity, light, world] : registry.view<PointLightComponent, WorldTransformComponent>().each())
		{
			if (world.ActiveInHierarchy)
				addLight(world.Matrix, light.Color, light.Intensity, light.Range, RenderData::c_LightPoint, 0.0f, 0.0f);
		}
		for (auto [entity, light, world] : registry.view<SpotLightComponent, WorldTransformComponent>().each())
		{
			if (!world.ActiveInHierarchy)
				continue;
			const float outer = std::clamp(light.OuterConeAngle, 0.1f, 89.9f);
			const float inner = std::clamp(light.InnerConeAngle, 0.0f, outer);
			addLight(world.Matrix, light.Color, light.Intensity, light.Range, RenderData::c_LightSpot, inner, outer);
		}

		// The order also decides which lights a full cluster keeps.
		std::sort(m_LightCandidates.begin(), m_LightCandidates.end(), [](const LightCandidate& a, const LightCandidate& b)
		{
			return a.Relevance != b.Relevance ? a.Relevance > b.Relevance : a.Order < b.Order;
		});
		if (m_LightCandidates.size() > m_Specification.MaxLights)
		{
			m_Stats.DroppedLights = static_cast<uint32_t>(m_LightCandidates.size() - m_Specification.MaxLights);
			m_LightCandidates.resize(m_Specification.MaxLights);
			if (!m_WarnedLightOverflow)
			{
				ST_CORE_WARN("SceneRenderer '{}': more than {} point and spot lights are visible; the least relevant ones are not rendered", m_Specification.DebugName,
					m_Specification.MaxLights);
				m_WarnedLightOverflow = true;
			}
		}

		m_Lights.clear();
		m_LightBounds.clear();
		for (const LightCandidate& candidate : m_LightCandidates)
		{
			m_Lights.push_back(candidate.Data);
			const glm::vec3 viewCenter = glm::vec3(camera.View * glm::vec4(glm::vec3(candidate.Bounds), 1.0f));
			m_LightBounds.push_back(RenderData::LightBounds { glm::vec4(viewCenter, candidate.Bounds.w) });
		}
		frame.LightCounts = glm::uvec4(static_cast<uint32_t>(m_Lights.size()), 0, 0, 0);

		// Exponential depth slices between the near and far distance; the first slice also holds everything closer, the
		// last everything further away.
		const glm::uvec3 grid = m_Specification.LightClusterGrid;
		const float sliceNear = std::max(camera.Near, 0.01f);
		const float sliceFar = std::isfinite(camera.Far) ? std::clamp(camera.Far, sliceNear * 2.0f, 1.0e6f) : 1.0e6f;
		const float scale = static_cast<float>(grid.z) / std::log(sliceFar / sliceNear);
		frame.ClusterGrid = glm::uvec4(grid, m_Specification.MaxLightsPerCluster);
		frame.ClusterDepth = glm::vec4(scale, -scale * std::log(sliceNear), std::min(camera.Near, 0.0f), c_ClusterDepthEnd);
	}

	void SceneRenderer::CollectDraws(Scene& scene, const SceneCamera& camera)
	{
		const Frustum frustum(camera.Projection * camera.View);
		m_DrawRecords.clear();
		m_InstancePayloads.clear();

		Ref<Material> defaultMaterial = AssetManager::GetAsset<Material>(BuiltinAssets::DefaultMaterial);
		entt::registry& registry = scene.GetRegistry();
		for (auto [entity, meshRenderer, world] : registry.view<MeshRendererComponent, WorldTransformComponent>().each())
		{
			if (!world.ActiveInHierarchy || !meshRenderer.Mesh.IsValid())
				continue;

			Ref<Mesh> mesh = AssetManager::GetAsset<Mesh>(meshRenderer.Mesh, AssetPriority::High);
			if (!mesh || !mesh->GetIndexBuffer())
			{
				CountPendingAsset(meshRenderer.Mesh);
				continue;
			}

			const glm::mat4& transform = world.Matrix;
			const float determinant = glm::determinant(glm::mat3(transform));
			if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f || !IsFinite(world.Matrix))
				continue; // Zero scale: nothing to see
			const AABB bounds = mesh->GetBounds().Transformed(transform);
			if (!frustum.IsAABBVisible(bounds))
			{
				m_Stats.CulledInstances++;
				continue;
			}

			// Projected size of the bounding sphere as a fraction of the viewport height selects the level of detail.
			const float screenFraction = GetScreenFraction(camera, bounds.GetCenter(), glm::length(bounds.GetExtents()));
			const float viewDepth = -(camera.View * glm::vec4(bounds.GetCenter(), 1.0f)).z;
			const bool mirrored = determinant < 0.0f;

			RenderData::InstanceData payload = {};
			payload.World = transform;
			payload.NormalMatrix = glm::transpose(glm::inverse(transform));
			payload.EntityID = static_cast<uint32_t>(entity) + 1;
			payload.Flags = mirrored ? RenderData::c_InstanceMirrored : 0u;

			const std::vector<Submesh>& submeshes = mesh->GetSubmeshes();
			for (uint32_t submeshIndex = 0; submeshIndex < submeshes.size(); submeshIndex++)
			{
				const Submesh& submesh = submeshes[submeshIndex];
				const AssetHandle materialHandle = meshRenderer.Material.IsValid() ? meshRenderer.Material
					: (submesh.Material.IsValid() ? submesh.Material : BuiltinAssets::DefaultMaterial);
				Ref<Material> material = AssetManager::GetAsset<Material>(materialHandle, AssetPriority::High);
				if (!material)
				{
					CountPendingAsset(materialHandle);
					material = defaultMaterial;
				}
				if (!material)
					continue;

				const MaterialProperties& properties = material->GetProperties();
				payload.MaterialIndex = GetMaterialIndex(material);
				DrawRecord record;
				record.MeshAsset = mesh.get();
				record.SubmeshIndex = submeshIndex;
				record.LOD = SelectLOD(submesh, screenFraction);
				record.MaterialIndex = payload.MaterialIndex;
				record.PayloadIndex = static_cast<uint32_t>(m_InstancePayloads.size());
				record.ViewDepth = viewDepth;
				record.DoubleSided = properties.DoubleSided;
				record.Mirrored = mirrored;
				record.Transparent = properties.AlphaMode == MaterialAlphaMode::Blend;
				m_InstancePayloads.push_back(payload);
				m_DrawRecords.push_back(record);
				m_Stats.InstancesPerLOD[std::min<size_t>(record.LOD, SceneRendererStats::c_LODStatCount - 1)]++;
			}
			m_FrameMeshes.push_back(std::move(mesh));
		}

		BuildBatches(m_DrawRecords, m_InstancePayloads, m_OpaqueBatches, &m_TransparentBatches, m_Instances, false);
	}

	void SceneRenderer::DrawBatches(nvrhi::ICommandList* commandList, Pass pass, bool transparent)
	{
		const std::vector<DrawBatch>& batches = transparent ? m_TransparentBatches : m_OpaqueBatches;
		if (batches.empty())
			return;

		nvrhi::GraphicsState state;
		state.framebuffer = pass == Pass::Prepass ? m_PrepassFramebuffer.Get() : m_ForwardFramebuffer.Get();
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y)));
		state.bindings = { m_SceneBindingSet, Renderer::GetBindlessTextures().GetTable() };

		for (const DrawBatch& batch : batches)
		{
			const Submesh& submesh = batch.MeshAsset->GetSubmeshes()[batch.SubmeshIndex];
			const MeshLOD& lod = submesh.LODs[batch.LOD];
			state.pipeline = GetScenePipeline(PipelineKey { pass, batch.DoubleSided, batch.Mirrored });
			state.vertexBuffers = {
				nvrhi::VertexBufferBinding { batch.MeshAsset->GetPositionBuffer(), 0, 0 },
				nvrhi::VertexBufferBinding { batch.MeshAsset->GetAttributeBuffer(), 1, 0 }
			};
			state.indexBuffer = nvrhi::IndexBufferBinding { batch.MeshAsset->GetIndexBuffer(), nvrhi::Format::R32_UINT, 0 };
			commandList->setGraphicsState(state);

			nvrhi::DrawArguments arguments;
			arguments.vertexCount = lod.IndexCount;
			arguments.instanceCount = batch.InstanceCount;
			arguments.startIndexLocation = lod.IndexOffset;
			arguments.startVertexLocation = submesh.BaseVertex;
			arguments.startInstanceLocation = batch.FirstInstance;
			commandList->drawIndexed(arguments);

			if (pass != Pass::Prepass)
			{
				m_Stats.DrawCalls++;
				m_Stats.Triangles += static_cast<uint64_t>(lod.IndexCount / 3) * batch.InstanceCount;
			}
		}
	}

	void SceneRenderer::BuildLightClusters(nvrhi::ICommandList* commandList)
	{
		const glm::uvec3 grid = m_Specification.LightClusterGrid;
		const uint32_t clusterCount = grid.x * grid.y * grid.z;
		nvrhi::ComputeState state;
		state.pipeline = m_ClusterPipeline;
		state.bindings = { m_ClusterBindingSet };
		commandList->setComputeState(state);
		commandList->dispatch((clusterCount + c_ClusterThreadGroupSize - 1) / c_ClusterThreadGroupSize, 1, 1);
	}

	bool SceneRenderer::ValidateTarget(nvrhi::IFramebuffer* target)
	{
		const nvrhi::FramebufferDesc& desc = target->getDesc();
		const nvrhi::FramebufferInfoEx& info = target->getFramebufferInfo();
		if (desc.colorAttachments.empty() || !desc.colorAttachments[0].texture)
		{
			ReportError("the target framebuffer has no color attachment");
			return false;
		}
		const nvrhi::FormatInfo& format = nvrhi::getFormatInfo(info.colorFormats[0]);
		if (format.kind != nvrhi::FormatKind::Normalized || format.isSigned || format.isSRGB || info.sampleCount != 1)
		{
			ReportError(fmt::format("the target's color format {} is not supported (single-sampled, non-sRGB UNORM required)", format.name));
			return false;
		}
		if (info.width != m_ViewportSize.x || info.height != m_ViewportSize.y)
		{
			ReportError(fmt::format("the target is {}x{} but the viewport is {}x{}; the renderer does not rescale", info.width, info.height, m_ViewportSize.x,
				m_ViewportSize.y));
			return false;
		}
		return true;
	}

	bool SceneRenderer::Render(Scene& scene, const SceneCamera& camera, nvrhi::IFramebuffer* target, const SceneRenderOptions& options)
	{
		ST_PROFILE_FUNCTION();
		m_Stats = {};
		if (m_ViewportSize.x == 0 || m_ViewportSize.y == 0)
			return false; // Nothing to show (e.g. a minimized window); not an error
		if (!m_TargetsValid)
		{
			ReportError("the render targets are unavailable (see the previous error)");
			return false;
		}
		if (target && !ValidateTarget(target))
			return false;

		scene.UpdateWorldTransforms();

		m_Materials.clear();
		m_MaterialIndices.clear();
		m_FrameMaterials.clear();
		m_FrameMeshes.clear();

		RenderData::FrameConstants frame = {};
		frame.View = camera.View;
		frame.Projection = camera.Projection;
		frame.ViewProjection = camera.Projection * camera.View;
		frame.InverseView = glm::inverse(camera.View);
		frame.InverseProjection = glm::inverse(camera.Projection);
		frame.InverseViewProjection = glm::inverse(frame.ViewProjection);
		frame.CameraPosition = glm::vec4(camera.Position, camera.Near);
		const glm::vec3 forward = -glm::vec3(frame.InverseView[2]);
		frame.CameraForward = glm::vec4(glm::length(forward) > 1e-6f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f), camera.Orthographic ? 1.0f : 0.0f);
		frame.ViewportSize = glm::vec4(glm::vec2(m_ViewportSize), 1.0f / glm::vec2(m_ViewportSize));
		frame.TimeParams = glm::vec4(static_cast<float>(scene.GetTime()), 0.0f, 0.0f, 0.0f);
		CollectLights(scene, camera, frame);
		CollectDraws(scene, camera);
		CollectShadowCasters(scene, camera, frame);
		bool degraded = false; // Rendered with a reported limitation: keep the error so it is not logged every frame
		if (frame.ShadowParams.w > 0.0f && !EnsureShadowMap())
		{
			ReportError("failed to create the shadow map; rendering without shadows");
			frame.ShadowParams.w = 0.0f;
			degraded = true;
		}
		const PostProcessComponent postProcess = GetPostProcessSettings(scene);
		frame.AOParams = glm::vec4(postProcess.AmbientOcclusion ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

		bool buffersRecreated = !m_SceneBindingSet || m_BoundShadowMap != (m_ShadowMap ? m_ShadowMap.Get() : m_DummyShadowMap.Get());
		const bool buffersReady = EnsureBufferCapacity(m_InstanceBuffer, m_Instances.size(), sizeof(RenderData::InstanceData), "Instances", buffersRecreated)
			&& EnsureBufferCapacity(m_ShadowInstanceBuffer, m_ShadowInstances.size(), sizeof(RenderData::InstanceData), "ShadowInstances", buffersRecreated)
			&& EnsureBufferCapacity(m_MaterialBuffer, m_Materials.size(), sizeof(RenderData::MaterialData), "Materials", buffersRecreated)
			&& EnsureBufferCapacity(m_LightBuffer, m_Lights.size(), sizeof(RenderData::LightData), "Lights", buffersRecreated)
			&& EnsureBufferCapacity(m_LightBoundsBuffer, m_LightBounds.size(), sizeof(RenderData::LightBounds), "LightBounds", buffersRecreated);
		if (!buffersReady)
		{
			m_Stats = {};
			return false;
		}

		m_Stats.Instances = static_cast<uint32_t>(m_Instances.size());
		m_Stats.Materials = static_cast<uint32_t>(m_Materials.size());
		m_Stats.Lights = static_cast<uint32_t>(m_Lights.size()) + (frame.DirectionalLightDirection.w > 0.0f ? 1u : 0u);

		nvrhi::ICommandList* commandList = m_CommandList;
		commandList->open();
		commandList->beginMarker(m_Specification.DebugName.c_str());
		UpdateEnvironment(scene, frame, commandList);
		if ((buffersRecreated || m_BoundEnvironment != m_Environment.Cube.Get()) && !CreateSceneBindingSets())
		{
			commandList->endMarker();
			commandList->close();
			m_Device->executeCommandList(commandList); // Keeps the environment processing that was recorded
			ReportError("failed to create the scene binding sets");
			m_Stats = {};
			return false;
		}

		commandList->writeBuffer(m_FrameConstantBuffer, &frame, sizeof(frame));
		if (!m_Instances.empty())
			commandList->writeBuffer(m_InstanceBuffer, m_Instances.data(), m_Instances.size() * sizeof(RenderData::InstanceData));
		if (!m_Materials.empty())
			commandList->writeBuffer(m_MaterialBuffer, m_Materials.data(), m_Materials.size() * sizeof(RenderData::MaterialData));
		if (!m_Lights.empty())
		{
			commandList->writeBuffer(m_LightBuffer, m_Lights.data(), m_Lights.size() * sizeof(RenderData::LightData));
			commandList->writeBuffer(m_LightBoundsBuffer, m_LightBounds.data(), m_LightBounds.size() * sizeof(RenderData::LightBounds));
		}
		if (!m_ShadowInstances.empty())
			commandList->writeBuffer(m_ShadowInstanceBuffer, m_ShadowInstances.data(), m_ShadowInstances.size() * sizeof(RenderData::InstanceData));

		if (!m_Lights.empty())
		{
			commandList->beginMarker("LightClusters");
			BuildLightClusters(commandList);
			commandList->endMarker();
		}

		if (frame.ShadowParams.w > 0.0f)
		{
			commandList->beginMarker("Shadows");
			DrawShadows(commandList, static_cast<uint32_t>(frame.ShadowParams.w));
			commandList->endMarker();
		}

		const glm::vec4 clear = camera.ClearColor;
		commandList->clearDepthStencilTexture(m_DepthTexture, nvrhi::AllSubresources, true, 0.0f, false, 0);
		commandList->clearTextureFloat(m_NormalTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		commandList->clearTextureUInt(m_EntityIDTexture, nvrhi::AllSubresources, 0);
		commandList->clearTextureFloat(m_HDRTexture, nvrhi::AllSubresources, nvrhi::Color(SRGBToLinear(clear.r), SRGBToLinear(clear.g), SRGBToLinear(clear.b), 1.0f));

		commandList->beginMarker("Prepass");
		DrawBatches(commandList, Pass::Prepass, false);
		commandList->endMarker();

		if (postProcess.AmbientOcclusion)
		{
			commandList->beginMarker("AmbientOcclusion");
			RenderAmbientOcclusion(commandList, postProcess);
			commandList->endMarker();
		}
		commandList->beginMarker("Opaque");
		DrawBatches(commandList, Pass::Opaque, false);
		commandList->endMarker();

		if (frame.EnvironmentParams.w > 0.0f && frame.SkyParams.y > 0.0f)
		{
			if (!m_SkyboxPipeline)
			{
				nvrhi::GraphicsPipelineDesc desc;
				desc.primType = nvrhi::PrimitiveType::TriangleList;
				desc.VS = m_FullscreenVertexShader;
				desc.PS = m_SkyboxPixelShader;
				desc.bindingLayouts = { m_SceneBindingLayout, Renderer::GetBindlessTextures().GetLayout() };
				desc.renderState.rasterState.setCullNone();
				// The fullscreen triangle lies at depth 0 (the far plane with reversed-Z): it covers only empty pixels.
				desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
				m_SkyboxPipeline = m_Device->createGraphicsPipeline(desc, m_ForwardFramebuffer->getFramebufferInfo());
				ST_CORE_VERIFY(m_SkyboxPipeline, "SceneRenderer: failed to create the skybox pipeline");
			}
			nvrhi::GraphicsState skyState;
			skyState.pipeline = m_SkyboxPipeline;
			skyState.framebuffer = m_ForwardFramebuffer;
			skyState.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y)));
			skyState.bindings = { m_SceneBindingSet, Renderer::GetBindlessTextures().GetTable() };
			commandList->beginMarker("Sky");
			commandList->setGraphicsState(skyState);
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
			commandList->endMarker();
		}
		commandList->beginMarker("Transparent");
		DrawBatches(commandList, Pass::Transparent, true);
		commandList->endMarker();

		commandList->beginMarker("Exposure");
		UpdateExposure(commandList, postProcess);
		commandList->endMarker();
		const bool bloom = postProcess.Bloom && postProcess.BloomIntensity > 0.0f;
		if (bloom)
		{
			commandList->beginMarker("Bloom");
			RenderBloom(commandList, postProcess);
			commandList->endMarker();
		}

		// Tone mapping into the display target, or into the intermediate target FXAA reads.
		RenderData::TonemapParameters tonemap = {};
		tonemap.BloomIntensity = bloom ? std::min(postProcess.BloomIntensity, 1.0f) : 0.0f;
		tonemap.BloomScale = 1.0f / static_cast<float>(m_BloomLevels);
		tonemap.Saturation = std::max(postProcess.Saturation, 0.0f);
		tonemap.Vignette = std::clamp(postProcess.Vignette, 0.0f, 1.0f);
		tonemap.Contrast = std::clamp(postProcess.Contrast, 0.1f, 4.0f);
		tonemap.Operator = static_cast<int32_t>(postProcess.Tonemapper);
		tonemap.BloomAdditive = postProcess.BloomThreshold > 0.0f ? 1u : 0u;

		// Overlays are composed in the output texture, which is then copied into the target.
		const bool overlays = HasOverlays(options);
		nvrhi::IFramebuffer* output = target && !overlays ? target : m_OutputFramebuffer.Get();
		auto drawFullscreen = [&](nvrhi::IGraphicsPipeline* pipeline, nvrhi::IFramebuffer* framebuffer, nvrhi::IBindingSet* bindingSet, const void* pushConstants,
			size_t pushConstantSize)
		{
			nvrhi::GraphicsState state;
			state.pipeline = pipeline;
			state.framebuffer = framebuffer;
			state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y)));
			state.bindings = { bindingSet };
			commandList->setGraphicsState(state);
			if (pushConstants)
				commandList->setPushConstants(pushConstants, pushConstantSize);
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		};

		nvrhi::IFramebuffer* tonemapTarget = postProcess.AntiAliasing ? m_LDRFramebuffer.Get() : output;
		commandList->beginMarker("Tonemap");
		drawFullscreen(GetFullscreenPipeline(m_TonemapPipelines, m_TonemapPixelShader, m_TonemapBindingLayout, tonemapTarget->getFramebufferInfo()),
			tonemapTarget, m_TonemapBindingSet, &tonemap, sizeof(tonemap));
		commandList->endMarker();
		if (postProcess.AntiAliasing)
		{
			commandList->beginMarker("FXAA");
			drawFullscreen(GetFullscreenPipeline(m_FXAAPipelines, m_FXAAPixelShader, m_FXAABindingLayout, output->getFramebufferInfo()), output, m_FXAABindingSet, nullptr, 0);
			commandList->endMarker();
		}

		if (overlays)
		{
			commandList->beginMarker("Overlays");
			const bool overlaysDrawn = RenderOverlays(commandList, scene, options);
			commandList->endMarker();
			if (!overlaysDrawn)
				degraded = true; // The image is complete without them; RenderOverlays reported why
			if (target)
			{
				commandList->beginMarker("Copy");
				drawFullscreen(GetFullscreenPipeline(m_CopyPipelines, m_CopyPixelShader, m_FXAABindingLayout, target->getFramebufferInfo()), target, m_CopyBindingSet, nullptr, 0);
				commandList->endMarker();
			}
		}

		commandList->endMarker();
		commandList->close();
		m_Device->executeCommandList(commandList);

		m_Stats.Rendered = true;
		m_HasRenderedFrame = true;
		if (!degraded)
			m_LastError.clear();
		return true;
	}

	void SceneRenderer::CreateOverlayResources()
	{
		ShaderLibrary& shaders = Renderer::GetShaderLibrary();
		m_GridPixelShader = shaders.Get("Overlay/Grid.frag");
		m_OutlinePixelShader = shaders.Get("Overlay/Outline.frag");
		m_DebugLineVertexShader = shaders.Get("Overlay/DebugLine.vert");
		m_DebugLinePixelShader = shaders.Get("Overlay/DebugLine.frag");
		m_CopyPixelShader = shaders.Get("PostProcess/Copy.frag");
		ST_CORE_VERIFY(m_GridPixelShader && m_OutlinePixelShader && m_DebugLineVertexShader && m_DebugLinePixelShader && m_CopyPixelShader,
			"SceneRenderer overlay shaders are missing");

		// Slot 1: register b0 belongs to the frame constants.
		nvrhi::BindingLayoutDesc gridLayout;
		gridLayout.visibility = nvrhi::ShaderType::Pixel;
		gridLayout.bindings = { nvrhi::BindingLayoutItem::VolatileConstantBuffer(0), nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::GridParameters)) };
		m_GridBindingLayout = m_Device->createBindingLayout(gridLayout);
		nvrhi::BindingSetDesc gridSet;
		gridSet.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer), nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::GridParameters)) };
		m_GridBindingSet = m_Device->createBindingSet(gridSet, m_GridBindingLayout);

		nvrhi::BindingLayoutDesc outlineLayout;
		outlineLayout.visibility = nvrhi::ShaderType::Pixel;
		outlineLayout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::OutlineParameters))
		};
		m_OutlineBindingLayout = m_Device->createBindingLayout(outlineLayout);

		nvrhi::BindingLayoutDesc debugLineLayout;
		debugLineLayout.visibility = nvrhi::ShaderType::Vertex;
		debugLineLayout.bindings = { nvrhi::BindingLayoutItem::VolatileConstantBuffer(0) };
		m_DebugLineBindingLayout = m_Device->createBindingLayout(debugLineLayout);
		nvrhi::BindingSetDesc debugLineSet;
		debugLineSet.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer) };
		m_DebugLineBindingSet = m_Device->createBindingSet(debugLineSet, m_DebugLineBindingLayout);
		const nvrhi::VertexAttributeDesc lineAttributes[] = {
			nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RGB32_FLOAT).setBufferIndex(0)
				.setOffset(offsetof(DebugLineVertex, Position)).setElementStride(sizeof(DebugLineVertex)),
			nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA8_UNORM).setBufferIndex(0)
				.setOffset(offsetof(DebugLineVertex, Color)).setElementStride(sizeof(DebugLineVertex))
		};
		m_DebugLineInputLayout = m_Device->createInputLayout(lineAttributes, static_cast<uint32_t>(std::size(lineAttributes)), m_DebugLineVertexShader);

		bool recreated = false;
		const bool buffersCreated = EnsureBufferCapacity(m_SelectionBuffer, 64, sizeof(uint32_t), "Selection", recreated)
			&& EnsureBufferCapacity(m_DebugLineBuffer, 1024, sizeof(DebugLineVertex), "DebugLines", recreated, true);
		ST_CORE_VERIFY(buffersCreated && m_GridBindingLayout && m_GridBindingSet && m_OutlineBindingLayout && m_DebugLineBindingLayout && m_DebugLineBindingSet
			&& m_DebugLineInputLayout, "SceneRenderer: failed to create overlay resources");
	}

	bool SceneRenderer::HasOverlays(const SceneRenderOptions& options)
	{
		return options.ShowGrid || !options.SelectedEntities.empty() || (options.DebugShapes && !options.DebugShapes->IsEmpty());
	}

	void SceneRenderer::DrawDebugLines(nvrhi::ICommandList* commandList, DebugDrawDepth depth, uint32_t firstVertex, uint32_t vertexCount)
	{
		nvrhi::GraphicsState state;
		state.pipeline = m_DebugLinePipelines[static_cast<size_t>(depth)];
		state.framebuffer = m_OverlayFramebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y)));
		state.bindings = { m_DebugLineBindingSet };
		state.vertexBuffers = { nvrhi::VertexBufferBinding { m_DebugLineBuffer, 0, 0 } };
		commandList->setGraphicsState(state);
		commandList->draw(nvrhi::DrawArguments().setVertexCount(vertexCount).setStartVertexLocation(firstVertex));
	}

	bool SceneRenderer::RenderOverlays(nvrhi::ICommandList* commandList, Scene& scene, const SceneRenderOptions& options)
	{
		if (!m_GridPipeline)
		{
			// Blended over the image; depth-tested passes compare against the scene depth without writing it.
			nvrhi::BlendState::RenderTarget blend;
			blend.setBlendEnable(true)
				.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
				.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
				.setSrcBlendAlpha(nvrhi::BlendFactor::One)
				.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
			nvrhi::GraphicsPipelineDesc desc;
			desc.primType = nvrhi::PrimitiveType::TriangleList;
			desc.VS = m_FullscreenVertexShader;
			desc.PS = m_GridPixelShader;
			desc.bindingLayouts = { m_GridBindingLayout };
			desc.renderState.rasterState.setCullNone();
			desc.renderState.blendState.setRenderTarget(0, blend);
			desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
			const nvrhi::FramebufferInfo& info = m_OverlayFramebuffer->getFramebufferInfo();
			m_GridPipeline = m_Device->createGraphicsPipeline(desc, info);

			desc.PS = m_OutlinePixelShader;
			desc.bindingLayouts = { m_OutlineBindingLayout };
			desc.renderState.depthStencilState.setDepthTestEnable(false);
			m_OutlinePipeline = m_Device->createGraphicsPipeline(desc, info);

			desc.primType = nvrhi::PrimitiveType::LineList;
			desc.inputLayout = m_DebugLineInputLayout;
			desc.VS = m_DebugLineVertexShader;
			desc.PS = m_DebugLinePixelShader;
			desc.bindingLayouts = { m_DebugLineBindingLayout };
			m_DebugLinePipelines[static_cast<size_t>(DebugDrawDepth::OnTop)] = m_Device->createGraphicsPipeline(desc, info);
			desc.renderState.depthStencilState.setDepthTestEnable(true);
			m_DebugLinePipelines[static_cast<size_t>(DebugDrawDepth::Tested)] = m_Device->createGraphicsPipeline(desc, info);
			ST_CORE_VERIFY(m_GridPipeline && m_OutlinePipeline && m_DebugLinePipelines[0] && m_DebugLinePipelines[1], "SceneRenderer: failed to create the overlay pipelines");
		}
		const nvrhi::Viewport viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y));

		if (options.ShowGrid)
		{
			RenderData::GridParameters parameters = {};
			parameters.MinorColor = options.GridMinorColor;
			parameters.MajorColor = options.GridMajorColor;
			parameters.AxisXColor = options.GridAxisXColor;
			parameters.AxisZColor = options.GridAxisZColor;
			parameters.Spacing = std::max(options.GridSpacing, 1e-4f);
			parameters.MajorEvery = static_cast<float>(std::max(options.GridMajorEvery, 1u));
			parameters.FadeDistance = std::max(options.GridFadeDistance, 1e-3f);
			nvrhi::GraphicsState state;
			state.pipeline = m_GridPipeline;
			state.framebuffer = m_OverlayFramebuffer;
			state.viewport.addViewportAndScissorRect(viewport);
			state.bindings = { m_GridBindingSet };
			commandList->setGraphicsState(state);
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		}

		// Both depth modes share one upload: tested lines first.
		uint32_t testedVertices = 0;
		uint32_t onTopVertices = 0;
		if (options.DebugShapes && !options.DebugShapes->IsEmpty())
		{
			const std::span<const DebugLineVertex> tested = options.DebugShapes->GetLines(DebugDrawDepth::Tested);
			const std::span<const DebugLineVertex> onTop = options.DebugShapes->GetLines(DebugDrawDepth::OnTop);
			bool recreated = false;
			if (!EnsureBufferCapacity(m_DebugLineBuffer, tested.size() + onTop.size(), sizeof(DebugLineVertex), "DebugLines", recreated, true))
				return false;
			if (!tested.empty())
				commandList->writeBuffer(m_DebugLineBuffer, tested.data(), tested.size_bytes(), 0);
			if (!onTop.empty())
				commandList->writeBuffer(m_DebugLineBuffer, onTop.data(), onTop.size_bytes(), tested.size_bytes());
			testedVertices = static_cast<uint32_t>(tested.size());
			onTopVertices = static_cast<uint32_t>(onTop.size());
			m_Stats.DebugLines = (testedVertices + onTopVertices) / 2;
		}
		if (testedVertices > 0)
			DrawDebugLines(commandList, DebugDrawDepth::Tested, 0, testedVertices);

		// Selection outline from the entity-ID buffer (ids + 1, sorted for the shader's binary search).
		m_SelectionIDs.clear();
		for (const Entity& entity : options.SelectedEntities)
		{
			if (entity.GetScene() == &scene && entity.IsValid())
				m_SelectionIDs.push_back(static_cast<uint32_t>(entity.GetHandle()) + 1);
		}
		std::sort(m_SelectionIDs.begin(), m_SelectionIDs.end());
		m_SelectionIDs.erase(std::unique(m_SelectionIDs.begin(), m_SelectionIDs.end()), m_SelectionIDs.end());
		if (!m_SelectionIDs.empty())
		{
			bool recreated = false;
			if (!EnsureBufferCapacity(m_SelectionBuffer, m_SelectionIDs.size(), sizeof(uint32_t), "Selection", recreated))
				return false;
			if (recreated || !m_OutlineBindingSet)
			{
				nvrhi::BindingSetDesc desc;
				desc.bindings = {
					nvrhi::BindingSetItem::Texture_SRV(0, m_EntityIDTexture),
					nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_SelectionBuffer),
					nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::OutlineParameters))
				};
				m_OutlineBindingSet = m_Device->createBindingSet(desc, m_OutlineBindingLayout);
				if (!m_OutlineBindingSet)
				{
					ReportError("failed to create the selection outline bindings");
					return false;
				}
			}
			commandList->writeBuffer(m_SelectionBuffer, m_SelectionIDs.data(), m_SelectionIDs.size() * sizeof(uint32_t));

			RenderData::OutlineParameters parameters = {};
			parameters.Color = options.SelectionColor;
			parameters.Width = static_cast<int32_t>(std::clamp(options.OutlineWidth, 1u, 8u));
			parameters.SelectedCount = static_cast<uint32_t>(m_SelectionIDs.size());
			nvrhi::GraphicsState state;
			state.pipeline = m_OutlinePipeline;
			state.framebuffer = m_OverlayFramebuffer;
			state.viewport.addViewportAndScissorRect(viewport);
			state.bindings = { m_OutlineBindingSet };
			commandList->setGraphicsState(state);
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
			m_Stats.OutlinedEntities = parameters.SelectedCount;
		}

		if (onTopVertices > 0)
			DrawDebugLines(commandList, DebugDrawDepth::OnTop, testedVertices, onTopVertices);
		return true;
	}

	Entity SceneRenderer::GetEntityAt(Scene& scene, uint32_t x, uint32_t y)
	{
		if (!m_HasRenderedFrame || !m_EntityIDTexture || x >= m_ViewportSize.x || y >= m_ViewportSize.y)
			return {};

		ReadbackImage image;
		if (!Renderer::ReadTexture(m_EntityIDTexture, image) || image.BytesPerPixel != sizeof(uint32_t))
			return {};

		uint32_t id = 0;
		std::memcpy(&id, image.Pixels.data() + (static_cast<size_t>(y) * image.Width + x) * sizeof(uint32_t), sizeof(uint32_t));
		if (id == 0)
			return {};

		const entt::entity handle = static_cast<entt::entity>(id - 1);
		if (!scene.GetRegistry().valid(handle))
			return {};
		return Entity(handle, &scene);
	}

}
