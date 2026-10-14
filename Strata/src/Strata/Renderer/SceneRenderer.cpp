#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Math/Frustum.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/LODSelection.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/TextRenderer.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Renderer/TextureStreamer.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

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
		// Ambient occlusion lies in [0, 1]: half precision holds it, where the device can store r16f images (an optional
		// feature); r32f storage needs none.
		constexpr nvrhi::Format c_AOFormat = nvrhi::Format::R16_FLOAT;
		constexpr nvrhi::Format c_AOFallbackFormat = nvrhi::Format::R32_FLOAT;

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

		// Rasterizer depth bias of the shadow pass (reversed-Z: negative values push depth away from the light). The
		// constant part is in units of the depth format's resolution, the slope part scales the depth slope per texel.
		constexpr int c_ShadowDepthBias = -16;
		constexpr float c_ShadowSlopeDepthBias = -1.5f;

		// Elevation and azimuth (from +X toward +Z), in degrees, of SceneRenderer::GetDefaultSunDirection.
		constexpr float c_DefaultSunElevation = 45.0f;
		constexpr float c_DefaultSunAzimuth = 37.0f;
		// Of SceneRenderer::GetPreviewSunDirection: the top, +X and +Z faces of a box get 0.82, 0.54 and 0.20 of the
		// sunlight, different enough to tell them apart after tone mapping, which compresses bright values.
		constexpr float c_PreviewSunElevation = 55.0f;
		constexpr float c_PreviewSunAzimuth = 20.0f;

		glm::vec3 DirectionFromAngles(float elevationDegrees, float azimuthDegrees)
		{
			const float elevation = glm::radians(elevationDegrees);
			const float azimuth = glm::radians(azimuthDegrees);
			return glm::vec3(std::cos(elevation) * std::cos(azimuth), std::sin(elevation), std::cos(elevation) * std::sin(azimuth));
		}

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// A color of the procedural sky as the shader may use it: finite, not negative, within half-float range.
		glm::vec4 SanitizeRadiance(const glm::vec3& color)
		{
			return glm::vec4(IsFinite(color) ? glm::clamp(color, glm::vec3(0.0f), glm::vec3(65000.0f)) : glm::vec3(0.0f), 0.0f);
		}

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

		template<typename Component>
		bool HasActiveComponent(Scene& scene)
		{
			for (auto [entity, component, world] : scene.GetRegistry().view<Component, WorldTransformComponent>().each())
			{
				if (world.ActiveInHierarchy)
					return true;
			}
			return false;
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

	glm::vec3 SceneRenderer::GetDefaultSunDirection()
	{
		return DirectionFromAngles(c_DefaultSunElevation, c_DefaultSunAzimuth);
	}

	glm::vec3 SceneRenderer::GetPreviewSunDirection()
	{
		return DirectionFromAngles(c_PreviewSunElevation, c_PreviewSunAzimuth);
	}

	bool SceneRenderer::NeedsPreviewLighting(Scene& scene)
	{
		// Any light means the scene is lit the way it is meant to be, a night scene lit by a torch as well as a day.
		return !HasActiveComponent<DirectionalLightComponent>(scene) && !HasActiveComponent<SkyLightComponent>(scene)
			&& !HasActiveComponent<PointLightComponent>(scene) && !HasActiveComponent<SpotLightComponent>(scene);
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
		: m_Specification(specification), m_Errors("SceneRenderer '" + specification.DebugName + "'")
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
		m_SkinnedMeshVertexShader = shaders.Get("Scene/MeshSkinned.vert");
		m_SkinnedShadowVertexShader = shaders.Get("Scene/ShadowMeshSkinned.vert");
		m_PrepassPixelShader = shaders.Get(m_Specification.EntityIDs ? "Scene/DepthPrepass.frag" : "Scene/DepthPrepassNoEntityIDs.frag");
		m_ForwardPixelShader = shaders.Get("Scene/Forward.frag");
		m_ForwardTransparentPixelShader = shaders.Get("Scene/ForwardTransparent.frag");
		m_FullscreenVertexShader = shaders.Get("PostProcess/Fullscreen.vert");
		m_TonemapPixelShader = shaders.Get("PostProcess/Tonemap.frag");
		m_SkyboxPixelShader = shaders.Get("Scene/Skybox.frag");
		m_ShadowVertexShader = shaders.Get("Scene/ShadowMesh.vert");
		m_ShadowMaskPixelShader = shaders.Get("Scene/ShadowMask.frag");
		m_FXAAPixelShader = shaders.Get("PostProcess/FXAA.frag");
		ST_CORE_VERIFY(m_MeshVertexShader && m_PrepassPixelShader && m_ForwardPixelShader && m_ForwardTransparentPixelShader && m_FullscreenVertexShader
			&& m_TonemapPixelShader && m_SkyboxPixelShader && m_ShadowVertexShader && m_ShadowMaskPixelShader && m_FXAAPixelShader && m_SkinnedMeshVertexShader
			&& m_SkinnedShadowVertexShader, "SceneRenderer shaders are missing");

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
		// Skinned meshes add their skin stream (MeshSkinVertex): four joint indices and four weights in 1/65535ths.
		const nvrhi::VertexAttributeDesc skinnedAttributes[] = {
			attributes[0], attributes[1], attributes[2], attributes[3],
			nvrhi::VertexAttributeDesc().setName("JOINTS").setFormat(nvrhi::Format::RGBA16_UINT).setBufferIndex(2)
				.setOffset(offsetof(MeshSkinVertex, Joints)).setElementStride(sizeof(MeshSkinVertex)),
			nvrhi::VertexAttributeDesc().setName("WEIGHTS").setFormat(nvrhi::Format::RGBA16_UNORM).setBufferIndex(2)
				.setOffset(offsetof(MeshSkinVertex, Weights)).setElementStride(sizeof(MeshSkinVertex))
		};
		m_SkinnedInputLayout = m_Device->createInputLayout(skinnedAttributes, static_cast<uint32_t>(std::size(skinnedAttributes)), m_SkinnedMeshVertexShader);

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
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(14),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(15), // Joint matrices (only the skinned vertex shaders read them)
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
			&& EnsureBufferCapacity(m_DrawListBuffer, 1024, sizeof(uint32_t), "DrawList", recreated)
			&& EnsureBufferCapacity(m_MaterialBuffer, 256, sizeof(RenderData::MaterialData), "Materials", recreated)
			&& EnsureBufferCapacity(m_LightBuffer, 64, sizeof(RenderData::LightData), "Lights", recreated)
			&& EnsureBufferCapacity(m_LightBoundsBuffer, 64, sizeof(RenderData::LightBounds), "LightBounds", recreated)
			&& EnsureBufferCapacity(m_ShadowDrawListBuffer, 1024, sizeof(uint32_t), "ShadowDrawList", recreated)
			// Bound in every scene pass; it grows only once posed skinned meshes are drawn.
			&& EnsureBufferCapacity(m_JointBuffer, 1, sizeof(RenderData::AffineTransform), "Joints", recreated);
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
		const bool halfAO = Renderer::GetGraphicsDevice().GetInfo().SupportsExtendedStorageFormats;
		m_AOFormat = halfAO ? c_AOFormat : c_AOFallbackFormat;
		m_GTAOPipeline = createComputePipeline(halfAO ? "Effects/GTAOHalf.comp" : "Effects/GTAO.comp", m_AOBindingLayout);
		m_AOBlurPipeline = createComputePipeline(halfAO ? "Effects/AOBlurHalf.comp" : "Effects/AOBlur.comp", m_AOBindingLayout);
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
		m_TextRenderer = CreateScope<TextRenderer>(m_Specification.DebugName, m_FrameConstantBuffer);
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

		// The procedural sky reads nothing: only its output and the sky parameters.
		nvrhi::BindingLayoutDesc proceduralLayoutDesc;
		proceduralLayoutDesc.visibility = nvrhi::ShaderType::Compute;
		proceduralLayoutDesc.bindings = {
			nvrhi::BindingLayoutItem::Texture_UAV(0),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::ProceduralSkyParameters))
		};
		m_ProceduralSkyBindingLayout = m_Device->createBindingLayout(proceduralLayoutDesc);
		nvrhi::ComputePipelineDesc proceduralDesc;
		proceduralDesc.CS = shaders.Get("IBL/ProceduralSky.comp");
		ST_CORE_VERIFY(proceduralDesc.CS && m_ProceduralSkyBindingLayout, "SceneRenderer: the procedural sky shader or its layout is missing");
		proceduralDesc.bindingLayouts = { m_ProceduralSkyBindingLayout };
		m_ProceduralSkyPipeline = m_Device->createComputePipeline(proceduralDesc);
		ST_CORE_VERIFY(m_ProceduralSkyPipeline, "SceneRenderer: failed to create the IBL/ProceduralSky.comp pipeline");
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

	SceneRenderer::EnvironmentMaps SceneRenderer::CreateEnvironmentMaps()
	{
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
		maps.Cube = createCube(c_EnvironmentCubeSize, MipCountFor(c_EnvironmentCubeSize), "EnvironmentCube");
		maps.Irradiance = createCube(c_IrradianceSize, 1, "Irradiance");
		maps.Prefiltered = createCube(c_PrefilteredSize, c_PrefilteredMipCount, "Prefiltered");
		return maps;
	}

	void SceneRenderer::ProcessEnvironment(nvrhi::ICommandList* commandList, const Ref<Texture>& source)
	{
		ST_PROFILE_FUNCTION();
		EnvironmentMaps maps = CreateEnvironmentMaps();
		maps.Source = source;

		commandList->beginMarker("Environment");
		const uint32_t sourceWidth = source->GetWidth();
		DispatchIBL(commandList, m_EquirectToCubePipeline, source->GetGPUTexture(), nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D, maps.Cube, 0,
			nvrhi::TextureDimension::Texture2DArray, RenderData::IBLParameters { c_EnvironmentCubeSize, sourceWidth, 0.0f, 1 }, 6);
		FilterEnvironment(commandList, maps);
		commandList->endMarker();
		// Every path of a frame that recorded this executes its command list, and other renderers record (and execute) theirs
		// later on the same queue: a renderer that takes the maps over samples them only after they were computed.
		m_Environment = CreateRef<const EnvironmentMaps>(std::move(maps));
		ShareEnvironment(m_Environment);
		m_Stats.EnvironmentUpdates++;
	}

	struct SceneRenderer::SharedEnvironments
	{
		std::mutex Mutex; // Renderers draw on the main thread; the lock keeps the registry safe for any other
		// The entries do not keep the maps: they live as long as a renderer lights with them. A live entry's maps hold their
		// source texture, so its address names nothing else meanwhile.
		std::unordered_map<const Texture*, std::weak_ptr<const EnvironmentMaps>> Maps;
	};

	SceneRenderer::SharedEnvironments& SceneRenderer::GetSharedEnvironments()
	{
		static SharedEnvironments s_SharedEnvironments;
		return s_SharedEnvironments;
	}

	Ref<const SceneRenderer::EnvironmentMaps> SceneRenderer::FindSharedEnvironment(const Texture& source)
	{
		SharedEnvironments& shared = GetSharedEnvironments();
		std::scoped_lock<std::mutex> lock(shared.Mutex);
		auto it = shared.Maps.find(&source);
		if (it == shared.Maps.end())
			return nullptr;
		Ref<const EnvironmentMaps> maps = it->second.lock();
		if (!maps || maps->Source.get() != &source)
		{
			shared.Maps.erase(it); // No renderer lights with it any more
			return nullptr;
		}
		return maps;
	}

	void SceneRenderer::ShareEnvironment(const Ref<const EnvironmentMaps>& environment)
	{
		ST_CORE_ASSERT(environment && environment->Source && !environment->Procedural, "SceneRenderer: only the maps of environment map textures are shared");
		SharedEnvironments& shared = GetSharedEnvironments();
		std::scoped_lock<std::mutex> lock(shared.Mutex);
		// Entries of maps no renderer holds any more go first, so the registry holds as many entries as maps in use.
		std::erase_if(shared.Maps, [](const auto& entry) { return entry.second.expired(); });
		shared.Maps[environment->Source.get()] = environment;
	}

	void SceneRenderer::GenerateProceduralSky(nvrhi::ICommandList* commandList, const RenderData::ProceduralSkyParameters& parameters)
	{
		ST_PROFILE_FUNCTION();
		// The textures of the current procedural sky are rewritten in place: commands that still read them were recorded
		// earlier on the same queue, and the state tracking orders the writes after them. They are this renderer's own
		// (only maps computed from environment map textures are shared).
		EnvironmentMaps maps = m_Environment && m_Environment->Procedural ? *m_Environment : CreateEnvironmentMaps();
		maps.Source = nullptr;
		maps.Procedural = parameters;

		nvrhi::BindingSetDesc setDesc;
		setDesc.bindings = {
			nvrhi::BindingSetItem::Texture_UAV(0, maps.Cube, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 6), nvrhi::TextureDimension::Texture2DArray),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::ProceduralSkyParameters))
		};
		nvrhi::BindingSetHandle bindingSet = m_Device->createBindingSet(setDesc, m_ProceduralSkyBindingLayout);
		ST_CORE_VERIFY(bindingSet, "SceneRenderer: failed to create the procedural sky binding set");

		commandList->beginMarker("ProceduralSky");
		nvrhi::ComputeState state;
		state.pipeline = m_ProceduralSkyPipeline;
		state.bindings = { bindingSet };
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch(DispatchGroups(parameters.Size.x), DispatchGroups(parameters.Size.x), 6);
		FilterEnvironment(commandList, maps);
		commandList->endMarker();
		m_Environment = CreateRef<const EnvironmentMaps>(std::move(maps));
		m_Stats.EnvironmentUpdates++;
	}

	void SceneRenderer::FilterEnvironment(nvrhi::ICommandList* commandList, const EnvironmentMaps& maps)
	{
		const uint32_t cubeMips = MipCountFor(c_EnvironmentCubeSize);
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
	}

	RenderData::ProceduralSkyParameters SceneRenderer::GetProceduralSkyParameters(const SkyLightComponent& sky, const RenderData::FrameConstants& frame) const
	{
		RenderData::ProceduralSkyParameters parameters = {};
		parameters.ZenithColor = SanitizeRadiance(sky.ZenithColor);
		parameters.HorizonColor = SanitizeRadiance(sky.HorizonColor);
		parameters.GroundColor = SanitizeRadiance(sky.GroundColor);
		// The sun sits where the scene's light comes from (CollectLights ran before), tinted like it.
		const bool hasSun = frame.DirectionalLightDirection.w > 0.0f;
		const glm::vec3 sunDirection = hasSun ? -glm::vec3(frame.DirectionalLightDirection) : GetDefaultSunDirection();
		const glm::vec3 sunColor = hasSun ? m_SunColor : glm::vec3(1.0f);
		const float sunSize = std::isfinite(sky.SunSize) ? std::clamp(sky.SunSize, 0.0f, 180.0f) : 0.0f;
		const float sunIntensity = std::isfinite(sky.SunIntensity) ? std::max(sky.SunIntensity, 0.0f) : 0.0f;
		parameters.SunDirection = glm::vec4(sunDirection, glm::radians(sunSize) * 0.5f);
		parameters.SunRadiance = SanitizeRadiance(IsFinite(sunColor) ? glm::max(sunColor, glm::vec3(0.0f)) * sunIntensity : glm::vec3(0.0f));
		parameters.Size = glm::uvec4(c_EnvironmentCubeSize, 0, 0, 0);
		return parameters;
	}

	void SceneRenderer::UpdateEnvironment(Scene& scene, bool preview, RenderData::FrameConstants& frame, nvrhi::ICommandList* commandList)
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
		// The preview sky of an unlit scene in an editor view: the default procedural sky.
		SkyLightComponent previewSky;
		if (!skyLight && preview)
		{
			previewSky.Source = SkyLightSource::Procedural;
			skyLight = &previewSky;
		}
		const bool procedural = skyLight && skyLight->Source == SkyLightSource::Procedural;
		if (!skyLight || (!procedural && !skyLight->EnvironmentMap.IsValid()))
		{
			m_Environment = nullptr; // Release the maps of a removed environment
			m_EnvironmentHold = nullptr;
			return;
		}

		float rotation = 0.0f; // The procedural sky is computed in world space: its sun follows the light
		if (procedural)
		{
			m_EnvironmentHold = nullptr;
			const RenderData::ProceduralSkyParameters parameters = GetProceduralSkyParameters(*skyLight, frame);
			if (!m_Environment || !m_Environment->Procedural || *m_Environment->Procedural != parameters)
				GenerateProceduralSky(commandList, parameters);
		}
		else
		{
			Ref<Texture> texture = m_Assets.Resolve<Texture>(skyLight->EnvironmentMap, AssetPriority::High);
			if (!texture || !texture->GetGPUTexture())
			{
				// Constant ambient until the environment arrives (counted as pending by the resolve cache); the map is raised
				// whole as soon as it is loaded.
				HoldEnvironmentMap(skyLight->EnvironmentMap);
				return;
			}
			// The environment is computed from the whole map: this renderer holds it whole in its streamer until then. Once
			// computed, the cube is all renderers sample, so nothing asks for the map's levels any more and they drop to its
			// tail; a renderer that starts lighting with the map afterwards takes over the environment another one computed.
			if (!m_Environment || texture != m_Environment->Source)
			{
				if (Ref<const EnvironmentMaps> shared = FindSharedEnvironment(*texture))
				{
					m_Environment = std::move(shared);
				}
				else if (texture->GetResidentLevel() > 0)
				{
					HoldEnvironmentMap(skyLight->EnvironmentMap);
					m_Assets.CountPending(skyLight->EnvironmentMap);
					return;
				}
				else
				{
					ProcessEnvironment(commandList, texture);
				}
			}
			m_EnvironmentHold = nullptr;
			rotation = glm::radians(skyLight->Rotation);
		}

		frame.EnvironmentParams = glm::vec4(std::max(skyLight->Intensity, 0.0f), rotation, static_cast<float>(c_PrefilteredMipCount - 1), 1.0f);
		const float backgroundMips = static_cast<float>(MipCountFor(c_EnvironmentCubeSize) - 1);
		frame.SkyParams = glm::vec4(std::clamp(skyLight->BackgroundBlur, 0.0f, 1.0f) * backgroundMips, skyLight->ShowBackground ? 1.0f : 0.0f, 0.0f, 0.0f);
		m_Stats.EnvironmentLighting = true;
	}

	void SceneRenderer::HoldEnvironmentMap(AssetHandle map)
	{
		// A need reported only in the frames this renderer draws would not do: one that draws now and then (the capture
		// renderer of a headless editor, whose viewport panel never draws) would find the map at its tail every time, its
		// levels raised for its frame and dropped again before its next. The hold lasts until the environment is computed.
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		TextureStreamer* streamer = manager ? TextureStreamer::Find(*manager) : nullptr;
		if (!streamer)
		{
			m_EnvironmentHold = nullptr;
			return;
		}
		if (!m_EnvironmentHold || m_EnvironmentHold->Texture != map)
			m_EnvironmentHold = CreateRef<TextureHold>(TextureHold { map });
		// Again in every frame it waits: a hold placed in another manager's streamer (a project opened since) lapsed with it.
		streamer->Hold(m_EnvironmentHold);
	}

	bool SceneRenderer::CreateSceneBindingSets()
	{
		nvrhi::ITexture* blackCube = Renderer::GetBlackCubeTexture();
		nvrhi::ITexture* environmentCube = GetEnvironmentCube();
		const bool hasEnvironment = environmentCube != nullptr;
		nvrhi::ITexture* shadowMap = m_ShadowMap ? m_ShadowMap.Get() : m_DummyShadowMap.Get();

		// The shadow pass reads the same resources except for its own draw list (and never the shadow map).
		auto create = [&](nvrhi::IBuffer* drawList, nvrhi::ITexture* boundShadowMap)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_InstanceBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(14, drawList),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(15, m_JointBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_MaterialBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_LightBuffer),
				nvrhi::BindingSetItem::Texture_SRV(3, hasEnvironment ? m_Environment->Irradiance.Get() : blackCube),
				nvrhi::BindingSetItem::Texture_SRV(4, hasEnvironment ? m_Environment->Prefiltered.Get() : blackCube),
				nvrhi::BindingSetItem::Texture_SRV(5, m_BRDFLut),
				nvrhi::BindingSetItem::Texture_SRV(6, hasEnvironment ? environmentCube : blackCube),
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
		m_SceneBindingSet = create(m_DrawListBuffer, shadowMap);
		m_ShadowBindingSet = create(m_ShadowDrawListBuffer, m_DummyShadowMap);

		nvrhi::BindingSetDesc clusterSet;
		clusterSet.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_LightBoundsBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_ClusterBuffer)
		};
		m_ClusterBindingSet = m_Device->createBindingSet(clusterSet, m_ClusterBindingLayout);

		m_BoundEnvironment = environmentCube;
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
		return LODSelection::SelectLOD(screenFraction, m_Specification.LODThreshold, static_cast<uint32_t>(submesh.LODs.size()));
	}

	float SceneRenderer::GetScreenFraction(const SceneCamera& camera, const glm::vec3& center, float radius)
	{
		return LODSelection::GetScreenFraction(camera, center, radius);
	}

	bool SceneRenderer::UsesIndirectDraws() const
	{
		// One indirect call per bucket needs several draws per call and draws that set their first instance (the draw list
		// offset of their batch); without them, each batch is a direct draw (the bucket's state is still set once).
		const GraphicsDeviceInfo& info = Renderer::GetGraphicsDevice().GetInfo();
		return m_Specification.IndirectMode != IndirectDrawMode::PerDraw && info.SupportsMultiDrawIndirect && info.SupportsDrawIndirectFirstInstance;
	}

	void SceneRenderer::BuildDrawArguments()
	{
		m_DrawArgumentsBuild.clear();
		const auto append = [this](const SceneDrawBatch& batch)
		{
			const GeometryAllocation& geometry = m_FrameGeometry[batch.MeshSlot];
			const Submesh& submesh = m_FrameMeshes[batch.MeshSlot]->GetSubmeshes()[batch.SubmeshIndex];
			const MeshLOD& lod = submesh.LODs[batch.LOD];
			nvrhi::DrawIndexedIndirectArguments arguments;
			arguments.indexCount = lod.IndexCount;
			arguments.instanceCount = batch.InstanceCount;
			arguments.startIndexLocation = geometry.FirstIndex + lod.IndexOffset;
			arguments.baseVertexLocation = static_cast<int32_t>(geometry.FirstVertex + submesh.BaseVertex);
			arguments.startInstanceLocation = batch.FirstInstance;
			m_DrawArgumentsBuild.push_back(arguments);
		};
		// Opaque batches (the prepass and the opaque pass draw them alike), transparent ones, then the shadow batches.
		for (const SceneDrawBatch& batch : m_OpaqueBatches)
			append(batch);
		for (const SceneDrawBatch& batch : m_TransparentBatches)
			append(batch);
		for (const SceneDrawBatch& batch : m_ShadowBatches)
			append(batch);
	}

	void SceneRenderer::DrawBuckets(nvrhi::ICommandList* commandList, nvrhi::GraphicsState& state, std::span<const SceneDrawBatch> batches, uint32_t firstArgument,
		const std::function<PipelineKey(const SceneDrawBatch&)>& getKey, const std::function<void(const SceneDrawBatch&, nvrhi::GraphicsState&)>& prepareState,
		const std::function<void(const SceneDrawBatch&)>& afterState)
	{
		const GeometryPool& pool = *Renderer::GetGeometryPool();
		const bool indirect = UsesIndirectDraws();
		state.indexBuffer = nvrhi::IndexBufferBinding { pool.GetIndexBuffer(), nvrhi::Format::R32_UINT, 0 };
		state.indirectParams = indirect ? m_DrawArgumentBuffer.Get() : nullptr;
		size_t first = 0;
		while (first < batches.size())
		{
			// A bucket: the batches up to the next change of pipeline state or vertex arena (and, for shadows, of cascade).
			const SceneDrawBatch& head = batches[first];
			const PipelineKey key = getKey(head);
			const GeometryArena arena = m_FrameGeometry[head.MeshSlot].Arena;
			size_t end = first + 1;
			while (end < batches.size() && getKey(batches[end]) == key && m_FrameGeometry[batches[end].MeshSlot].Arena == arena && batches[end].Cascade == head.Cascade)
				end++;

			state.pipeline = GetScenePipeline(key);
			state.vertexBuffers = {
				nvrhi::VertexBufferBinding { pool.GetPositionBuffer(arena), 0, 0 },
				nvrhi::VertexBufferBinding { pool.GetAttributeBuffer(arena), 1, 0 }
			};
			if (key.Skinned)
				state.vertexBuffers.push_back(nvrhi::VertexBufferBinding { pool.GetSkinBuffer(), 2, 0 });
			if (prepareState)
				prepareState(head, state);
			commandList->setGraphicsState(state);
			if (afterState)
				afterState(head);
			if (indirect)
			{
				commandList->drawIndexedIndirect(static_cast<uint32_t>((firstArgument + first) * sizeof(nvrhi::DrawIndexedIndirectArguments)),
					static_cast<uint32_t>(end - first));
				m_Stats.DrawCommands++;
			}
			else
			{
				for (size_t index = first; index < end; index++)
				{
					// The frame's arguments: the upload made them what the argument buffer holds (m_DrawArguments).
					const nvrhi::DrawIndexedIndirectArguments& arguments = m_DrawArguments[firstArgument + index];
					nvrhi::DrawArguments draw;
					draw.vertexCount = arguments.indexCount;
					draw.instanceCount = arguments.instanceCount;
					draw.startIndexLocation = arguments.startIndexLocation;
					draw.startVertexLocation = static_cast<uint32_t>(arguments.baseVertexLocation);
					draw.startInstanceLocation = arguments.startInstanceLocation;
					commandList->drawIndexed(draw);
					m_Stats.DrawCommands++;
				}
			}
			first = end;
		}
	}

	void SceneRenderer::DrawShadows(nvrhi::ICommandList* commandList, uint32_t cascadeCount)
	{
		commandList->clearDepthStencilTexture(m_ShadowMap, nvrhi::AllSubresources, true, 0.0f, false, 0);
		const float mapSize = static_cast<float>(m_Specification.ShadowMapSize);
		// Batches are sorted by cascade first: the ones of cascades beyond the count (none) end the list.
		size_t count = 0;
		while (count < m_ShadowBatches.size() && m_ShadowBatches[count].Cascade < cascadeCount)
			count++;
		nvrhi::GraphicsState state;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(mapSize, mapSize));
		state.bindings = { m_ShadowBindingSet, Renderer::GetBindlessTextures().GetTable(), m_ShadowPushBindingSet };
		const uint32_t firstArgument = static_cast<uint32_t>(m_OpaqueBatches.size() + m_TransparentBatches.size());
		// Each bucket draws into its cascade's layer, which the vertex shader also learns (a push constant after the state).
		DrawBuckets(commandList, state, std::span<const SceneDrawBatch>(m_ShadowBatches.data(), count), firstArgument,
			[](const SceneDrawBatch& batch) { return PipelineKey { batch.AlphaMask ? Pass::ShadowMasked : Pass::Shadow, batch.DoubleSided, batch.Mirrored, batch.Skinned }; },
			[this](const SceneDrawBatch& batch, nvrhi::GraphicsState& bucketState) { bucketState.framebuffer = m_ShadowFramebuffers[batch.Cascade]; },
			[commandList](const SceneDrawBatch& batch)
			{
				const uint32_t cascade = batch.Cascade;
				commandList->setPushConstants(&cascade, sizeof(cascade));
			});
	}

	bool SceneRenderer::EnsureBufferCapacity(nvrhi::BufferHandle& buffer, size_t elementCount, size_t elementSize, const char* name, bool& outRecreated, BufferKind kind)
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
		switch (kind)
		{
			case BufferKind::Vertex:
				desc.isVertexBuffer = true;
				desc.initialState = nvrhi::ResourceStates::VertexBuffer;
				break;
			case BufferKind::IndirectArguments:
				desc.isDrawIndirectArgs = true;
				desc.initialState = nvrhi::ResourceStates::IndirectArgument;
				break;
			case BufferKind::Structured:
				desc.structStride = static_cast<uint32_t>(elementSize);
				desc.initialState = nvrhi::ResourceStates::ShaderResource;
				break;
		}
		desc.keepInitialState = true;
		nvrhi::BufferHandle created = m_Device->createBuffer(desc);
		if (!created)
		{
			m_Errors.Report(fmt::format("failed to allocate the {} buffer ({} bytes)", name, capacity));
			return false;
		}
		// Frames in flight may still read the replaced buffer.
		if (buffer)
			Renderer::ReleaseDeferred({ nvrhi::ResourceHandle(buffer.Get()) });
		buffer = created;
		outRecreated = true;
		return true;
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
			m_Errors.Report(fmt::format("viewport size {}x{} exceeds the device's texture size limit ({})", width, height, limit));
			return;
		}
		m_TargetsValid = CreateRenderTargets();
	}

	void SceneRenderer::ReleaseRenderTargets()
	{
		// Submitted frames may still use the targets. NVRHI keeps alive what their command lists reference, but not the
		// textures they only cleared (in a view where nothing is drawn, the entity IDs are only cleared): the deferred
		// release keeps everything until the GPU is done with it.
		std::vector<nvrhi::ResourceHandle> released;
		auto release = [&released](auto& handle)
		{
			if (handle)
				released.emplace_back(handle.Get());
			handle = nullptr;
		};
		release(m_DepthTexture);
		release(m_NormalTexture);
		release(m_EntityIDTexture);
		release(m_HDRTexture);
		release(m_OutputTexture);
		release(m_AORawTexture);
		release(m_AOTexture);
		release(m_GTAOBindingSet);
		release(m_AOBlurBindingSet);
		release(m_PrepassFramebuffer);
		release(m_ForwardFramebuffer);
		release(m_OutputFramebuffer);
		release(m_ExposureBindingSet);
		release(m_BloomTexture);
		m_BloomLevels = 0;
		for (std::vector<nvrhi::BindingSetHandle>* sets : { &m_BloomDownsampleSets, &m_BloomUpsampleSets })
		{
			for (nvrhi::BindingSetHandle& set : *sets)
				release(set);
			sets->clear();
		}
		release(m_TonemapBindingSet);
		release(m_LDRTexture);
		release(m_LDRFramebuffer);
		release(m_FXAABindingSet);
		release(m_SceneBindingSet); // References the ambient occlusion texture
		release(m_ShadowBindingSet);
		release(m_OverlayFramebuffer);
		release(m_CopyBindingSet);
		release(m_OutlineBindingSet); // References the entity-ID texture
		release(m_StatusTintBindingSet); // Likewise
		Renderer::ReleaseDeferred(std::move(released));
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
			m_Errors.Report(fmt::format("failed to create the {} for a {}x{} viewport", what, width, height));
			return false;
		};

		m_DepthTexture = CreateRenderTarget(m_Device, width, height, c_DepthFormat, name + ".Depth", true);
		m_NormalTexture = CreateRenderTarget(m_Device, width, height, c_NormalFormat, name + ".Normals", false);
		if (m_Specification.EntityIDs)
			m_EntityIDTexture = CreateRenderTarget(m_Device, width, height, c_EntityIDFormat, name + ".EntityIDs", false);
		m_HDRTexture = CreateRenderTarget(m_Device, width, height, c_HDRFormat, name + ".HDR", false);
		// Tone mapping writes here when FXAA follows.
		m_LDRTexture = CreateRenderTarget(m_Device, width, height, c_OutputFormat, name + ".LDR", false);
		auto createStorage = [&](const std::string& debugName)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = m_AOFormat;
			desc.isUAV = true;
			desc.debugName = debugName;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			return m_Device->createTexture(desc);
		};
		m_AORawTexture = createStorage(name + ".AORaw");
		m_AOTexture = createStorage(name + ".AO");
		if (!m_DepthTexture || !m_NormalTexture || (m_Specification.EntityIDs && !m_EntityIDTexture) || !m_HDRTexture || !m_LDRTexture || !m_AORawTexture
			|| !m_AOTexture)
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

		nvrhi::FramebufferDesc prepassDesc;
		prepassDesc.addColorAttachment(m_NormalTexture);
		if (m_EntityIDTexture)
			prepassDesc.addColorAttachment(m_EntityIDTexture);
		prepassDesc.setDepthAttachment(m_DepthTexture);
		m_PrepassFramebuffer = m_Device->createFramebuffer(prepassDesc);
		m_ForwardFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc()
			.addColorAttachment(m_HDRTexture)
			.setDepthAttachment(m_DepthTexture));
		m_LDRFramebuffer = m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_LDRTexture));
		if (!m_GTAOBindingSet || !m_AOBlurBindingSet || !m_PrepassFramebuffer || !m_ForwardFramebuffer || !m_LDRFramebuffer)
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
		return true;
	}

	bool SceneRenderer::EnsureOutputTarget()
	{
		if (m_OutputTexture)
			return true;

		const uint32_t width = m_ViewportSize.x;
		const uint32_t height = m_ViewportSize.y;
		nvrhi::TextureHandle output = CreateRenderTarget(m_Device, width, height, c_OutputFormat, m_Specification.DebugName + ".Output", false);
		nvrhi::FramebufferHandle outputFramebuffer = output ? m_Device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(output)) : nullptr;
		// Overlays are drawn into the output texture, tested against the scene depth (which they never change).
		nvrhi::FramebufferHandle overlayFramebuffer = output ? m_Device->createFramebuffer(nvrhi::FramebufferDesc()
			.addColorAttachment(output)
			.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(m_DepthTexture).setReadOnly(true))) : nullptr;
		nvrhi::BindingSetHandle copyBindingSet;
		if (output)
		{
			nvrhi::BindingSetDesc copySet;
			copySet.bindings = { nvrhi::BindingSetItem::Texture_SRV(0, output), nvrhi::BindingSetItem::Sampler(0, m_PointClampSampler) };
			copyBindingSet = m_Device->createBindingSet(copySet, m_FXAABindingLayout);
		}
		if (!output || !outputFramebuffer || !overlayFramebuffer || !copyBindingSet)
		{
			// Nothing used them yet: they go with their handles.
			m_Errors.Report(fmt::format("failed to create the output texture for a {}x{} viewport", width, height));
			return false;
		}
		m_OutputTexture = std::move(output);
		m_OutputFramebuffer = std::move(outputFramebuffer);
		m_OverlayFramebuffer = std::move(overlayFramebuffer);
		m_CopyBindingSet = std::move(copyBindingSet);
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
		desc.inputLayout = key.Skinned ? m_SkinnedInputLayout : m_InputLayout;
		if (key.Skinned)
			desc.VS = shadowPass ? m_SkinnedShadowVertexShader : m_SkinnedMeshVertexShader;
		else
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

	void SceneRenderer::CollectLights(Scene& scene, const SceneCamera& camera, bool preview, RenderData::FrameConstants& frame)
	{
		ST_PROFILE_STAGE("Render.Extract");
		entt::registry& registry = scene.GetRegistry();

		frame.DirectionalLightDirection = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
		frame.DirectionalLightColor = glm::vec4(0.0f);
		m_ShadowLight = {};
		m_SunColor = glm::vec3(1.0f);
		auto useDirectionalLight = [&](const DirectionalLightComponent& light, const glm::vec3& direction)
		{
			frame.DirectionalLightDirection = glm::vec4(direction, 1.0f);
			frame.DirectionalLightColor = glm::vec4(light.Color * light.Intensity, 1.0f);
			m_SunColor = light.Color;
			m_ShadowLight.Enabled = light.CastShadows && light.Intensity > 0.0f;
			m_ShadowLight.Direction = direction;
			m_ShadowLight.Distance = light.ShadowDistance;
			m_ShadowLight.Softness = light.ShadowSoftness;
			m_ShadowLight.Bias = light.ShadowBias;
			m_ShadowLight.NormalBias = light.ShadowNormalBias;
		};
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
			useDirectionalLight(light, direction / length);
		}
		// The preview sun is a default directional light (with shadows). The preview sky's disk follows it, like any sun.
		if (preview)
			useDirectionalLight(DirectionalLightComponent(), -GetPreviewSunDirection());

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

	void SceneRenderer::DrawBatches(nvrhi::ICommandList* commandList, Pass pass, bool transparent)
	{
		const std::vector<SceneDrawBatch>& batches = transparent ? m_TransparentBatches : m_OpaqueBatches;
		if (batches.empty())
			return;

		nvrhi::GraphicsState state;
		state.framebuffer = pass == Pass::Prepass ? m_PrepassFramebuffer.Get() : m_ForwardFramebuffer.Get();
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y)));
		state.bindings = { m_SceneBindingSet, Renderer::GetBindlessTextures().GetTable() };
		const uint32_t firstArgument = transparent ? static_cast<uint32_t>(m_OpaqueBatches.size()) : 0u;
		DrawBuckets(commandList, state, batches, firstArgument,
			[pass](const SceneDrawBatch& batch) { return PipelineKey { pass, batch.DoubleSided, batch.Mirrored, batch.Skinned }; }, nullptr, nullptr);
		if (pass == Pass::Prepass)
			return;
		for (const SceneDrawBatch& batch : batches)
		{
			const MeshLOD& lod = m_FrameMeshes[batch.MeshSlot]->GetSubmeshes()[batch.SubmeshIndex].LODs[batch.LOD];
			m_Stats.DrawCalls++;
			m_Stats.Triangles += static_cast<uint64_t>(lod.IndexCount / 3) * batch.InstanceCount;
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
		// The final passes write one color output: further attachments would be left undefined.
		if (desc.colorAttachments.size() != 1 || !desc.colorAttachments[0].texture)
		{
			m_Errors.Report(fmt::format("the target framebuffer has {} color attachments; exactly one is required", desc.colorAttachments.size()));
			return false;
		}
		const nvrhi::FormatInfo& format = nvrhi::getFormatInfo(info.colorFormats[0]);
		if (format.kind != nvrhi::FormatKind::Normalized || format.isSigned || format.isSRGB || info.sampleCount != 1)
		{
			m_Errors.Report(fmt::format("the target's color format {} is not supported (single-sampled, non-sRGB UNORM required)", format.name));
			return false;
		}
		if (info.width != m_ViewportSize.x || info.height != m_ViewportSize.y)
		{
			m_Errors.Report(fmt::format("the target is {}x{} but the viewport is {}x{}; the renderer does not rescale", info.width, info.height, m_ViewportSize.x,
				m_ViewportSize.y));
			return false;
		}
		return true;
	}

	bool SceneRenderer::Render(Scene& scene, const SceneCamera& camera, nvrhi::IFramebuffer* target, const SceneRenderOptions& options)
	{
		ST_PROFILE_FUNCTION();
		ST_PROFILE_STAGE("Render.Submit"); // Extraction (Render.Extract) nests in it
		const bool rendered = RenderFrame(scene, camera, target, options);
		if (rendered)
			ReportTextureUse(camera);
		// The references the frame resolved end with it, so a renderer that stops rendering (a capture renderer between
		// captures, a hidden view) keeps none of the meshes, materials and textures it drew resident (the environment
		// cache keeps the sky light's map) and no asset manager alive.
		ReleaseFrameAssets();
		return rendered;
	}

	void SceneRenderer::ReportTextureUse(const SceneCamera& camera)
	{
		// A view that jumped since the previous frame: what it needs now goes first.
		const glm::vec3 forward = -glm::vec3(camera.View[0][2], camera.View[1][2], camera.View[2][2]);
		const float forwardLength = glm::length(forward);
		const glm::vec3 direction = forwardLength > 0.0f ? forward / forwardLength : glm::vec3(0.0f, 0.0f, -1.0f);
		const bool cut = m_HasPreviousCamera && TextureStreamingPolicy::IsCameraCut(m_PreviousCameraPosition, m_PreviousCameraForward, camera.Position, direction);
		m_HasPreviousCamera = true;
		m_PreviousCameraPosition = camera.Position;
		m_PreviousCameraForward = direction;

		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		TextureStreamer* streamer = manager ? TextureStreamer::Find(*manager) : nullptr;
		if (!streamer)
			return;
		TextureStreamingView view;
		view.ViewportHeight = static_cast<float>(m_ViewportSize.y);
		view.Streaming = m_Specification.TextureStreaming;
		view.CameraCut = cut;
		streamer->ReportView(view, m_VisibleTextures);
	}

	GPUTimerRecord& SceneRenderer::BeginGPURecord()
	{
		// A record per profiler frame slot: a frame of a slot the newest record does not belong to starts the next record.
		GPUTimerRecord& newest = m_GPURecords[m_GPURecordIndex];
		if (newest.Slot != 0 && Renderer::IsInitialized() && newest.Slot != Renderer::GetGPUProfiler().GetOpenSlot())
		{
			m_GPURecordIndex = (m_GPURecordIndex + 1) % c_GPURecordCount;
			m_GPURecords[m_GPURecordIndex].Clear();
		}
		return m_GPURecords[m_GPURecordIndex];
	}

	void SceneRenderer::ResolveGPUStats(bool wait)
	{
		if (!Renderer::IsInitialized())
			return;
		GPUProfiler& profiler = Renderer::GetGPUProfiler();
		if (!profiler.IsEnabled())
			return;
		profiler.Resolve(wait);
		// The newest record whose timestamps are all written; older ones are then of no use.
		for (uint32_t age = 0; age < c_GPURecordCount; age++)
		{
			GPUTimerRecord& record = m_GPURecords[(m_GPURecordIndex + c_GPURecordCount - age) % c_GPURecordCount];
			if (record.Slot != 0 && record.Slot == m_ResolvedGPURecord)
				return; // Known already, as everything older
			if (record.Slot == 0)
				continue;
			if (!profiler.ReadRecord(record, m_GPUStageScratch))
				continue;
			m_GPUStats.HasTimes = true;
			m_GPUStats.Frame = record.ProfilerFrame;
			m_GPUStats.StageMs = m_GPUStageScratch;
			m_GPUStats.BusyMs = 0.0f;
			for (const GPUStageTime& stage : m_GPUStats.StageMs)
				m_GPUStats.BusyMs += stage.Milliseconds;
			m_ResolvedGPURecord = record.Slot;
			for (uint32_t older = age + 1; older < c_GPURecordCount; older++)
				m_GPURecords[(m_GPURecordIndex + c_GPURecordCount - older) % c_GPURecordCount].Clear();
			return;
		}
	}

	bool SceneRenderer::RenderFrame(Scene& scene, const SceneCamera& camera, nvrhi::IFramebuffer* target, const SceneRenderOptions& options)
	{
		ResolveGPUStats(false);
		m_Stats = {};
		if (m_ViewportSize.x == 0 || m_ViewportSize.y == 0)
			return false; // Nothing to show (e.g. a minimized window); not an error
		if (!m_TargetsValid)
		{
			m_Errors.Report("the render targets are unavailable (see the previous error)");
			return false;
		}
		if (target && !ValidateTarget(target))
			return false;

		// The scene's render proxies follow what changed since the last frame (world transforms included), and their mesh
		// layouts what the asset manager loaded.
		const Ref<AssetManagerBase>& assets = AssetManager::GetActive();
		RenderScene& renderScene = RenderScene::Get(scene);
		{
			// Following the scene's changes is the first part of extraction, like the culling and gathering below.
			ST_PROFILE_STAGE("Render.Extract");
			// Another user of the render scene (a second renderer, the editor's debug views) may have synced it already: the
			// stat counts the updates since this renderer's previous frame.
			renderScene.Sync(assets);
			const uint64_t totalUpdates = renderScene.GetStats().TotalUpdates;
			m_Stats.ProxyUpdates = static_cast<uint32_t>(totalUpdates - (m_ProxyUpdatesScene == renderScene.GetId() ? m_ProxyUpdatesSeen : 0));
			m_ProxyUpdatesScene = renderScene.GetId();
			m_ProxyUpdatesSeen = totalUpdates;
			m_Assets.BeginFrame(assets);
			PrepareExtraction(renderScene);
		}

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
		// Preview lighting only stands in for lighting the scene does not have at all.
		const bool preview = options.PreviewEnvironment && NeedsPreviewLighting(scene);
		m_Stats.PreviewLighting = preview;
		CollectLights(scene, camera, preview, frame);

		const auto extractionStart = std::chrono::steady_clock::now();
		const Frustum frustum(frame.ViewProjection);
		CullView(renderScene, camera, frustum);
		FindShadowCasters(renderScene, camera, frustum, frame);
		ResolveAssets(renderScene);
		GatherDraws(renderScene, camera);
		GatherShadowDraws(renderScene, camera, frame);
		BuildDrawArguments();
		ReportVisibleAnimators(scene);
		m_Stats.ExtractMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - extractionStart).count() - m_Stats.SortMs;
		bool degraded = false; // Rendered with a reported limitation: keep the error so it is not logged every frame
		if (frame.ShadowParams.w > 0.0f && !EnsureShadowMap())
		{
			m_Errors.Report("failed to create the shadow map; rendering without shadows");
			frame.ShadowParams.w = 0.0f;
			degraded = true;
		}
		const PostProcessComponent postProcess = GetPostProcessSettings(scene);
		frame.AOParams = glm::vec4(postProcess.AmbientOcclusion ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

		bool buffersRecreated = !m_SceneBindingSet || m_BoundShadowMap != (m_ShadowMap ? m_ShadowMap.Get() : m_DummyShadowMap.Get());
		bool instancesRecreated = false;
		bool materialsRecreated = false;
		bool drawListsRecreated = false;
		bool jointsRecreated = false;
		const bool buffersReady = EnsureBufferCapacity(m_InstanceBuffer, renderScene.GetInstances().size(), sizeof(RenderData::InstanceData), "Instances", instancesRecreated)
			&& EnsureBufferCapacity(m_JointBuffer, renderScene.GetPalette().size(), sizeof(RenderData::AffineTransform), "Joints", jointsRecreated)
			&& EnsureBufferCapacity(m_MaterialBuffer, m_MaterialData.size(), sizeof(RenderData::MaterialData), "Materials", materialsRecreated)
			&& EnsureBufferCapacity(m_DrawListBuffer, m_DrawInstancesBuild.size(), sizeof(uint32_t), "DrawList", drawListsRecreated)
			&& EnsureBufferCapacity(m_ShadowDrawListBuffer, m_ShadowDrawInstancesBuild.size(), sizeof(uint32_t), "ShadowDrawList", drawListsRecreated)
			&& EnsureBufferCapacity(m_LightBuffer, m_Lights.size(), sizeof(RenderData::LightData), "Lights", buffersRecreated)
			&& EnsureBufferCapacity(m_LightBoundsBuffer, m_LightBounds.size(), sizeof(RenderData::LightBounds), "LightBounds", buffersRecreated)
			&& EnsureBufferCapacity(m_DrawArgumentBuffer, m_DrawArgumentsBuild.size(), sizeof(nvrhi::DrawIndexedIndirectArguments), "DrawArguments", drawListsRecreated,
				BufferKind::IndirectArguments);
		buffersRecreated = buffersRecreated || instancesRecreated || materialsRecreated || drawListsRecreated || jointsRecreated;
		if (!buffersReady)
		{
			// A replaced buffer holds nothing yet: upload everything to the next one.
			m_FullUpload = m_FullUpload || instancesRecreated || materialsRecreated || drawListsRecreated || jointsRecreated;
			m_Stats = {};
			return false;
		}

		m_Stats.Lights = static_cast<uint32_t>(m_Lights.size()) + (frame.DirectionalLightDirection.w > 0.0f ? 1u : 0u);

		nvrhi::ICommandList* commandList = m_CommandList;
		commandList->open();
		commandList->beginMarker(m_Specification.DebugName.c_str());
		// The GPU's time per pass: each stage runs from its first command to the next stage's.
		const RendererGPUStages& gpuStages = RendererGPUStages::Get();
		GPUPassTimer gpuTimer(commandList, gpuStages.Setup, &BeginGPURecord());
		UpdateEnvironment(scene, preview, frame, commandList);
		if ((buffersRecreated || m_BoundEnvironment != GetEnvironmentCube()) && !CreateSceneBindingSets())
		{
			gpuTimer.End();
			commandList->endMarker();
			commandList->close();
			m_Device->executeCommandList(commandList); // Keeps the environment processing that was recorded
			m_Errors.Report("failed to create the scene binding sets");
			m_FullUpload = m_FullUpload || instancesRecreated || materialsRecreated || drawListsRecreated || jointsRecreated;
			m_Stats = {};
			return false;
		}

		commandList->writeBuffer(m_FrameConstantBuffer, &frame, sizeof(frame));
		UploadSceneData(commandList, renderScene, instancesRecreated, materialsRecreated, drawListsRecreated, jointsRecreated);
		if (!m_Lights.empty())
		{
			commandList->writeBuffer(m_LightBuffer, m_Lights.data(), m_Lights.size() * sizeof(RenderData::LightData));
			commandList->writeBuffer(m_LightBoundsBuffer, m_LightBounds.data(), m_LightBounds.size() * sizeof(RenderData::LightBounds));
		}

		if (!m_Lights.empty())
		{
			commandList->beginMarker("LightClusters");
			BuildLightClusters(commandList);
			commandList->endMarker();
		}

		if (frame.ShadowParams.w > 0.0f)
		{
			gpuTimer.Next(gpuStages.Shadows);
			commandList->beginMarker("Shadows");
			DrawShadows(commandList, static_cast<uint32_t>(frame.ShadowParams.w));
			commandList->endMarker();
		}

		gpuTimer.Next(gpuStages.Prepass);
		const glm::vec4 clear = camera.ClearColor;
		commandList->clearDepthStencilTexture(m_DepthTexture, nvrhi::AllSubresources, true, 0.0f, false, 0);
		commandList->clearTextureFloat(m_NormalTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		if (m_EntityIDTexture)
			commandList->clearTextureUInt(m_EntityIDTexture, nvrhi::AllSubresources, 0);
		commandList->clearTextureFloat(m_HDRTexture, nvrhi::AllSubresources, nvrhi::Color(SRGBToLinear(clear.r), SRGBToLinear(clear.g), SRGBToLinear(clear.b), 1.0f));

		commandList->beginMarker("Prepass");
		DrawBatches(commandList, Pass::Prepass, false);
		commandList->endMarker();

		if (postProcess.AmbientOcclusion)
		{
			gpuTimer.Next(gpuStages.AO);
			commandList->beginMarker("AmbientOcclusion");
			RenderAmbientOcclusion(commandList, postProcess);
			commandList->endMarker();
		}
		gpuTimer.Next(gpuStages.Opaque);
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
		gpuTimer.Next(gpuStages.Transparent);
		commandList->beginMarker("Transparent");
		DrawBatches(commandList, Pass::Transparent, true);
		commandList->endMarker();

		gpuTimer.Next(gpuStages.Post);
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

		TextRenderStats textStats;
		const bool text = m_TextRenderer->Prepare(scene, m_ViewportSize, options.DrawScreenSpaceText, commandList, textStats);
		m_Stats.Texts = textStats.Texts;
		m_Stats.ScreenSpaceTexts = textStats.ScreenSpaceTexts;
		m_Stats.HiddenScreenSpaceTexts = textStats.HiddenScreenSpaceTexts;
		m_Stats.TextGlyphs = textStats.Glyphs;
		m_Stats.PendingTextGlyphs = textStats.PendingGlyphs;
		m_Stats.RasterizedTextGlyphs = textStats.RasterizedGlyphs;
		const AssetResolveStats& assetStats = m_Assets.GetStats();
		m_Stats.PendingAssets = assetStats.Pending + textStats.PendingFonts;
		m_Stats.RequestsByVisibility = assetStats.Requested;
		m_Stats.PrefetchRequests = assetStats.Prefetched;
		m_Stats.GetAssetCalls = assetStats.GetAssetCalls;

		// Overlays (and text) are composed in the output texture, which is then copied into the target. Without a target or
		// anything to compose, the frame needs no output texture: renderers that only render into targets (games without
		// text) never allocate one.
		const bool overlays = HasOverlays(options) || text;
		if ((!target || overlays) && !EnsureOutputTarget())
		{
			gpuTimer.End();
			commandList->endMarker();
			commandList->close();
			m_Device->executeCommandList(commandList); // Keeps the uploads that were recorded
			m_Stats = {};
			return false;
		}
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
			gpuTimer.Next(gpuStages.Overlays);
			commandList->beginMarker("Overlays");
			const bool overlaysDrawn = RenderOverlays(commandList, scene, options, text);
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

		gpuTimer.End();
		commandList->endMarker();
		commandList->close();
		m_Device->executeCommandList(commandList);

		m_Stats.Rendered = true;
		m_HasRenderedFrame = true;
		if (!degraded)
			m_Errors.Clear();
		return true;
	}

	void SceneRenderer::CreateOverlayResources()
	{
		ShaderLibrary& shaders = Renderer::GetShaderLibrary();
		m_GridPixelShader = shaders.Get("Overlay/Grid.frag");
		m_OutlinePixelShader = shaders.Get("Overlay/Outline.frag");
		m_StatusTintPixelShader = shaders.Get("Overlay/StatusTint.frag");
		m_DebugLineVertexShader = shaders.Get("Overlay/DebugLine.vert");
		m_DebugLinePixelShader = shaders.Get("Overlay/DebugLine.frag");
		m_CopyPixelShader = shaders.Get("PostProcess/Copy.frag");
		ST_CORE_VERIFY(m_GridPixelShader && m_OutlinePixelShader && m_StatusTintPixelShader && m_DebugLineVertexShader && m_DebugLinePixelShader
			&& m_CopyPixelShader, "SceneRenderer overlay shaders are missing");

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

		nvrhi::BindingLayoutDesc statusTintLayout;
		statusTintLayout.visibility = nvrhi::ShaderType::Pixel;
		statusTintLayout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
			nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::StatusTintParameters))
		};
		m_StatusTintBindingLayout = m_Device->createBindingLayout(statusTintLayout);

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
			&& EnsureBufferCapacity(m_TintBuffer, 64, sizeof(glm::uvec2), "EntityTints", recreated)
			&& EnsureBufferCapacity(m_DebugLineBuffer, 1024, sizeof(DebugLineVertex), "DebugLines", recreated, BufferKind::Vertex);
		ST_CORE_VERIFY(buffersCreated && m_GridBindingLayout && m_GridBindingSet && m_OutlineBindingLayout && m_StatusTintBindingLayout && m_DebugLineBindingLayout
			&& m_DebugLineBindingSet && m_DebugLineInputLayout, "SceneRenderer: failed to create overlay resources");
	}

	bool SceneRenderer::HasOverlays(const SceneRenderOptions& options)
	{
		return options.ShowGrid || !options.SelectedEntities.empty() || (options.DebugShapes && !options.DebugShapes->IsEmpty()) || !options.EntityTints.empty();
	}

	bool SceneRenderer::DrawEntityTints(nvrhi::ICommandList* commandList, Scene& scene, std::span<const SceneEntityTint> tints, const nvrhi::Viewport& viewport)
	{
		// Sorted by id for the shader's binary search; the first color of an entity listed twice wins (a stable sort keeps
		// the given order among equal ids).
		m_TintEntries.clear();
		for (const SceneEntityTint& tint : tints)
		{
			if (tint.Target.GetScene() == &scene && tint.Target.IsValid())
				m_TintEntries.push_back(glm::uvec2(static_cast<uint32_t>(tint.Target.GetHandle()) + 1, DebugDraw::PackColor(tint.Color)));
		}
		std::stable_sort(m_TintEntries.begin(), m_TintEntries.end(), [](const glm::uvec2& a, const glm::uvec2& b) { return a.x < b.x; });
		m_TintEntries.erase(std::unique(m_TintEntries.begin(), m_TintEntries.end(), [](const glm::uvec2& a, const glm::uvec2& b) { return a.x == b.x; }),
			m_TintEntries.end());
		if (m_TintEntries.empty())
			return true;

		bool recreated = false;
		if (!EnsureBufferCapacity(m_TintBuffer, m_TintEntries.size(), sizeof(glm::uvec2), "EntityTints", recreated))
			return false;
		if (recreated || !m_StatusTintBindingSet)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::Texture_SRV(0, m_EntityIDTexture),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_TintBuffer),
				nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::StatusTintParameters))
			};
			m_StatusTintBindingSet = m_Device->createBindingSet(desc, m_StatusTintBindingLayout);
			if (!m_StatusTintBindingSet)
			{
				m_Errors.Report("failed to create the entity tint bindings");
				return false;
			}
		}
		commandList->writeBuffer(m_TintBuffer, m_TintEntries.data(), m_TintEntries.size() * sizeof(glm::uvec2));

		RenderData::StatusTintParameters parameters = {};
		parameters.Count = static_cast<uint32_t>(m_TintEntries.size());
		nvrhi::GraphicsState state;
		state.pipeline = m_StatusTintPipeline;
		state.framebuffer = m_OverlayFramebuffer;
		state.viewport.addViewportAndScissorRect(viewport);
		state.bindings = { m_StatusTintBindingSet };
		commandList->setGraphicsState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		m_Stats.TintedEntities = parameters.Count;
		return true;
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

	bool SceneRenderer::RenderOverlays(nvrhi::ICommandList* commandList, Scene& scene, const SceneRenderOptions& options, bool drawText)
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

			desc.PS = m_StatusTintPixelShader;
			desc.bindingLayouts = { m_StatusTintBindingLayout };
			m_StatusTintPipeline = m_Device->createGraphicsPipeline(desc, info);

			desc.primType = nvrhi::PrimitiveType::LineList;
			desc.inputLayout = m_DebugLineInputLayout;
			desc.VS = m_DebugLineVertexShader;
			desc.PS = m_DebugLinePixelShader;
			desc.bindingLayouts = { m_DebugLineBindingLayout };
			m_DebugLinePipelines[static_cast<size_t>(DebugDrawDepth::OnTop)] = m_Device->createGraphicsPipeline(desc, info);
			desc.renderState.depthStencilState.setDepthTestEnable(true);
			m_DebugLinePipelines[static_cast<size_t>(DebugDrawDepth::Tested)] = m_Device->createGraphicsPipeline(desc, info);
			ST_CORE_VERIFY(m_GridPipeline && m_OutlinePipeline && m_StatusTintPipeline && m_DebugLinePipelines[0] && m_DebugLinePipelines[1],
				"SceneRenderer: failed to create the overlay pipelines");
		}
		const nvrhi::Viewport viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y));

		// Tints and the selection outline read the entity-ID buffer: without one (SceneRendererSpecification::EntityIDs) they
		// are left out, and the other overlays drawn.
		const bool entityIDs = m_EntityIDTexture != nullptr;
		bool complete = true;
		if (!entityIDs && (!options.EntityTints.empty() || !options.SelectedEntities.empty()))
		{
			m_Errors.Report("selection outlines and entity tints need the entity-ID buffer, which this renderer does not write");
			complete = false;
		}

		// Tints color the scene itself: the other overlays go over them.
		if (entityIDs && !options.EntityTints.empty() && !DrawEntityTints(commandList, scene, options.EntityTints, viewport))
			return false;

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
			if (!EnsureBufferCapacity(m_DebugLineBuffer, tested.size() + onTop.size(), sizeof(DebugLineVertex), "DebugLines", recreated, BufferKind::Vertex))
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
		if (drawText)
			m_TextRenderer->Draw(commandList, m_OverlayFramebuffer, false);

		// Selection outline from the entity-ID buffer (ids + 1, sorted for the shader's binary search). The sorted list and its
		// upload are kept while the same entities of the same scene stay selected and no entity was created or destroyed
		// (which would change which of them are valid).
		const bool selectionChanged = m_SelectionSceneId != m_RenderSceneId || m_SelectionHierarchyVersion != scene.GetHierarchyVersion()
			|| !std::equal(options.SelectedEntities.begin(), options.SelectedEntities.end(), m_SelectionEntities.begin(), m_SelectionEntities.end());
		if (selectionChanged)
		{
			m_SelectionSceneId = m_RenderSceneId;
			m_SelectionHierarchyVersion = scene.GetHierarchyVersion();
			m_SelectionEntities.assign(options.SelectedEntities.begin(), options.SelectedEntities.end());
			m_SelectionIDs.clear();
			for (const Entity& entity : options.SelectedEntities)
			{
				if (entity.GetScene() == &scene && entity.IsValid())
					m_SelectionIDs.push_back(static_cast<uint32_t>(entity.GetHandle()) + 1);
			}
			std::sort(m_SelectionIDs.begin(), m_SelectionIDs.end());
			m_SelectionIDs.erase(std::unique(m_SelectionIDs.begin(), m_SelectionIDs.end()), m_SelectionIDs.end());
			m_SelectionUploaded = false;
		}
		if (entityIDs && !m_SelectionIDs.empty())
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
					m_Errors.Report("failed to create the selection outline bindings");
					return false;
				}
			}
			if (recreated || !m_SelectionUploaded)
			{
				commandList->writeBuffer(m_SelectionBuffer, m_SelectionIDs.data(), m_SelectionIDs.size() * sizeof(uint32_t));
				m_SelectionUploaded = true;
			}

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
		if (drawText)
			m_TextRenderer->Draw(commandList, m_OverlayFramebuffer, true); // The HUD goes over everything
		return complete;
	}

	Entity SceneRenderer::GetEntityAt(Scene& scene, uint32_t x, uint32_t y)
	{
		Scope<TextureReadback> readback = ReadEntityIDAsync(x, y);
		if (!readback)
			return {};
		readback->Wait();
		ReadbackImage image;
		if (!readback->GetResult(image) || image.BytesPerPixel != sizeof(uint32_t) || image.Pixels.size() < sizeof(uint32_t))
			return {};

		uint32_t id = 0;
		std::memcpy(&id, image.Pixels.data(), sizeof(uint32_t));
		return GetEntityFromID(scene, id);
	}

	Scope<TextureReadback> SceneRenderer::ReadEntityIDAsync(uint32_t x, uint32_t y)
	{
		if (!m_HasRenderedFrame || !m_EntityIDTexture || x >= m_ViewportSize.x || y >= m_ViewportSize.y)
			return nullptr;

		TextureReadbackRegion region;
		region.X = x;
		region.Y = y;
		region.Width = 1;
		region.Height = 1;
		std::string error;
		Scope<TextureReadback> readback = TextureReadback::Create(m_EntityIDTexture, region, &error);
		if (!readback)
			m_Errors.Report(fmt::format("reading the entity ID at ({}, {}) failed: {}", x, y, error));
		return readback;
	}

	Entity SceneRenderer::GetEntityFromID(Scene& scene, uint32_t id)
	{
		// The prepass writes the entity handle + 1, so 0 means that nothing was drawn.
		if (id == 0)
			return {};
		const entt::entity handle = static_cast<entt::entity>(id - 1);
		if (!scene.GetRegistry().valid(handle))
			return {};
		return Entity(handle, &scene);
	}

}
