#include "stpch.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Math/Frustum.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/FontAtlasCache.h"
#include "Strata/Renderer/LODSelection.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/RenderSceneGPU.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Renderer/TextRenderer.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Renderer/TextureStreamer.h"
#include "Strata/Renderer/TextureTable.h"
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
		// Motion vectors in UV units: 11 significant bits keep a motion to about 2^-11 of its length.
		constexpr nvrhi::Format c_VelocityFormat = nvrhi::Format::RG16_FLOAT;
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
		// FilterEnvironmentStep: the cube's levels, the irradiance, then each prefiltered level.
		constexpr uint32_t c_EnvironmentFilterSteps = 2 + c_PrefilteredMipCount;
		static_assert(SceneRenderer::c_AtmosphereRefreshSteps == 6 + c_EnvironmentFilterSteps);
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

		// Temporal anti-aliasing (PostProcess/TAA.comp): the current frame's least weight once the history holds eight frames (a
		// still pixel then follows the mean of the jitter's eight phases, within 13% of their spread whatever their order), and
		// its weight per pixel of motion, up to the largest, which keeps moving surfaces sharp at the cost of anti-aliasing.
		constexpr float c_TemporalMinWeight = 1.0f / 8.0f;
		constexpr float c_TemporalMotionWeightScale = 1.0f / 32.0f;
		constexpr float c_TemporalMaxMotionWeight = 0.5f;

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
		m_AtmosphereSkyboxPixelShader = shaders.Get("Scene/SkyboxAtmosphere.frag");
		m_ShadowVertexShader = shaders.Get("Scene/ShadowMesh.vert");
		m_ShadowMaskPixelShader = shaders.Get("Scene/ShadowMask.frag");
		m_FXAAPixelShader = shaders.Get("PostProcess/FXAA.frag");
		m_SelectionMaskPixelShader = shaders.Get("Scene/SelectionMask.frag");
		m_LocalShadowVertexShader = shaders.Get("Scene/LocalShadowMesh.vert");
		m_SkinnedLocalShadowVertexShader = shaders.Get("Scene/LocalShadowMeshSkinned.vert");
		ST_CORE_VERIFY(m_MeshVertexShader && m_PrepassPixelShader && m_ForwardPixelShader && m_ForwardTransparentPixelShader && m_FullscreenVertexShader
			&& m_TonemapPixelShader && m_SkyboxPixelShader && m_AtmosphereSkyboxPixelShader && m_ShadowVertexShader && m_ShadowMaskPixelShader && m_FXAAPixelShader && m_SkinnedMeshVertexShader
			&& m_SkinnedShadowVertexShader && m_SelectionMaskPixelShader && m_LocalShadowVertexShader && m_SkinnedLocalShadowVertexShader,
			"SceneRenderer shaders are missing");

		m_Specification.ShadowCascades = std::clamp<uint32_t>(m_Specification.ShadowCascades, 1, RenderData::c_MaxCascades);
		m_Specification.ShadowMapSize = std::clamp<uint32_t>(m_Specification.ShadowMapSize, 256, 8192);
		m_Specification.MaxLights = std::max(m_Specification.MaxLights, 1u);
		// The local shadows' atlas: a power of two within its range; at most the shaders' slots of shadowed lights.
		m_Specification.LocalShadowAtlasSize = ShadowAtlasAllocator::FloorPowerOfTwo(std::clamp<uint32_t>(m_Specification.LocalShadowAtlasSize, 512, 8192));
		m_Specification.MaxShadowedLocalLights = std::min(m_Specification.MaxShadowedLocalLights, RenderData::c_MaxShadowedLocalLights);
		m_Specification.LocalShadowViewsPerFrame = std::max(m_Specification.LocalShadowViewsPerFrame, 1u);
		m_LocalShadowAllocator.Reset(m_Specification.LocalShadowAtlasSize, 128);
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
			nvrhi::BindingLayoutItem::Texture_SRV(8),            // The local shadows' atlas
			nvrhi::BindingLayoutItem::Texture_SRV(9),            // The reflection probes' cube array
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(10),  // The reflection probes (RenderData::ReflectionProbeBuffer)
			nvrhi::BindingLayoutItem::Texture_SRV(11),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(12),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(13),  // The shadowed point and spot lights (RenderData::LocalShadowData)
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(14),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(15), // Joint matrices (only the skinned vertex shaders read them)
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(16), // The texture table (material data names textures by its indices)
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(17), // The shadow cascades (Scene/CascadeFit.comp)
			// The motion history's instances and joint matrices (SceneRenderOptions::MotionVectors): the previous frame's.
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(20),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(21),
			nvrhi::BindingLayoutItem::Texture_SRV(27),          // The atmosphere's sky-view table (Include/Atmosphere.glsl)
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
		nvrhi::TextureDesc dummyAtlasDesc = dummyShadowDesc;
		dummyAtlasDesc.arraySize = 1;
		dummyAtlasDesc.dimension = nvrhi::TextureDimension::Texture2D;
		dummyAtlasDesc.debugName = m_Specification.DebugName + ".NoLocalShadows";
		m_DummyLocalShadowAtlas = m_Device->createTexture(dummyAtlasDesc);
		m_LocalShadowBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(RenderData::c_MaxShadowedLocalLights * sizeof(RenderData::LocalShadowData))
			.setStructStride(sizeof(RenderData::LocalShadowData))
			.setInitialState(nvrhi::ResourceStates::ShaderResource)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".LocalShadows"));
		// Fixed size: the slots' irradiance (written on the GPU) and the frame's list (zero probes until a frame has some).
		m_ReflectionProbeBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(sizeof(RenderData::ReflectionProbeBuffer))
			.setStructStride(sizeof(glm::vec4))
			.setCanHaveUAVs(true)
			.setInitialState(nvrhi::ResourceStates::ShaderResource)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".ReflectionProbes"));

		m_FrameConstantBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(sizeof(RenderData::FrameConstants))
			.setIsConstantBuffer(true)
			.setIsVolatile(true)
			.setMaxVersions(16)
			.setDebugName(m_Specification.DebugName + ".FrameConstants"));

		bool recreated = false;
		const bool buffersCreated = EnsureBufferCapacity(m_DrawListBuffer, 1024, sizeof(uint32_t), "DrawList", recreated)
			&& EnsureBufferCapacity(m_LightBuffer, 64, sizeof(RenderData::LightData), "Lights", recreated, BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_LightBoundsBuffer, 64, sizeof(RenderData::LightBounds), "LightBounds", recreated, BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_LightCountBuffer, 1, sizeof(uint32_t), "LightCount", recreated, BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_ShadowDrawListBuffer, 1024, sizeof(uint32_t), "ShadowDrawList", recreated);
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
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(21),
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

		// The shadow cascades, fitted on the GPU (both paths), and what the fit reads.
		m_CascadeBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(RenderData::c_MaxCascades * sizeof(RenderData::CascadeData))
			.setStructStride(sizeof(RenderData::CascadeData))
			.setCanHaveUAVs(true)
			.setInitialState(nvrhi::ResourceStates::ShaderResource)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".Cascades"));
		m_CasterDepthBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(RenderData::c_MaxCascades * sizeof(uint32_t))
			.setStructStride(sizeof(uint32_t))
			.setCanHaveUAVs(true)
			.setInitialState(nvrhi::ResourceStates::ShaderResource)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".CasterDepths"));
		m_CascadeFitConstantBuffer = m_Device->createBuffer(nvrhi::BufferDesc()
			.setByteSize(sizeof(RenderData::CascadeFitConstants))
			.setIsConstantBuffer(true)
			.setInitialState(nvrhi::ResourceStates::ConstantBuffer)
			.setKeepInitialState(true)
			.setDebugName(m_Specification.DebugName + ".CascadeFit"));
		nvrhi::BindingLayoutDesc fitLayout;
		fitLayout.visibility = nvrhi::ShaderType::Compute;
		fitLayout.bindings = {
			nvrhi::BindingLayoutItem::ConstantBuffer(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
			nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1)
		};
		m_CascadeFitBindingLayout = m_Device->createBindingLayout(fitLayout);
		m_CascadeFitPipeline = createComputePipeline("Scene/CascadeFit.comp", m_CascadeFitBindingLayout);
		nvrhi::BindingSetDesc fitSet;
		fitSet.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_CascadeFitConstantBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_CasterDepthBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_CascadeBuffer)
		};
		m_CascadeFitBindingSet = m_Device->createBindingSet(fitSet, m_CascadeFitBindingLayout);
		ST_CORE_VERIFY(m_CascadeBuffer && m_CasterDepthBuffer && m_CascadeFitConstantBuffer && m_CascadeFitPipeline && m_CascadeFitBindingSet,
			"SceneRenderer: failed to create the shadow cascades' resources");
		CreateCullResources();
		CreateLightCullResources();

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
		// The automatic exposure's results come back to choose the pre-exposure: two floats (Include/Exposure.glsl).
		for (ExposureReadback& entry : m_ExposureReadbacks)
			entry.Readback = CreateScope<BufferReadback>(m_Device, 2 * sizeof(float), m_Specification.DebugName + ".ExposureReadback");
		const bool exposureReadbacks = std::all_of(m_ExposureReadbacks.begin(), m_ExposureReadbacks.end(),
			[](const ExposureReadback& entry) { return entry.Readback->IsValid(); });

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

		// Temporal anti-aliasing's resolve: the frame constants and the linear clamp sampler at the scene set's slots, its images
		// above them (PostProcess/TAA.comp).
		nvrhi::BindingLayoutDesc temporalLayout;
		temporalLayout.visibility = nvrhi::ShaderType::Compute;
		temporalLayout.bindings = {
			nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
			nvrhi::BindingLayoutItem::Texture_SRV(22),
			nvrhi::BindingLayoutItem::Texture_SRV(23),
			nvrhi::BindingLayoutItem::Texture_SRV(24),
			nvrhi::BindingLayoutItem::Texture_SRV(25),
			nvrhi::BindingLayoutItem::StructuredBuffer_SRV(26),
			nvrhi::BindingLayoutItem::Sampler(8),
			nvrhi::BindingLayoutItem::Texture_UAV(0),
			nvrhi::BindingLayoutItem::PushConstants(1, sizeof(RenderData::TemporalParameters))
		};
		m_TemporalBindingLayout = m_Device->createBindingLayout(temporalLayout);
		m_TemporalPipeline = createComputePipeline("PostProcess/TAA.comp", m_TemporalBindingLayout);

		nvrhi::BindingLayoutDesc fxaaLayout;
		fxaaLayout.visibility = nvrhi::ShaderType::Pixel;
		fxaaLayout.bindings = { nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0) };
		m_FXAABindingLayout = m_Device->createBindingLayout(fxaaLayout);

		ST_CORE_VERIFY(m_InputLayout && m_SceneBindingLayout && m_TonemapBindingLayout && m_FrameConstantBuffer && m_CommandList && m_ShadowPushBindingSet
			&& m_PointClampSampler && m_LinearClampSampler && m_EquirectSampler && m_ShadowCompareSampler && m_DummyShadowMap && m_DummyLocalShadowAtlas
			&& m_LocalShadowBuffer && m_ReflectionProbeBuffer && m_ClusterBuffer
			&& m_ClusterBindingLayout && m_ClusterPipeline && m_AOBindingLayout && m_GTAOPipeline && m_AOBlurPipeline && m_ExposureBindingLayout
			&& m_HistogramPipeline && m_ExposurePipeline && m_HistogramBuffer && m_ExposureBuffer && exposureReadbacks && m_BloomBindingLayout && m_BloomPipeline
			&& m_FXAABindingLayout
			&& m_TemporalBindingLayout && m_TemporalPipeline, "SceneRenderer: failed to create GPU resources");
		CreateIBLResources();
		CreateOverlayResources();
		m_FontAtlases = CreateScope<FontAtlasCache>();
		m_TextRenderer = CreateScope<TextRenderer>(m_Specification.DebugName, m_FrameConstantBuffer, *m_FontAtlases);
		// Modules above the renderer (the game UI) draw their screen-space overlays through passes they register.
		for (const SceneOverlayRegistry::Entry& entry : SceneOverlayRegistry::GetAll())
		{
			if (Scope<ISceneOverlayPass> pass = entry.Factory(SceneOverlayPassContext { m_Specification.DebugName }))
				m_OverlayPasses.push_back(std::move(pass));
		}
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
		// No probe in the list (and no slot's irradiance) until a frame has some.
		const RenderData::ReflectionProbeBuffer noProbes = {};
		m_CommandList->writeBuffer(m_ReflectionProbeBuffer, &noProbes, sizeof(noProbes));
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
		for (uint32_t step = 0; step < c_EnvironmentFilterSteps; step++)
			FilterEnvironmentStep(commandList, maps, step);
	}

	void SceneRenderer::FilterEnvironmentStep(nvrhi::ICommandList* commandList, const EnvironmentMaps& maps, uint32_t step)
	{
		const uint32_t cubeMips = MipCountFor(c_EnvironmentCubeSize);
		if (step == 0)
		{
			for (uint32_t mip = 1; mip < cubeMips; mip++)
			{
				const uint32_t size = std::max(1u, c_EnvironmentCubeSize >> mip);
				DispatchIBL(commandList, m_CubeDownsamplePipeline, maps.Cube, nvrhi::TextureSubresourceSet(mip - 1, 1, 0, 6), nvrhi::TextureDimension::Texture2DArray,
					maps.Cube, mip, nvrhi::TextureDimension::Texture2DArray, RenderData::IBLParameters { size, size * 2, 0.0f, 1 }, 6);
			}
			return;
		}

		const nvrhi::TextureSubresourceSet wholeCube(0, cubeMips, 0, 6);
		if (step == 1)
		{
			DispatchIBL(commandList, m_IrradiancePipeline, maps.Cube, wholeCube, nvrhi::TextureDimension::TextureCube, maps.Irradiance, 0,
				nvrhi::TextureDimension::Texture2DArray, RenderData::IBLParameters { c_IrradianceSize, c_EnvironmentCubeSize, 0.0f, c_IrradianceSamples }, 6);
			return;
		}
		const uint32_t mip = step - 2;
		ST_CORE_ASSERT(mip < c_PrefilteredMipCount, "Environment filter step {} out of range", step);
		const uint32_t size = std::max(1u, c_PrefilteredSize >> mip);
		const float roughness = static_cast<float>(mip) / static_cast<float>(c_PrefilteredMipCount - 1);
		DispatchIBL(commandList, m_PrefilterPipeline, maps.Cube, wholeCube, nvrhi::TextureDimension::TextureCube, maps.Prefiltered, mip,
			nvrhi::TextureDimension::Texture2DArray, RenderData::IBLParameters { size, c_EnvironmentCubeSize, roughness, c_PrefilterSamples }, 6);
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
		frame.SunDiskDirection = glm::vec4(0.0f);
		frame.SunDiskColor = glm::vec4(0.0f);
		m_SkyHorizon.reset();
		m_AtmosphereFrame = false;

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
		const bool atmosphere = skyLight && skyLight->Source == SkyLightSource::Atmosphere;
		// The atmosphere's tables and its second set of maps exist only while the sky is the atmosphere.
		if (!atmosphere)
			m_Atmosphere.reset();
		if (!skyLight || (!procedural && !atmosphere && !skyLight->EnvironmentMap.IsValid()))
		{
			m_Environment = nullptr; // Release the maps of a removed environment
			m_EnvironmentHold = nullptr;
			return;
		}

		float rotation = 0.0f; // The procedural sky and the atmosphere are computed in world space: their sun follows the light
		float intensity = std::max(skyLight->Intensity, 0.0f);
		if (atmosphere)
		{
			m_EnvironmentHold = nullptr;
			if (!UpdateAtmosphere(*skyLight, frame, commandList))
			{
				m_Environment = nullptr;
				m_Atmosphere.reset();
				return;
			}
			m_AtmosphereFrame = true;
			// The tables and the environment hold the sky per lux of sun: times the light's illuminance above the atmosphere.
			intensity *= m_AtmosphereSunIlluminance;
		}
		else if (procedural)
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

		frame.EnvironmentParams = glm::vec4(std::isfinite(intensity) ? intensity : 0.0f, rotation, static_cast<float>(c_PrefilteredMipCount - 1), 1.0f);
		if (procedural)
			m_SkyHorizon = glm::vec3(SanitizeRadiance(skyLight->HorizonColor * frame.EnvironmentParams.x));
		const float backgroundMips = static_cast<float>(MipCountFor(c_EnvironmentCubeSize) - 1);
		// The atmosphere's background is drawn from its sky-view table, sharp.
		const float blur = atmosphere ? 0.0f : std::clamp(skyLight->BackgroundBlur, 0.0f, 1.0f);
		frame.SkyParams = glm::vec4(blur * backgroundMips, skyLight->ShowBackground ? 1.0f : 0.0f, 0.0f, 0.0f);
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
		nvrhi::ITexture* localShadowAtlas = m_LocalShadowAtlas ? m_LocalShadowAtlas.Get() : m_DummyLocalShadowAtlas.Get();
		// Without a motion history the scene's own buffers stand in (nothing reads them then).
		nvrhi::IBuffer* previousInstances = m_PreviousInstances ? m_PreviousInstances.Get() : m_SceneGPU->GetInstanceBuffer();
		nvrhi::IBuffer* previousPalette = m_PreviousPalette ? m_PreviousPalette.Get() : m_SceneGPU->GetPaletteBuffer();
		nvrhi::ITexture* skyView = GetBoundSkyView();
		nvrhi::ITexture* probeArray = GetBoundReflectionProbes();

		// The shadow pass reads the same resources except for its own draw list (and never the shadow maps it draws into).
		auto create = [&](nvrhi::IBuffer* drawList, nvrhi::ITexture* boundShadowMap, nvrhi::ITexture* boundAtlas)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_SceneGPU->GetInstanceBuffer()),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(14, drawList),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(15, m_SceneGPU->GetPaletteBuffer()),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(16, Renderer::GetTextureTable().GetBuffer()),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(17, m_CascadeBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(20, previousInstances),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(21, previousPalette),
				nvrhi::BindingSetItem::Texture_SRV(27, skyView),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_SceneGPU->GetMaterialBuffer()),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_LightBuffer),
				nvrhi::BindingSetItem::Texture_SRV(3, hasEnvironment ? m_Environment->Irradiance.Get() : blackCube),
				nvrhi::BindingSetItem::Texture_SRV(4, hasEnvironment ? m_Environment->Prefiltered.Get() : blackCube),
				nvrhi::BindingSetItem::Texture_SRV(5, m_BRDFLut),
				nvrhi::BindingSetItem::Texture_SRV(6, hasEnvironment ? environmentCube : blackCube),
				nvrhi::BindingSetItem::Texture_SRV(7, boundShadowMap),
				nvrhi::BindingSetItem::Texture_SRV(8, boundAtlas),
				nvrhi::BindingSetItem::Texture_SRV(9, probeArray, nvrhi::Format::UNKNOWN, nvrhi::AllSubresources, nvrhi::TextureDimension::TextureCubeArray),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(10, m_ReflectionProbeBuffer),
				nvrhi::BindingSetItem::Texture_SRV(11, m_AOTexture),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(12, m_ClusterBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_LocalShadowBuffer),
				nvrhi::BindingSetItem::Sampler(8, m_LinearClampSampler),
				nvrhi::BindingSetItem::Sampler(9, m_ShadowCompareSampler),
				nvrhi::BindingSetItem::Sampler(10, m_PointClampSampler)
			};
			for (uint32_t index = 0; index < RenderData::c_SamplerCount; index++)
				desc.bindings.push_back(nvrhi::BindingSetItem::Sampler(0, m_MaterialSamplers[index]).setArrayElement(index));
			return m_Device->createBindingSet(desc, m_SceneBindingLayout);
		};
		m_SceneBindingSet = create(m_DrawListBuffer, shadowMap, localShadowAtlas);
		m_ShadowBindingSet = create(m_ShadowDrawListBuffer, m_DummyShadowMap, m_DummyLocalShadowAtlas);
		// The GPU-driven path's passes read the draw list its culling writes.
		const bool gpuSets = m_GPUDrawListBuffer != nullptr;
		m_GPUSceneBindingSet = gpuSets ? create(m_GPUDrawListBuffer, shadowMap, localShadowAtlas) : nullptr;
		m_GPUShadowBindingSet = gpuSets ? create(m_GPUDrawListBuffer, m_DummyShadowMap, m_DummyLocalShadowAtlas) : nullptr;

		nvrhi::BindingSetDesc clusterSet;
		clusterSet.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_LightBoundsBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_SRV(21, m_LightCountBuffer),
			nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_ClusterBuffer)
		};
		m_ClusterBindingSet = m_Device->createBindingSet(clusterSet, m_ClusterBindingLayout);

		m_BoundEnvironment = environmentCube;
		m_BoundShadowMap = shadowMap;
		m_BoundLocalShadowAtlas = localShadowAtlas;
		m_BoundSceneGPUVersion = m_SceneGPU->GetBufferVersion();
		m_BoundTextureTableVersion = Renderer::GetTextureTable().GetBufferVersion();
		m_BoundPreviousInstances = previousInstances;
		m_BoundPreviousPalette = previousPalette;
		m_BoundSkyView = skyView;
		m_BoundReflectionProbes = probeArray;
		if (!m_SceneBindingSet || !m_ShadowBindingSet || !m_ClusterBindingSet || (gpuSets && (!m_GPUSceneBindingSet || !m_GPUShadowBindingSet)))
		{
			m_SceneBindingSet = nullptr; // Recreated next frame
			return false;
		}
		return true;
	}

	nvrhi::ITexture* SceneRenderer::GetBoundReflectionProbes() const
	{
		return Renderer::GetBlackCubeArrayTexture();
	}

	bool SceneRenderer::EnsureMotionResources(bool keepContents)
	{
		if (!m_MotionCopyPipeline)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.bindings = {
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
				nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
				nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),
				nvrhi::BindingLayoutItem::PushConstants(0, sizeof(RenderData::MotionCopyParameters))
			};
			m_MotionCopyLayout = m_Device->createBindingLayout(layout);
			nvrhi::ComputePipelineDesc desc;
			desc.CS = Renderer::GetShaderLibrary().Get("Scene/MotionCopy.comp");
			desc.bindingLayouts = { m_MotionCopyLayout };
			ST_CORE_VERIFY(desc.CS && m_MotionCopyLayout, "SceneRenderer: the motion copy shader is missing");
			m_MotionCopyPipeline = m_Device->createComputePipeline(desc);
			ST_CORE_VERIFY(m_MotionCopyPipeline, "SceneRenderer: failed to create the motion copy pipeline");
			ShaderLibrary& shaders = Renderer::GetShaderLibrary();
			m_MeshVelocityVertexShader = shaders.Get("Scene/MeshVelocity.vert");
			m_SkinnedMeshVelocityVertexShader = shaders.Get("Scene/MeshSkinnedVelocity.vert");
			m_PrepassVelocityPixelShader = shaders.Get(m_Specification.EntityIDs ? "Scene/DepthPrepassVelocity.frag" : "Scene/DepthPrepassNoEntityIDsVelocity.frag");
			ST_CORE_VERIFY(m_MeshVelocityVertexShader && m_SkinnedMeshVelocityVertexShader && m_PrepassVelocityPixelShader,
				"SceneRenderer: the velocity shaders are missing");
		}
		if (!m_VelocityTexture)
		{
			m_VelocityTexture = CreateRenderTarget(m_Device, m_ViewportSize.x, m_ViewportSize.y, c_VelocityFormat, m_Specification.DebugName + ".Velocity", false);
			if (m_VelocityTexture)
			{
				nvrhi::FramebufferDesc desc;
				desc.addColorAttachment(m_NormalTexture);
				if (m_EntityIDTexture)
					desc.addColorAttachment(m_EntityIDTexture);
				desc.addColorAttachment(m_VelocityTexture);
				desc.setDepthAttachment(m_DepthTexture);
				m_MotionPrepassFramebuffer = m_Device->createFramebuffer(desc);
			}
			if (!m_VelocityTexture || !m_MotionPrepassFramebuffer)
			{
				m_VelocityTexture = nullptr;
				m_MotionPrepassFramebuffer = nullptr;
				m_Errors.Report(fmt::format("failed to create the velocity target for a {}x{} viewport; rendering without motion", m_ViewportSize.x, m_ViewportSize.y));
				return false;
			}
		}
		if (!keepContents)
		{
			m_GrownPreviousInstances = nullptr;
			m_GrownPreviousPalette = nullptr;
		}
		const auto ensure = [&](nvrhi::BufferHandle& buffer, nvrhi::BufferHandle& grown, uint64_t capacity, uint32_t elementSize, const char* name)
		{
			const uint64_t bytes = std::max<uint64_t>(capacity, 1) * elementSize;
			if (buffer && buffer->getDesc().byteSize >= bytes)
				return true;
			nvrhi::BufferDesc desc;
			desc.byteSize = bytes;
			desc.structStride = elementSize;
			desc.canHaveUAVs = true;
			desc.debugName = m_Specification.DebugName + "." + name;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			nvrhi::BufferHandle created = m_Device->createBuffer(desc);
			if (!created)
			{
				m_Errors.Report(fmt::format("failed to allocate the motion history's {} buffer ({} bytes); rendering without motion", name, bytes));
				return false;
			}
			// A history the frame compares with moves over at its start (RecordMotionGrowth); frames in flight may read the old one.
			if (keepContents && buffer)
				grown = buffer;
			else if (buffer)
				Renderer::ReleaseDeferred({ nvrhi::ResourceHandle(buffer.Get()) });
			buffer = std::move(created);
			return true;
		};
		return ensure(m_PreviousInstances, m_GrownPreviousInstances, m_SceneGPU->GetInstanceCapacity(), sizeof(RenderData::InstanceData), "PreviousInstances")
			&& ensure(m_PreviousPalette, m_GrownPreviousPalette, m_SceneGPU->GetPaletteCapacity(), sizeof(RenderData::AffineTransform), "PreviousPalette");
	}

	void SceneRenderer::RecordMotionGrowth(nvrhi::ICommandList* commandList)
	{
		const auto grow = [commandList](nvrhi::BufferHandle& grown, nvrhi::IBuffer* buffer)
		{
			if (!grown)
				return;
			// The new space holds zeros, no instance's entity (0 is none): what is drawn there first moves with the camera only.
			// NVRHI places no barrier between transfers into a buffer that stays a copy destination, so the clear and the copy
			// each end with a transition of their own (the frame's copy at its end may write the whole buffer again, with no pass
			// reading it in between when nothing is drawn).
			const auto barrier = [commandList, buffer]()
			{
				commandList->setBufferState(buffer, nvrhi::ResourceStates::ShaderResource);
				commandList->commitBarriers();
			};
			commandList->clearBufferUInt(buffer, 0);
			barrier();
			commandList->copyBuffer(buffer, 0, grown, 0, grown->getDesc().byteSize);
			barrier();
			Renderer::ReleaseDeferred({ nvrhi::ResourceHandle(grown.Get()) });
			grown = nullptr;
		};
		grow(m_GrownPreviousInstances, m_PreviousInstances);
		grow(m_GrownPreviousPalette, m_PreviousPalette);
	}

	void SceneRenderer::RecordMotionCopy(nvrhi::ICommandList* commandList, const RenderScene& renderScene, bool full)
	{
		// Per buffer: copied whole, or the elements written since the history's version, listed (each once, in order).
		struct Part
		{
			nvrhi::IBuffer* Source = nullptr;
			nvrhi::IBuffer* Destination = nullptr;
			uint64_t Capacity = 0;
			uint32_t ElementSize = 0;
			bool Full = false;
			uint32_t ListOffset = 0;
			uint32_t Count = 0;
		};
		std::array<Part, 2> parts;
		parts[0] = Part { m_SceneGPU->GetInstanceBuffer(), m_PreviousInstances, m_SceneGPU->GetInstanceCapacity(), sizeof(RenderData::InstanceData) };
		parts[1] = Part { m_SceneGPU->GetPaletteBuffer(), m_PreviousPalette, m_SceneGPU->GetPaletteCapacity(), sizeof(RenderData::AffineTransform) };
		m_MotionCopyList.clear();
		for (size_t index = 0; index < parts.size(); index++)
		{
			Part& part = parts[index];
			m_MotionRanges.clear();
			const bool logged = !full
				&& (index == 0 ? m_SceneGPU->GetInstanceWrites(m_MotionSceneId, m_MotionWriteVersion, m_MotionRanges)
					: m_SceneGPU->GetPaletteWrites(m_MotionSceneId, m_MotionWriteVersion, m_MotionRanges));
			if (!logged)
			{
				part.Full = true;
				continue;
			}
			part.ListOffset = static_cast<uint32_t>(m_MotionCopyList.size());
			std::sort(m_MotionRanges.begin(), m_MotionRanges.end());
			uint64_t covered = 0; // Elements before it are listed
			for (const auto& [first, count] : m_MotionRanges)
			{
				const uint64_t end = std::min<uint64_t>(static_cast<uint64_t>(first) + count, part.Capacity);
				for (uint64_t element = std::max<uint64_t>(first, covered); element < end; element++)
					m_MotionCopyList.push_back(static_cast<uint32_t>(element));
				covered = std::max(covered, end);
			}
			part.Count = static_cast<uint32_t>(m_MotionCopyList.size() - part.ListOffset);
			// A list of half the buffer copies no faster than the whole buffer.
			if (static_cast<uint64_t>(part.Count) * 2 > part.Capacity)
			{
				m_MotionCopyList.resize(part.ListOffset);
				part.Count = 0;
				part.Full = true;
			}
		}

		bool listReady = m_MotionCopyList.empty();
		if (!listReady)
		{
			bool listRecreated = false;
			listReady = EnsureBufferCapacity(m_MotionCopyListBuffer, m_MotionCopyList.size(), sizeof(uint32_t), "MotionCopyList", listRecreated);
			if (listReady)
				commandList->writeBuffer(m_MotionCopyListBuffer, m_MotionCopyList.data(), m_MotionCopyList.size() * sizeof(uint32_t));
		}
		for (size_t index = 0; index < parts.size(); index++)
		{
			Part& part = parts[index];
			if (!part.Full && part.Count > 0)
			{
				const std::array<const nvrhi::IBuffer*, 3> bound = { part.Source, m_MotionCopyListBuffer.Get(), part.Destination };
				if (listReady && (!m_MotionCopySets[index] || m_MotionCopyBound[index] != bound))
				{
					nvrhi::BindingSetDesc desc;
					desc.bindings = {
						nvrhi::BindingSetItem::StructuredBuffer_SRV(0, part.Source),
						nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_MotionCopyListBuffer),
						nvrhi::BindingSetItem::StructuredBuffer_UAV(0, part.Destination),
						nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::MotionCopyParameters))
					};
					m_MotionCopySets[index] = m_Device->createBindingSet(desc, m_MotionCopyLayout);
					m_MotionCopyBound[index] = m_MotionCopySets[index] ? bound : std::array<const nvrhi::IBuffer*, 3> {};
				}
				// Without its list or binding set the buffer is copied whole.
				part.Full = !listReady || !m_MotionCopySets[index];
			}
			if (part.Full)
			{
				const uint64_t bytes = std::min(part.Source->getDesc().byteSize, part.Destination->getDesc().byteSize);
				commandList->copyBuffer(part.Destination, 0, part.Source, 0, bytes);
				part.Count = static_cast<uint32_t>(bytes / part.ElementSize);
				continue;
			}
			if (part.Count == 0)
				continue;
			// At most 65535 groups of 64 elements per dispatch.
			constexpr uint32_t c_GroupSize = 64;
			constexpr uint32_t c_MaxEntriesPerDispatch = 65535 * c_GroupSize;
			nvrhi::ComputeState state;
			state.pipeline = m_MotionCopyPipeline;
			state.bindings = { m_MotionCopySets[index] };
			commandList->setComputeState(state);
			for (uint32_t first = 0; first < part.Count; first += c_MaxEntriesPerDispatch)
			{
				const RenderData::MotionCopyParameters parameters = { first, part.Count, part.ElementSize / 4, part.ListOffset };
				commandList->setPushConstants(&parameters, sizeof(parameters));
				commandList->dispatch((std::min(part.Count - first, c_MaxEntriesPerDispatch) + c_GroupSize - 1) / c_GroupSize, 1, 1);
			}
		}
		m_Stats.MotionFullCopy = parts[0].Full || parts[1].Full;
		m_Stats.MotionCopiedInstances = parts[0].Count;
		m_Stats.MotionCopiedJoints = parts[1].Count;
		m_MotionSceneId = renderScene.GetId();
		m_MotionWriteVersion = m_SceneGPU->GetWriteVersion();
		m_MotionRender = m_RenderCount + 1;
	}

	const HeightFogComponent* SceneRenderer::FindHeightFog(Scene& scene)
	{
		for (auto [entity, fog, world] : scene.GetRegistry().view<HeightFogComponent, WorldTransformComponent>().each())
		{
			if (world.ActiveInHierarchy)
				return &fog;
		}
		return nullptr;
	}

	void SceneRenderer::SetHeightFog(Scene& scene, const SceneCamera& camera, RenderData::FrameConstants& frame)
	{
		frame.FogDensity = glm::vec4(0.0f);
		frame.FogColor = glm::vec4(0.0f);
		frame.FogLight = glm::vec4(0.0f);
		frame.FogParams = glm::vec4(0.0f);
		const HeightFogComponent* fog = FindHeightFog(scene);
		if (!fog)
			return;

		// Values the shaders may use (files and commands keep them in their ranges; this keeps them finite whatever wrote them).
		const auto finiteOr = [](float value, float fallback) { return std::isfinite(value) ? value : fallback; };
		const float density = std::max(finiteOr(fog->Density, 0.0f), 0.0f);
		const float falloff = std::max(finiteOr(fog->HeightFalloff, HeightFogComponent::c_MinHeightFalloff), HeightFogComponent::c_MinHeightFalloff);
		const float startDistance = std::max(finiteOr(fog->StartDistance, 0.0f), 0.0f);
		// The camera's height above the base height, subtracted in double so a camera far from the base keeps its precision.
		constexpr double c_MaxHeight = 1.0e9;
		const double height = static_cast<double>(camera.Position.y) - static_cast<double>(finiteOr(fog->BaseHeight, 0.0f));
		frame.FogDensity = glm::vec4(density, falloff, static_cast<float>(std::isfinite(height) ? std::clamp(height, -c_MaxHeight, c_MaxHeight) : 0.0), startDistance);
		frame.FogColor = glm::vec4(glm::vec3(SanitizeRadiance(fog->Color)), std::clamp(finiteOr(fog->MaxOpacity, 1.0f), 0.0f, 1.0f));
		// The light it scatters besides the sun's. The procedural sky's horizon, the haze its gradient ends in: fog that hides
		// the distance then meets the sky at the horizon without a seam (the irradiance from above, mostly the zenith's color,
		// made a darker band there). An environment map's irradiance from above, read on the GPU. Without an environment, what
		// lights the scene's surfaces then (the ambient color). Without ColorFromSky, Color itself is the fog's radiance.
		if (!fog->ColorFromSky)
			frame.FogLight = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
		else if (m_AtmosphereFrame)
			frame.FogLight = glm::vec4(0.0f, 0.0f, 0.0f, 2.0f); // Its sky-view table at the horizon, read on the GPU
		else if (m_SkyHorizon)
			frame.FogLight = glm::vec4(*m_SkyHorizon, 0.0f);
		else if (frame.EnvironmentParams.w > 0.0f)
			frame.FogLight = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
		else
			frame.FogLight = SanitizeRadiance(glm::vec3(frame.AmbientColor));
		const float sunScattering = std::clamp(finiteOr(fog->SunScattering, 0.0f), 0.0f, 1.0f);
		frame.FogParams = glm::vec4(1.0f, sunScattering * HeightFogComponent::c_MaxSunAsymmetry, 0.0f, 0.0f);
		m_Stats.HeightFog = true;
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

	void SceneRenderer::UpdateExposure(nvrhi::ICommandList* commandList, const PostProcessComponent& settings, nvrhi::IBindingSet* bindingSet)
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
		parameters.InversePreExposure = 1.0f / m_PreExposure;
		m_ResetExposure = false;

		commandList->clearBufferUInt(m_HistogramBuffer, 0);
		nvrhi::ComputeState state;
		state.pipeline = m_HistogramPipeline;
		state.bindings = { bindingSet };
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

		// The metered exposure comes back frames later to choose the pre-exposure (ChoosePreExposure). A readback whose earlier
		// copy is still under way is left to finish: the frame's is skipped.
		ExposureReadback& readback = m_ExposureReadbacks[m_ExposureReadbackIndex];
		if (readback.Readback->IsPending() && !readback.Readback->IsReady())
			return;
		readback.Readback->Record(commandList, m_ExposureBuffer, 0, 2 * sizeof(float));
		readback.Render = m_RenderCount + 1;
		readback.Epoch = m_PreExposureEpoch;
		readback.PreExposure = m_PreExposure;
		m_ExposureReadbackRecorded = true;
	}

	void SceneRenderer::SubmitExposureReadback()
	{
		if (!m_ExposureReadbackRecorded)
			return;
		m_ExposureReadbacks[m_ExposureReadbackIndex].Readback->Submit();
		m_ExposureReadbackIndex = (m_ExposureReadbackIndex + 1) % c_GPURecordCount;
		m_ExposureReadbackRecorded = false;
	}

	float SceneRenderer::GetPreExposureFor(float stops)
	{
		if (!std::isfinite(stops))
			return 1.0f;
		const float rounded = std::round(std::clamp(stops, static_cast<float>(-c_MaxPreExposureStops), static_cast<float>(c_MaxPreExposureStops)));
		return std::ldexp(1.0f, static_cast<int>(rounded));
	}

	void SceneRenderer::ChoosePreExposure(const PostProcessComponent& settings, const SceneRenderOptions& options)
	{
		if (m_ResetPreExposure)
		{
			m_ResetPreExposure = false;
			m_PreExposure = 1.0f;
			m_PreExposureSettled = false;
			m_PreExposureEpoch++;
		}
		const bool automatic = !options.PreExposure && settings.AutoExposure;
		if (!automatic)
		{
			// Readbacks of the automatic exposure before this stretch say nothing about the frames after it.
			if (m_PreExposureAutomatic)
				m_PreExposureEpoch++;
			m_PreExposureAutomatic = false;
			if (options.PreExposure)
				m_PreExposure = *options.PreExposure > 0.0f ? GetPreExposureFor(std::log2(*options.PreExposure)) : 1.0f;
			else
				m_PreExposure = GetPreExposureFor(std::clamp(settings.Exposure, -20.0f, 20.0f)); // The power of two nearest 2^Exposure
			m_PreExposureSettled = true;
			return;
		}
		if (!m_PreExposureAutomatic)
		{
			m_PreExposureAutomatic = true;
			m_PreExposureSettled = false; // Until a readback of this stretch arrives
		}

		// The newest readback of this epoch that completed and was not read yet.
		ExposureReadback* newest = nullptr;
		for (ExposureReadback& entry : m_ExposureReadbacks)
		{
			if (entry.Readback->IsPending() && entry.Epoch == m_PreExposureEpoch && entry.Render > m_ExposureReadbackSeen && entry.Readback->IsReady()
				&& (!newest || entry.Render > newest->Render))
				newest = &entry;
		}
		if (!newest)
			return;
		m_ExposureReadbackSeen = newest->Render;
		float values[2] = {};
		if (!newest->Readback->Read(values, sizeof(values)))
			return;
		const float multiplier = values[RenderData::c_ExposureMultiplier];
		if (!(multiplier > 0.0f) || !std::isfinite(multiplier))
			return; // Nothing to follow (an image that metered nothing keeps its exposure)
		// A target beyond the pre-exposure's range keeps the limit it reached.
		const float target = std::log2(multiplier) - static_cast<float>(c_PreExposureHeadroomStops);
		const float candidate = GetPreExposureFor(target);
		if (std::abs(std::log2(m_PreExposure) - target) > 1.0f && candidate != m_PreExposure)
		{
			m_PreExposure = candidate;
			m_PreExposureSettled = false;
		}
		else if (newest->PreExposure == m_PreExposure)
		{
			m_PreExposureSettled = true;
		}
	}

	void SceneRenderer::RenderBloom(nvrhi::ICommandList* commandList, const PostProcessComponent& settings, nvrhi::IBindingSet* prefilterSet)
	{
		RenderData::BloomParameters parameters = {};
		parameters.Threshold = std::max(settings.BloomThreshold, 0.0f);
		parameters.Knee = parameters.Threshold * 0.5f;
		parameters.InversePreExposure = 1.0f / m_PreExposure;

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

		dispatch(prefilterSet, RenderData::c_BloomPrefilter, 0);
		for (uint32_t level = 1; level < m_BloomLevels; level++)
			dispatch(m_BloomDownsampleSets[level], RenderData::c_BloomDownsample, level);
		for (uint32_t level = m_BloomLevels - 1; level-- > 0;)
			dispatch(m_BloomUpsampleSets[level], RenderData::c_BloomUpsample, level);
	}

	bool SceneRenderer::EnsureTemporalResources()
	{
		if (m_HistoryTextures[0])
			return true;

		const uint32_t width = m_ViewportSize.x;
		const uint32_t height = m_ViewportSize.y;
		std::array<nvrhi::TextureHandle, 2> textures;
		for (uint32_t index = 0; index < 2; index++)
		{
			nvrhi::TextureDesc desc;
			desc.width = width;
			desc.height = height;
			desc.format = c_HDRFormat;
			desc.isUAV = true;
			desc.debugName = fmt::format("{}.History{}", m_Specification.DebugName, index);
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			textures[index] = m_Device->createTexture(desc);
		}
		bool created = textures[0] && textures[1];
		std::array<nvrhi::BindingSetHandle, 2> resolveSets;
		std::array<nvrhi::BindingSetHandle, 2> exposureSets;
		std::array<nvrhi::BindingSetHandle, 2> bloomSets;
		std::array<nvrhi::BindingSetHandle, 2> tonemapSets;
		for (uint32_t index = 0; created && index < 2; index++)
		{
			nvrhi::BindingSetDesc resolve;
			resolve.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(22, m_HDRTexture),
				nvrhi::BindingSetItem::Texture_SRV(23, m_DepthTexture),
				nvrhi::BindingSetItem::Texture_SRV(24, m_VelocityTexture),
				nvrhi::BindingSetItem::Texture_SRV(25, textures[index]),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(26, m_ExposureBuffer),
				nvrhi::BindingSetItem::Sampler(8, m_LinearClampSampler),
				nvrhi::BindingSetItem::Texture_UAV(0, textures[1 - index]),
				nvrhi::BindingSetItem::PushConstants(1, sizeof(RenderData::TemporalParameters))
			};
			resolveSets[index] = m_Device->createBindingSet(resolve, m_TemporalBindingLayout);
			// The post-processing of the image a texture holds, as of the HDR target's (CreateRenderTargets).
			nvrhi::BindingSetDesc exposure;
			exposure.bindings = {
				nvrhi::BindingSetItem::Texture_SRV(0, textures[index]),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_HistogramBuffer),
				nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_ExposureBuffer),
				nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::ExposureParameters))
			};
			exposureSets[index] = m_Device->createBindingSet(exposure, m_ExposureBindingLayout);
			nvrhi::BindingSetDesc bloom;
			bloom.bindings = {
				nvrhi::BindingSetItem::Texture_SRV(0, textures[index]),
				nvrhi::BindingSetItem::Sampler(0, m_LinearClampSampler),
				nvrhi::BindingSetItem::Texture_UAV(0, m_BloomTexture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_ExposureBuffer),
				nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::BloomParameters))
			};
			bloomSets[index] = m_Device->createBindingSet(bloom, m_BloomBindingLayout);
			nvrhi::BindingSetDesc tonemap;
			tonemap.bindings = {
				nvrhi::BindingSetItem::Texture_SRV(0, textures[index]),
				nvrhi::BindingSetItem::Sampler(0, m_PointClampSampler),
				nvrhi::BindingSetItem::Texture_SRV(1, m_BloomTexture, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(0, 1, 0, 1)),
				nvrhi::BindingSetItem::Sampler(1, m_LinearClampSampler),
				nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_ExposureBuffer),
				nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::TonemapParameters))
			};
			tonemapSets[index] = m_Device->createBindingSet(tonemap, m_TonemapBindingLayout);
			created = resolveSets[index] && exposureSets[index] && bloomSets[index] && tonemapSets[index];
		}
		if (!created)
		{
			// Nothing used them yet: they go with their handles.
			m_Errors.Report(fmt::format("failed to create the temporal anti-aliasing history for a {}x{} viewport; drawing with FXAA", width, height));
			return false;
		}
		m_HistoryTextures = std::move(textures);
		m_HistoryIndex = 0;
		m_TemporalResolveSets = std::move(resolveSets);
		m_TemporalExposureSets = std::move(exposureSets);
		m_TemporalBloomSets = std::move(bloomSets);
		m_TemporalTonemapSets = std::move(tonemapSets);
		return true;
	}

	void SceneRenderer::ResolveTemporal(nvrhi::ICommandList* commandList)
	{
		RenderData::TemporalParameters parameters = {};
		parameters.CurrentWeight = 1.0f / static_cast<float>(m_TemporalFrames);
		parameters.MinWeight = c_TemporalMinWeight;
		parameters.MotionWeightScale = c_TemporalMotionWeightScale;
		parameters.MaxMotionWeight = c_TemporalMaxMotionWeight;
		parameters.HistoryScale = parameters.CurrentWeight < 1.0f ? m_PreExposure / m_HistoryPreExposure : 1.0f;
		parameters.InversePreExposure = 1.0f / m_PreExposure;
		m_HistoryPreExposure = m_PreExposure;
		nvrhi::ComputeState state;
		state.pipeline = m_TemporalPipeline;
		state.bindings = { m_TemporalResolveSets[m_HistoryIndex] };
		commandList->setComputeState(state);
		commandList->setPushConstants(&parameters, sizeof(parameters));
		commandList->dispatch(DispatchGroups(m_ViewportSize.x), DispatchGroups(m_ViewportSize.y), 1);
		m_HistoryIndex = 1 - m_HistoryIndex;
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
		// Without the cascades' pyramids the cascades are drawn whole (no occlusion culling): a limitation, not a failure.
		if (!EnsureShadowPyramid())
			ST_CORE_WARN("{}: the shadow cascades' depth pyramids could not be created; the cascades are not occlusion culled", m_Specification.DebugName);
		return true;
	}

	uint32_t SceneRenderer::SelectLOD(const Mesh& mesh, const Submesh& submesh, float screenFraction) const
	{
		return LODSelection::SelectLOD(submesh, mesh.GetBounds(), screenFraction, m_LODCriteria);
	}

	LODCriteria SceneRenderer::GetLODCriteria() const
	{
		const float height = static_cast<float>(m_ViewportSize.y);
		return LODCriteria { m_Specification.LODThreshold, LODSelection::GetErrorThreshold(m_Specification.LODErrorPixels, height), height };
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
		// Opaque batches (the prepass and the opaque pass draw them alike), transparent ones, the shadow batches, the selection
		// mask's, then the local shadow views'.
		for (const SceneDrawBatch& batch : m_OpaqueBatches)
			append(batch);
		for (const SceneDrawBatch& batch : m_TransparentBatches)
			append(batch);
		for (const SceneDrawBatch& batch : m_ShadowBatches)
			append(batch);
		for (const SceneDrawBatch& batch : m_SelectionBatches)
			append(batch);
		for (const SceneDrawBatch& batch : m_LocalShadowBatches)
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
				// Calls of at most the device's (or the specification's) draws each.
				const size_t maxDraws = GetMaxDrawsPerIndirectCall();
				for (size_t call = first; call < end;)
				{
					const size_t draws = std::min(maxDraws, end - call);
					commandList->drawIndexedIndirect(static_cast<uint32_t>((firstArgument + call) * sizeof(nvrhi::DrawIndexedIndirectArguments)),
						static_cast<uint32_t>(draws));
					m_Stats.DrawCommands++;
					call += draws;
				}
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
			case BufferKind::StructuredUAV:
				desc.structStride = static_cast<uint32_t>(elementSize);
				desc.canHaveUAVs = true;
				desc.initialState = nvrhi::ResourceStates::ShaderResource;
				break;
			case BufferKind::IndirectArgumentsUAV:
				desc.structStride = static_cast<uint32_t>(elementSize);
				desc.canHaveUAVs = true;
				desc.isDrawIndirectArgs = true;
				desc.initialState = nvrhi::ResourceStates::IndirectArgument;
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
		release(m_SelectionMaskTexture);
		release(m_SelectionMaskFramebuffer);
		release(m_OutlineMaskBindingSet);
		release(m_StatusTintBindingSet); // Likewise
		release(m_DepthPyramid);
		for (nvrhi::BindingSetHandle& set : m_DepthPyramidSets)
			release(set);
		m_DepthPyramidSets.clear();
		m_DepthPyramidLevels = 0;
		release(m_CullBindingSet); // References the depth pyramid
		// The motion history goes with the targets: the next frame that asks starts without one.
		release(m_VelocityTexture);
		release(m_MotionPrepassFramebuffer);
		release(m_MotionViewBindingSet);
		release(m_PreviousInstances);
		release(m_PreviousPalette);
		release(m_GrownPreviousInstances);
		release(m_GrownPreviousPalette);
		release(m_MotionCopyListBuffer);
		for (nvrhi::BindingSetHandle& set : m_MotionCopySets)
			release(set);
		m_MotionCopyBound = {};
		m_MotionRender = 0;
		// The temporal history starts again at the new size.
		for (uint32_t index = 0; index < 2; index++)
		{
			release(m_HistoryTextures[index]);
			release(m_TemporalResolveSets[index]);
			release(m_TemporalExposureSets[index]);
			release(m_TemporalBloomSets[index]);
			release(m_TemporalTonemapSets[index]);
		}
		m_HistoryIndex = 0;
		m_TemporalResolved = false;
		m_TemporalFrames = 0;
		m_TemporalRender = 0;
		// The local shadows' atlas goes with them: the views are drawn again in a new one.
		release(m_LocalShadowAtlas);
		release(m_LocalShadowFramebuffer);
		ResetLocalShadows();
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
		if (!CreateDepthPyramid())
			return fail("depth pyramid");

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

		const bool localShadowPass = key.PassType == Pass::LocalShadow || key.PassType == Pass::LocalShadowMasked;
		const bool shadowPass = key.PassType == Pass::Shadow || key.PassType == Pass::ShadowMasked || localShadowPass;
		const bool selectionMask = key.PassType == Pass::SelectionMask;
		nvrhi::GraphicsPipelineDesc desc;
		desc.primType = nvrhi::PrimitiveType::TriangleList;
		desc.inputLayout = key.Skinned ? m_SkinnedInputLayout : m_InputLayout;
		if (localShadowPass)
			desc.VS = key.Skinned ? m_SkinnedLocalShadowVertexShader : m_LocalShadowVertexShader;
		else if (key.Skinned)
			desc.VS = shadowPass ? m_SkinnedShadowVertexShader : (key.Velocity ? m_SkinnedMeshVelocityVertexShader : m_SkinnedMeshVertexShader);
		else
			desc.VS = shadowPass ? m_ShadowVertexShader : (key.Velocity ? m_MeshVelocityVertexShader : m_MeshVertexShader);
		switch (key.PassType)
		{
			case Pass::Prepass:      desc.PS = key.Velocity ? m_PrepassVelocityPixelShader : m_PrepassPixelShader; break;
			case Pass::ShadowMasked:
			case Pass::LocalShadowMasked: desc.PS = m_ShadowMaskPixelShader; break;
			case Pass::Shadow:
			case Pass::LocalShadow:  desc.PS = nullptr; break; // Depth only
			case Pass::Transparent:  desc.PS = m_ForwardTransparentPixelShader; break;
			case Pass::Opaque:       desc.PS = m_ForwardPixelShader; break;
			case Pass::SelectionMask: desc.PS = m_SelectionMaskPixelShader; break;
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
			case Pass::LocalShadow:
			case Pass::LocalShadowMasked:
				depth.setDepthWriteEnable(true).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
				// Push stored depth away from the light against shadow acne. NVRHI enables the depth bias only with a
				// non-zero constant part, which also keeps the slope part working.
				desc.renderState.rasterState.setDepthBias(c_ShadowDepthBias).setSlopeScaleDepthBias(c_ShadowSlopeDepthBias);
				break;
			case Pass::Opaque:
				depth.setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::Equal);
				break;
			case Pass::SelectionMask:
				// The selected surfaces where nothing is in front of them, drawn with the camera's own matrices against the
				// jittered depth: with its bias.
				depth.setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
				SetJitteredDepthBias(desc.renderState.rasterState);
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

		nvrhi::IFramebuffer* framebuffer = localShadowPass ? m_LocalShadowFramebuffer.Get() : shadowPass ? m_ShadowFramebuffers[0].Get()
			: (key.PassType == Pass::Prepass ? (key.Velocity ? m_MotionPrepassFramebuffer.Get() : m_PrepassFramebuffer.Get())
			: (selectionMask ? m_SelectionMaskFramebuffer.Get() : m_ForwardFramebuffer.Get()));
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

	void SceneRenderer::CollectLights(Scene& scene, const RenderScene& renderScene, const SceneCamera& camera, bool preview, RenderData::FrameConstants& frame)
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
			m_ShadowLight.CascadeBlend = std::isfinite(light.CascadeBlend) ? std::clamp(light.CascadeBlend, 0.0f, DirectionalLightComponent::c_MaxCascadeBlend) : 0.0f;
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

		// Exponential depth slices between the near and far distance; the first slice also holds everything closer, the
		// last everything further away.
		const glm::uvec3 grid = m_Specification.LightClusterGrid;
		const float sliceNear = std::max(camera.Near, 0.01f);
		const float sliceFar = std::isfinite(camera.Far) ? std::clamp(camera.Far, sliceNear * 2.0f, 1.0e6f) : 1.0e6f;
		const float scale = static_cast<float>(grid.z) / std::log(sliceFar / sliceNear);
		frame.ClusterGrid = glm::uvec4(grid, m_Specification.MaxLightsPerCluster);
		frame.ClusterDepth = glm::vec4(scale, -scale * std::log(sliceNear), std::min(camera.Near, 0.0f), c_ClusterDepthEnd);

		m_LightCandidates.clear();
		m_Lights.clear();
		m_LightBounds.clear();
		m_LightRecordIndices.clear();
		const RenderLights& lights = renderScene.GetLights();
		m_GPULightsFrame = m_GPUDrivenFrame && m_LightCullPipeline && m_LightCompactPipeline;
		if (m_GPULightsFrame)
		{
			// The GPU culls every light record into the light buffer (RunGPULightCulling): all lights in view, uncapped.
			m_GPULightRecords = m_SceneGPU->GetLightCapacity();
			frame.LightCounts = glm::uvec4(lights.GetCount() > 0 && m_GPULightRecords > 0 ? 1u : 0u, 1u, 0u, 0u);
			return;
		}

		// Point and spot lights: frustum culled by the bounding sphere of the region they reach, then ranked by
		// relevance (lights containing the camera first, then by the projected size of that sphere).
		const Frustum frustum(camera.Projection * camera.View);
		const std::span<const RenderData::LightRecord> records = lights.GetRecords();
		for (uint32_t index = 0; index < records.size(); index++)
		{
			const RenderData::LightRecord& record = records[index];
			if (record.SpotAngles.z == 0.0f)
				continue;
			LightCandidate candidate;
			candidate.Bounds = record.Bounds;
			if (!frustum.IsSphereVisible(glm::vec3(candidate.Bounds), candidate.Bounds.w))
			{
				m_Stats.CulledLights++;
				continue;
			}
			candidate.Data.PositionRange = record.PositionRange;
			candidate.Data.Color = record.Color;
			candidate.Data.DirectionType = record.DirectionType;
			candidate.Data.SpotAngles = glm::vec4(record.SpotAngles.x, record.SpotAngles.y, 0.0f, 0.0f);
			const float distance = glm::length(glm::vec3(candidate.Bounds) - camera.Position);
			if (camera.Orthographic)
				candidate.Relevance = candidate.Bounds.w; // The projected size does not depend on the distance
			else if (distance <= candidate.Bounds.w)
				candidate.Relevance = std::numeric_limits<float>::max();
			else
				candidate.Relevance = candidate.Bounds.w / (distance - candidate.Bounds.w);
			candidate.Order = index;
			m_LightCandidates.push_back(candidate);
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

		// Uploaded camera-relative (the shaders' positions are), the subtraction in double so lights near a camera far from
		// the origin keep their precision; the clusters' bounds are in view space.
		const glm::mat3 viewRotation(camera.View);
		const glm::dvec3 cameraPosition(camera.Position);
		for (const LightCandidate& candidate : m_LightCandidates)
		{
			RenderData::LightData light = candidate.Data;
			light.PositionRange = glm::vec4(glm::vec3(glm::dvec3(glm::vec3(light.PositionRange)) - cameraPosition), light.PositionRange.w);
			m_Lights.push_back(light);
			m_LightRecordIndices.push_back(candidate.Order);
			const glm::vec3 relativeCenter(glm::dvec3(glm::vec3(candidate.Bounds)) - cameraPosition);
			m_LightBounds.push_back(RenderData::LightBounds { glm::vec4(viewRotation * relativeCenter, candidate.Bounds.w) });
		}
		frame.LightCounts = glm::uvec4(static_cast<uint32_t>(m_Lights.size()), 0, 0, 0);
	}

	void SceneRenderer::DrawBatches(nvrhi::ICommandList* commandList, Pass pass, bool transparent)
	{
		const std::vector<SceneDrawBatch>& batches = transparent ? m_TransparentBatches : m_OpaqueBatches;
		if (batches.empty())
			return;

		nvrhi::GraphicsState state;
		state.framebuffer = pass == Pass::Prepass ? GetPrepassFramebuffer() : m_ForwardFramebuffer.Get();
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y)));
		state.bindings = { m_SceneBindingSet, Renderer::GetBindlessTextures().GetTable() };
		const uint32_t firstArgument = transparent ? static_cast<uint32_t>(m_OpaqueBatches.size()) : 0u;
		// Transparent surfaces have no EQUAL test to agree with and write no motion: the plain shaders.
		const bool velocity = m_MotionFrame && pass != Pass::Transparent;
		DrawBuckets(commandList, state, batches, firstArgument,
			[pass, velocity](const SceneDrawBatch& batch) { return PipelineKey { pass, batch.DoubleSided, batch.Mirrored, batch.Skinned, velocity }; }, nullptr,
			nullptr);
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
		m_LODCriteria = GetLODCriteria();
		const bool rendered = RenderFrame(scene, camera, target, options);
		if (rendered)
		{
			m_RenderCount++;
			ReportTextureUse(camera);
		}
		// The references the frame resolved end with it, so a renderer that stops rendering (a capture renderer between
		// captures, a hidden view) keeps none of the meshes, materials and textures it drew resident (the environment
		// cache keeps the sky light's map) and no asset manager alive.
		ReleaseFrameAssets();
		m_SceneGPU = nullptr; // Its mirror may go before the next frame (RenderSceneGPUCache::BeginFrame)
		return rendered;
	}

	glm::vec2 SceneRenderer::GetTemporalJitter(uint32_t phase)
	{
		// The radical inverses of the phase's index in bases 2 and 3; the index starts at 1, since 0 is the origin of both.
		const auto radicalInverse = [](uint32_t index, uint32_t base)
		{
			float result = 0.0f;
			float scale = 1.0f / static_cast<float>(base);
			for (; index > 0; index /= base)
			{
				result += static_cast<float>(index % base) * scale;
				scale /= static_cast<float>(base);
			}
			return result;
		};
		const uint32_t index = phase % c_TemporalJitterPhases + 1;
		return glm::vec2(radicalInverse(index, 2), radicalInverse(index, 3)) - 0.5f;
	}

	glm::vec3 SceneRenderer::GetViewDirection(const SceneCamera& camera)
	{
		const glm::vec3 forward = -glm::vec3(camera.View[0][2], camera.View[1][2], camera.View[2][2]);
		const float forwardLength = glm::length(forward);
		return forwardLength > 0.0f ? forward / forwardLength : glm::vec3(0.0f, 0.0f, -1.0f);
	}

	bool SceneRenderer::IsCameraCut(const SceneCamera& camera) const
	{
		return !m_HasPreviousCamera || TextureStreamingPolicy::IsCameraCut(m_PreviousCameraPosition, m_PreviousCameraForward, camera.Position, GetViewDirection(camera));
	}

	glm::mat4 SceneRenderer::GetRelativeView(const SceneCamera& camera)
	{
		return glm::mat4(glm::mat3(camera.View));
	}

	void SceneRenderer::ReportTextureUse(const SceneCamera& camera)
	{
		// A view that jumped since the previous frame: what it needs now goes first.
		const glm::vec3 direction = GetViewDirection(camera);
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
		ResolveCullCounters(wait);
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
			// A clone of the render scene the renderer saw counts on from the source's updates (it copied their count).
			const bool seenBefore = m_ProxyUpdatesScene == renderScene.GetId() || (m_ProxyUpdatesScene != 0 && m_ProxyUpdatesScene == renderScene.GetCloneSource());
			m_Stats.ProxyUpdates = static_cast<uint32_t>(totalUpdates - (seenBefore ? m_ProxyUpdatesSeen : 0));
			m_ProxyUpdatesScene = renderScene.GetId();
			m_ProxyUpdatesSeen = totalUpdates;
			m_Assets.BeginFrame(assets);
			PrepareExtraction(renderScene);
		}

		// The GPU's copy of the render scene, shared with the scene's other renderers: what changed since its last update
		// uploads now, in a command list of its own, ahead of this frame's. Only proxies it holds are drawn, with the meshes and
		// materials its tables hold: what is loaded (the frame's requests publish nothing, so they are what the frame resolves).
		m_SceneGPU = &Renderer::GetRenderSceneGPUs().Get(renderScene);
		RenderSceneGPUUpdate sceneUpload;
		if (!m_SceneGPU->Update(renderScene, assets, m_Specification.NewInstanceBytesPerFrame, &sceneUpload) && !m_SceneGPU->HasBuffers())
		{
			m_Errors.Report("the scene's GPU buffers are unavailable (see the previous error)");
			m_Stats = {};
			return false;
		}
		m_Stats.InstanceBytesUploaded += sceneUpload.InstanceBytes;
		m_Stats.InstanceUploadRanges += sceneUpload.InstanceRanges;
		m_Stats.ProxyBytesUploaded = sceneUpload.ProxyBytes;
		m_Stats.PaletteBytesUploaded = sceneUpload.PaletteBytes;
		m_Stats.MaterialBytesUploaded = sceneUpload.MaterialBytes;
		m_Stats.DeferredInstanceBytes = m_SceneGPU->GetDeferredBytes();
		m_Stats.DeferredProxies = m_SceneGPU->GetDeferredProxies();

		// Temporal anti-aliasing (below) blends along the motion vectors.
		const PostProcessComponent postProcess = GetPostProcessSettings(scene);
		ChoosePreExposure(postProcess, options);
		m_Stats.PreExposure = m_PreExposure;
		m_Stats.PreExposureSettled = m_PreExposureSettled;
		const bool temporalWanted = m_Specification.TemporalAntiAliasing && postProcess.AntiAliasing && postProcess.AntiAliasingMethod == AntiAliasingMode::TAA;

		// Motion: the history holds the renderer's previous frame when that frame copied it (its latest frame asked for motion
		// too) and drew this render scene or the one it is a clone of (the same slots).
		m_MotionFrame = options.MotionVectors || options.ShowMotionVectors || temporalWanted;
		const bool motionHistory = m_MotionFrame && m_MotionRender != 0 && m_MotionRender == m_RenderCount
			&& (m_MotionSceneId == renderScene.GetId() || renderScene.GetCloneSource() == m_MotionSceneId);
		bool motionFailed = false;
		if (m_MotionFrame)
		{
			// Until this frame copies it, the history holds no frame a later one could compare with (a failed frame leaves it so).
			m_MotionRender = 0;
			if (!EnsureMotionResources(motionHistory))
			{
				m_MotionFrame = false;
				motionFailed = true;
			}
		}
		m_Stats.MotionHistory = m_MotionFrame && motionHistory;

		// Temporal anti-aliasing: the history goes on from the renderer's previous frame when that frame resolved it, in this
		// device frame or the one before, of this render scene or the one it is a clone of (entering Play keeps it), without a
		// camera cut or a reset asked for (a resize released it). Counted in frames, never in time, so slow frames keep it.
		// Without its resources (or the motion vectors') the frame is drawn with FXAA.
		const bool cameraCut = IsCameraCut(camera);
		m_TemporalFrame = temporalWanted && m_MotionFrame && EnsureTemporalResources();
		const bool temporalFailed = temporalWanted && !m_TemporalFrame;
		m_TemporalResolved = false;
		glm::vec2 jitterClip(0.0f);
		glm::vec2 previousJitterClip(0.0f);
		if (m_TemporalFrame)
		{
			const uint64_t deviceFrame = Renderer::GetFrameNumber();
			const bool continues = m_TemporalFrames > 0 && m_TemporalRender != 0 && m_TemporalRender == m_RenderCount && !cameraCut && !m_ResetHistory
				&& (m_TemporalSceneId == renderScene.GetId() || renderScene.GetCloneSource() == m_TemporalSceneId)
				&& (deviceFrame == m_TemporalDeviceFrame || deviceFrame == m_TemporalDeviceFrame + 1);
			if (continues)
			{
				// Kept within the counter in whole cycles of the jitter, so its phases go on in order.
				constexpr uint32_t c_FramesWrap = 1u << 30;
				m_TemporalFrames = m_TemporalFrames + 1 < c_FramesWrap ? m_TemporalFrames + 1 : m_TemporalFrames + 1 - c_FramesWrap / 2;
				previousJitterClip = m_TemporalJitterClip;
			}
			else
			{
				m_TemporalFrames = 1;
				m_HistoryResets++;
			}
			m_TemporalRender = 0; // Until this frame resolved it
			m_TemporalDeviceFrame = deviceFrame;
			m_TemporalSceneId = renderScene.GetId();
			m_ResetHistory = false;
			m_Stats.Jitter = GetTemporalJitter((m_TemporalFrames - 1) % c_TemporalJitterPhases);
			// Clip space's +Y is up, the jitter's +y down.
			jitterClip = glm::vec2(2.0f * m_Stats.Jitter.x / static_cast<float>(m_ViewportSize.x), -2.0f * m_Stats.Jitter.y / static_cast<float>(m_ViewportSize.y));
			m_TemporalJitterClip = jitterClip;
		}
		m_Stats.TemporalAntiAliasing = m_TemporalFrame;
		m_Stats.TemporalFrames = m_TemporalFrame ? m_TemporalFrames : 0u;
		m_Stats.HistoryResets = m_HistoryResets;

		// Scene shaders work camera-relative (Include/Scene.glsl): the view's translation is left out of its matrices. The jitter
		// is only in the frame's copy of the projection: what the CPU culls, chooses levels of detail and fits the cascades with
		// is the camera's own, and so are the motion vectors' matrices.
		RenderData::FrameConstants frame = {};
		frame.View = GetRelativeView(camera);
		frame.Projection = camera.Projection;
		if (m_TemporalFrame)
			frame.Projection = glm::translate(glm::mat4(1.0f), glm::vec3(jitterClip, 0.0f)) * camera.Projection;
		frame.ViewProjection = frame.Projection * frame.View;
		frame.InverseView = glm::inverse(frame.View);
		frame.InverseProjection = glm::inverse(frame.Projection);
		frame.InverseViewProjection = glm::inverse(frame.ViewProjection);
		frame.JitterParams = glm::vec4(jitterClip, previousJitterClip);
		// Built in double, from the same products, so a camera that did not move gives the same matrix twice (no motion).
		frame.UnjitteredViewProjection = glm::mat4(glm::dmat4(camera.Projection) * glm::dmat4(frame.View));
		frame.PreviousViewProjection = frame.UnjitteredViewProjection;
		if (m_Stats.MotionHistory)
		{
			const glm::dvec3 offset = glm::dvec3(camera.Position) - glm::dvec3(m_MotionCamera.Position);
			frame.PreviousViewProjection = glm::mat4(glm::dmat4(m_MotionCamera.Projection) * glm::dmat4(m_MotionCamera.RelativeView)
				* glm::translate(glm::dmat4(1.0), offset));
		}
		// Per-pixel noise turns by the golden ratio's fraction of a turn each frame of the history (from its reset on, so captures
		// are deterministic): the frames the history averages sample the turn evenly.
		const double noiseTurn = m_TemporalFrame ? std::fmod(static_cast<double>(m_TemporalFrames - 1) * 0.6180339887498949, 1.0) : 0.0;
		frame.TemporalParams = glm::vec4(m_Stats.MotionHistory ? 1.0f : 0.0f, m_PreExposure, static_cast<float>(noiseTurn), 0.0f);
		frame.CameraPosition = glm::vec4(camera.Position, camera.Near);
		const glm::vec3 forward = -glm::vec3(frame.InverseView[2]);
		frame.CameraForward = glm::vec4(glm::length(forward) > 1e-6f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f), camera.Orthographic ? 1.0f : 0.0f);
		frame.ViewportSize = glm::vec4(glm::vec2(m_ViewportSize), 1.0f / glm::vec2(m_ViewportSize));
		frame.TimeParams = glm::vec4(static_cast<float>(scene.GetTime()), 0.0f, 0.0f, 0.0f);
		// Preview lighting only stands in for lighting the scene does not have at all.
		const bool preview = options.PreviewEnvironment && NeedsPreviewLighting(scene);
		m_Stats.PreviewLighting = preview;
		// The GPU-driven path culls and builds the opaque and shadow draws on the GPU, and culls the point and spot lights; the
		// CPU still resolves the frame's assets and fits the cascades, and draws transparent surfaces.
		m_GPUDrivenFrame = IsGPUDriven();
		m_Stats.GPUDriven = m_GPUDrivenFrame;
		CollectLights(scene, renderScene, camera, preview, frame);
		const auto extractionStart = std::chrono::steady_clock::now();
		// The CPU's culling works in world space (the spatial index holds world bounds).
		const Frustum frustum(camera.Projection * camera.View);
		// What the frame requests: the GPU's visibility feedback where it can tell (SceneRendererSpecification::
		// VisibilityFeedback), else the CPU's walk of the view.
		if (cameraCut)
		{
			m_CutRender = m_RenderCount + 1;
			m_ResetVisibility = true; // Occlusion culling starts without history
			m_LODFadeFrame++;         // And so do level-of-detail fades: a frame skipped draws every level at once
		}
		m_LODFadeFrame++;
		m_FeedbackFrame = UsesFeedback(renderScene);
		m_Stats.FeedbackRequests = m_FeedbackFrame;
		if (m_FeedbackFrame)
		{
			ApplyFeedback(renderScene, camera, frustum);
		}
		else
		{
			CullView(renderScene, camera, frustum);
			m_MarksRender = m_RenderCount + 1;
		}
		FindShadowCasters(renderScene, camera, frustum, frame, !m_FeedbackFrame);
		// The local shadow views' casters: the CPU's on frames that walk the view (both paths), else bounds for the GPU's culling.
		FindLocalShadowCasters(renderScene, camera, frustum, !m_FeedbackFrame, !m_FeedbackFrame);
		// Rendered with a reported limitation: keep the error so it is not logged every frame.
		bool degraded = motionFailed || temporalFailed;
		if (!m_LocalShadowLights.empty() && !EnsureLocalShadowAtlas())
		{
			ResetLocalShadows(); // Reported; no light casts a local shadow this frame
			m_Stats.LocalShadowLights = 0;
			m_Stats.LocalShadowViewsDrawn = 0;
			m_Stats.LocalShadowViewsPending = 0;
			degraded = true;
		}
		// The CPU's lights name their shadow slots (the GPU-driven path's light culling reads them from a table).
		if (!m_GPULightsFrame && !m_LocalShadowLights.empty())
		{
			for (size_t index = 0; index < m_Lights.size(); index++)
			{
				for (uint32_t slot = 0; slot < m_LocalShadowLights.size(); slot++)
				{
					if (m_LocalShadowLights[slot].Record == m_LightRecordIndices[index] && m_LocalShadowLights[slot].Strength > 0.0f)
						m_Lights[index].SpotAngles.z = static_cast<float>(slot + 1);
				}
			}
		}
		ResolveAssets(renderScene);
		GatherDraws(renderScene, camera, !m_GPUDrivenFrame);
		GatherShadowDraws(renderScene, camera, frame, !m_GPUDrivenFrame);
		GatherLocalShadowDraws(renderScene, !m_GPUDrivenFrame);
		BuildLocalShadowData(camera);
		// With the jitter the selection outline comes from a mask of the selected instances drawn without it.
		m_SelectionBatches.clear();
		if (m_TemporalFrame && m_EntityIDTexture && !options.SelectedEntities.empty())
			GatherSelectionDraws(renderScene, camera, scene, options.SelectedEntities);
		BuildDrawArguments();
		// Before the GPU's culling is prepared: the cascades' occlusion culling needs their pyramids, made with the map.
		if (frame.ShadowParams.w > 0.0f && !EnsureShadowMap())
		{
			m_Errors.Report("failed to create the shadow map; rendering without shadows");
			frame.ShadowParams.w = 0.0f;
			degraded = true;
		}
		if (m_GPUDrivenFrame)
			PrepareGPUCulling(renderScene, camera);
		ReportVisibleAnimators(scene);
		m_Stats.ExtractMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - extractionStart).count() - m_Stats.SortMs;
		frame.AOParams = glm::vec4(postProcess.AmbientOcclusion ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
		frame.MaterialParams = glm::vec4(m_Specification.FadeTextureLevels ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);

		bool buffersRecreated = !m_SceneBindingSet || m_BoundShadowMap != (m_ShadowMap ? m_ShadowMap.Get() : m_DummyShadowMap.Get())
			|| m_BoundLocalShadowAtlas != (m_LocalShadowAtlas ? m_LocalShadowAtlas.Get() : m_DummyLocalShadowAtlas.Get())
			|| m_BoundSceneGPUVersion != m_SceneGPU->GetBufferVersion()
			|| m_BoundPreviousInstances != (m_PreviousInstances ? m_PreviousInstances.Get() : m_SceneGPU->GetInstanceBuffer())
			|| m_BoundPreviousPalette != (m_PreviousPalette ? m_PreviousPalette.Get() : m_SceneGPU->GetPaletteBuffer());
		bool drawListsRecreated = false;
		bool cullRecreated = false;
		const bool buffersReady = (!m_GPUDrivenFrame || EnsureCullBuffers(cullRecreated))
			&& EnsureBufferCapacity(m_DrawListBuffer, m_DrawInstancesBuild.size(), sizeof(uint32_t), "DrawList", drawListsRecreated)
			&& EnsureBufferCapacity(m_ShadowDrawListBuffer, m_ShadowDrawInstancesBuild.size(), sizeof(uint32_t), "ShadowDrawList", drawListsRecreated)
			&& EnsureBufferCapacity(m_LightBuffer, m_GPULightsFrame ? m_GPULightRecords : m_Lights.size(), sizeof(RenderData::LightData), "Lights", buffersRecreated,
				BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_LightBoundsBuffer, m_GPULightsFrame ? m_GPULightRecords : m_LightBounds.size(), sizeof(RenderData::LightBounds), "LightBounds",
				buffersRecreated, BufferKind::StructuredUAV)
			&& EnsureBufferCapacity(m_DrawArgumentBuffer, m_DrawArgumentsBuild.size(), sizeof(nvrhi::DrawIndexedIndirectArguments), "DrawArguments", drawListsRecreated,
				BufferKind::IndirectArguments);
		buffersRecreated = buffersRecreated || drawListsRecreated || cullRecreated;
		if (cullRecreated)
		{
			// The culling tables start again in their new buffers, and the binding set names the new ones.
			m_CullBindingSet = nullptr;
			m_CullTablesUploaded = false;
		}
		if (!buffersReady)
		{
			// A replaced buffer holds nothing yet: upload everything to the next one.
			m_FullUpload = m_FullUpload || drawListsRecreated;
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
		if (m_GrownPreviousInstances || m_GrownPreviousPalette)
			RecordMotionGrowth(commandList);
		UpdateEnvironment(scene, preview, frame, commandList);
		SetHeightFog(scene, camera, frame);
		// The texture table's changed entries (textures that streamed levels moved to other bindless slots) before anything
		// samples them; a table that grew has a new buffer, which the binding sets follow.
		TextureTable& textureTable = Renderer::GetTextureTable();
		const bool tableUploaded = textureTable.RecordUploads(commandList);
		const bool rebind = buffersRecreated || m_BoundEnvironment != GetEnvironmentCube() || m_BoundSkyView != GetBoundSkyView()
			|| m_BoundReflectionProbes != GetBoundReflectionProbes()
			|| m_BoundTextureTableVersion != textureTable.GetBufferVersion();
		if (!tableUploaded || (rebind && !CreateSceneBindingSets()))
		{
			gpuTimer.End();
			commandList->endMarker();
			commandList->close();
			m_Device->executeCommandList(commandList); // Keeps the environment processing that was recorded
			m_Errors.Report(tableUploaded ? "failed to create the scene binding sets" : "the texture table could not be uploaded (see the previous error)");
			m_FullUpload = m_FullUpload || drawListsRecreated;
			m_Stats = {};
			return false;
		}

		commandList->writeBuffer(m_FrameConstantBuffer, &frame, sizeof(frame));
		UploadSceneData(commandList, drawListsRecreated);
		if (!m_LocalShadowData.empty())
			commandList->writeBuffer(m_LocalShadowBuffer, m_LocalShadowData.data(), m_LocalShadowData.size() * sizeof(RenderData::LocalShadowData));
		if (!m_Lights.empty())
		{
			commandList->writeBuffer(m_LightBuffer, m_Lights.data(), m_Lights.size() * sizeof(RenderData::LightData));
			commandList->writeBuffer(m_LightBoundsBuffer, m_LightBounds.data(), m_LightBounds.size() * sizeof(RenderData::LightBounds));
		}

		if (m_GPUDrivenFrame)
		{
			// After the frame's uploads of what casters are drawn with: which cascades keep their layers (the culling draws
			// nothing into them).
			m_CachedCascadeMask = frame.ShadowParams.w > 0.0f ? PrepareShadowCache(renderScene, camera) : 0u;
			m_CullConstants.CachedCascades = m_CachedCascadeMask;
			m_Stats.CachedCascadeMask = m_CachedCascadeMask;
			if (!UploadCullTables(commandList))
			{
				gpuTimer.End();
				commandList->endMarker();
				commandList->close();
				m_Device->executeCommandList(commandList); // Keeps the uploads that were recorded
				m_Errors.Report("the GPU's culling tables could not be uploaded (see the previous error)");
				m_Stats = {};
				return false;
			}
			gpuTimer.Next(gpuStages.Cull);
			commandList->beginMarker("Cull");
			RunGPUCulling(commandList, frame.ShadowParams.w > 0.0f || !m_LocalShadowViews.empty());
			commandList->endMarker();
		}

		if (m_GPULightsFrame && frame.LightCounts.x > 0)
		{
			commandList->beginMarker("LightCull");
			RunGPULightCulling(commandList, camera);
			commandList->endMarker();
		}
		if (frame.LightCounts.x > 0)
		{
			commandList->beginMarker("LightClusters");
			BuildLightClusters(commandList);
			commandList->endMarker();
		}

		if (frame.ShadowParams.w > 0.0f || !m_LocalShadowViews.empty())
		{
			gpuTimer.Next(gpuStages.Shadows);
			commandList->beginMarker("Shadows");
			if (frame.ShadowParams.w > 0.0f)
			{
				FitCascades(commandList);
				if (m_GPUDrivenFrame)
					DrawGPUShadows(commandList);
				else
				{
					m_ShadowCacheValid = 0; // The CPU path redraws every layer
					DrawShadows(commandList, static_cast<uint32_t>(frame.ShadowParams.w));
				}
			}
			if (!m_LocalShadowViews.empty())
			{
				commandList->beginMarker("LocalShadows");
				if (m_GPUDrivenFrame)
					DrawGPULocalShadows(commandList);
				else
					DrawLocalShadows(commandList);
				commandList->endMarker();
			}
			commandList->endMarker();
		}

		gpuTimer.Next(gpuStages.Prepass);
		const glm::vec4 clear = camera.ClearColor;
		commandList->clearDepthStencilTexture(m_DepthTexture, nvrhi::AllSubresources, true, 0.0f, false, 0);
		commandList->clearTextureFloat(m_NormalTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		if (m_EntityIDTexture)
			commandList->clearTextureUInt(m_EntityIDTexture, nvrhi::AllSubresources, 0);
		if (m_MotionFrame)
			commandList->clearTextureFloat(m_VelocityTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		// The clear color is a luminance too: pre-exposed like everything drawn over it.
		commandList->clearTextureFloat(m_HDRTexture, nvrhi::AllSubresources,
			nvrhi::Color(SRGBToLinear(clear.r) * m_PreExposure, SRGBToLinear(clear.g) * m_PreExposure, SRGBToLinear(clear.b) * m_PreExposure, 1.0f));

		commandList->beginMarker("Prepass");
		if (m_GPUDrivenFrame)
		{
			DrawGPUView(commandList, Pass::Prepass, 0, m_CullConstants.FirstPhaseViewBuckets);
			if (IsOcclusionCulled())
			{
				// What the first phase did not draw, found visible against its depth, in the same frame.
				RunOcclusionPhase(commandList);
				DrawGPUView(commandList, Pass::Prepass, m_CullConstants.FirstPhaseViewBuckets, m_CullConstants.ViewBucketCount);
			}
			RecordCullReadback(commandList);
		}
		else
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
		if (m_GPUDrivenFrame)
			DrawGPUView(commandList, Pass::Opaque, 0, m_CullConstants.ViewBucketCount);
		else
			DrawBatches(commandList, Pass::Opaque, false);
		commandList->endMarker();

		if (frame.EnvironmentParams.w > 0.0f && frame.SkyParams.y > 0.0f)
		{
			// The atmosphere's background comes from its sky-view table (the variant), the other skies' from the environment cube.
			nvrhi::GraphicsPipelineHandle& skyboxPipeline = m_AtmosphereFrame ? m_AtmosphereSkyboxPipeline : m_SkyboxPipeline;
			if (!skyboxPipeline)
			{
				nvrhi::GraphicsPipelineDesc desc;
				desc.primType = nvrhi::PrimitiveType::TriangleList;
				desc.VS = m_FullscreenVertexShader;
				desc.PS = m_AtmosphereFrame ? m_AtmosphereSkyboxPixelShader : m_SkyboxPixelShader;
				desc.bindingLayouts = { m_SceneBindingLayout, Renderer::GetBindlessTextures().GetLayout() };
				desc.renderState.rasterState.setCullNone();
				// The fullscreen triangle lies at depth 0 (the far plane with reversed-Z): it covers only empty pixels.
				desc.renderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
				skyboxPipeline = m_Device->createGraphicsPipeline(desc, m_ForwardFramebuffer->getFramebufferInfo());
				ST_CORE_VERIFY(skyboxPipeline, "SceneRenderer: failed to create the skybox pipeline");
			}
			nvrhi::GraphicsState skyState;
			skyState.pipeline = skyboxPipeline;
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

		// After every pass that reads the history: it now holds this frame for the next.
		if (m_MotionFrame)
		{
			gpuTimer.Next(gpuStages.TAA);
			commandList->beginMarker("MotionCopy");
			RecordMotionCopy(commandList, renderScene, !motionHistory);
			commandList->endMarker();
			m_MotionCamera = MotionCamera { frame.View, camera.Projection, camera.Position };
			if (m_TemporalFrame)
			{
				commandList->beginMarker("TAA");
				ResolveTemporal(commandList);
				commandList->endMarker();
				m_TemporalResolved = true;
			}
		}
		// Post-processing reads the resolved image with temporal anti-aliasing (the history's newest texture).
		const bool resolved = m_TemporalResolved;
		gpuTimer.Next(gpuStages.Post);
		commandList->beginMarker("Exposure");
		UpdateExposure(commandList, postProcess, resolved ? m_TemporalExposureSets[m_HistoryIndex].Get() : m_ExposureBindingSet.Get());
		commandList->endMarker();
		const bool bloom = postProcess.Bloom && postProcess.BloomIntensity > 0.0f;
		if (bloom)
		{
			commandList->beginMarker("Bloom");
			RenderBloom(commandList, postProcess, resolved ? m_TemporalBloomSets[m_HistoryIndex].Get() : m_BloomDownsampleSets[0].Get());
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
		tonemap.InversePreExposure = 1.0f / m_PreExposure;

		TextRenderStats textStats;
		m_FontAtlases->BeginFrame(TextRenderer::c_FrameRasterBudget);
		const bool text = m_TextRenderer->Prepare(scene, m_ViewportSize, options.DrawScreenSpaceText, camera.Position, commandList, textStats, options.ScreenView);
		m_Stats.Texts = textStats.Texts;
		m_Stats.ScreenSpaceTexts = textStats.ScreenSpaceTexts;
		m_Stats.HiddenScreenSpaceTexts = textStats.HiddenScreenSpaceTexts;
		m_Stats.TextGlyphs = textStats.Glyphs;
		m_Stats.PendingTextGlyphs = textStats.PendingGlyphs;
		m_Stats.RasterizedTextGlyphs = textStats.RasterizedGlyphs;
		const bool overlayPasses = PrepareOverlayPasses(scene, camera, options, commandList, gpuTimer);
		const AssetResolveStats& assetStats = m_Assets.GetStats();
		m_Stats.PendingAssets = assetStats.Pending + textStats.PendingFonts + m_Stats.Overlays.PendingAssets;
		m_Stats.RequestsByVisibility = assetStats.Requested;
		m_Stats.PrefetchRequests = assetStats.Prefetched;
		m_Stats.GetAssetCalls = assetStats.GetAssetCalls;

		// Overlays (and text) are composed in the output texture, which is then copied into the target. Without a target or
		// anything to compose, the frame needs no output texture: renderers that only render into targets (games without
		// text) never allocate one.
		const bool overlays = HasOverlays(options) || text || overlayPasses;
		if ((!target || overlays) && !EnsureOutputTarget())
		{
			gpuTimer.End();
			commandList->endMarker();
			commandList->close();
			m_Device->executeCommandList(commandList); // Keeps the uploads that were recorded
			if (m_GPUDrivenFrame)
				SubmitCullReadback();
			SubmitExposureReadback();
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

		// FXAA unless the image was resolved temporally.
		const bool fxaa = postProcess.AntiAliasing && !resolved;
		nvrhi::IFramebuffer* tonemapTarget = fxaa ? m_LDRFramebuffer.Get() : output;
		commandList->beginMarker("Tonemap");
		drawFullscreen(GetFullscreenPipeline(m_TonemapPipelines, m_TonemapPixelShader, m_TonemapBindingLayout, tonemapTarget->getFramebufferInfo()),
			tonemapTarget, resolved ? m_TemporalTonemapSets[m_HistoryIndex].Get() : m_TonemapBindingSet.Get(), &tonemap, sizeof(tonemap));
		commandList->endMarker();
		if (fxaa)
		{
			commandList->beginMarker("FXAA");
			drawFullscreen(GetFullscreenPipeline(m_FXAAPipelines, m_FXAAPixelShader, m_FXAABindingLayout, output->getFramebufferInfo()), output, m_FXAABindingSet, nullptr, 0);
			commandList->endMarker();
		}

		if (overlays)
		{
			gpuTimer.Next(gpuStages.Overlays);
			if (m_TemporalFrame)
			{
				// The overlays keep the camera's own matrices, so their lines and glyphs stay steady (the frame's depth they test
				// against is the jittered one, JitterParams).
				RenderData::FrameConstants overlayFrame = frame;
				overlayFrame.Projection = camera.Projection;
				overlayFrame.ViewProjection = camera.Projection * frame.View;
				overlayFrame.InverseProjection = glm::inverse(camera.Projection);
				overlayFrame.InverseViewProjection = glm::inverse(overlayFrame.ViewProjection);
				commandList->writeBuffer(m_FrameConstantBuffer, &overlayFrame, sizeof(overlayFrame));
			}
			commandList->beginMarker("Overlays");
			const bool overlaysDrawn = RenderOverlays(commandList, scene, camera, options, text, overlayPasses);
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
		if (m_GPUDrivenFrame)
			SubmitCullReadback();
		SubmitExposureReadback();
		CommitShadowCache();
		CommitLocalShadows();

		if (m_TemporalResolved)
			m_TemporalRender = m_RenderCount + 1;
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
		m_OutlineMaskPixelShader = shaders.Get("Overlay/OutlineMask.frag");
		m_StatusTintPixelShader = shaders.Get("Overlay/StatusTint.frag");
		m_DebugLineVertexShader = shaders.Get("Overlay/DebugLine.vert");
		m_DebugLinePixelShader = shaders.Get("Overlay/DebugLine.frag");
		m_CopyPixelShader = shaders.Get("PostProcess/Copy.frag");
		ST_CORE_VERIFY(m_GridPixelShader && m_OutlinePixelShader && m_OutlineMaskPixelShader && m_StatusTintPixelShader && m_DebugLineVertexShader && m_DebugLinePixelShader
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
		return options.ShowGrid || !options.SelectedEntities.empty() || (options.DebugShapes && !options.DebugShapes->IsEmpty()) || !options.EntityTints.empty()
			|| options.ShowMotionVectors;
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

	bool SceneRenderer::DrawMotionView(nvrhi::ICommandList* commandList, const nvrhi::Viewport& viewport)
	{
		if (!m_MotionViewPipeline)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.bindings = {
				nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::Texture_SRV(1)
			};
			m_MotionViewLayout = m_Device->createBindingLayout(layout);
			nvrhi::IShader* pixelShader = Renderer::GetShaderLibrary().Get("Overlay/MotionView.frag");
			ST_CORE_VERIFY(pixelShader && m_MotionViewLayout, "SceneRenderer: the motion view shader is missing");
			// Blended over the image without the depth attachment the other overlays test against: it reads the depth.
			nvrhi::BlendState::RenderTarget blend;
			blend.setBlendEnable(true)
				.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
				.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
				.setSrcBlendAlpha(nvrhi::BlendFactor::One)
				.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
			nvrhi::GraphicsPipelineDesc desc;
			desc.primType = nvrhi::PrimitiveType::TriangleList;
			desc.VS = m_FullscreenVertexShader;
			desc.PS = pixelShader;
			desc.bindingLayouts = { m_MotionViewLayout };
			desc.renderState.rasterState.setCullNone();
			desc.renderState.blendState.setRenderTarget(0, blend);
			desc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
			m_MotionViewPipeline = m_Device->createGraphicsPipeline(desc, m_OutputFramebuffer->getFramebufferInfo());
			ST_CORE_VERIFY(m_MotionViewPipeline, "SceneRenderer: failed to create the motion view pipeline");
		}
		if (!m_MotionViewBindingSet)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, m_FrameConstantBuffer),
				nvrhi::BindingSetItem::Texture_SRV(0, m_VelocityTexture),
				nvrhi::BindingSetItem::Texture_SRV(1, m_DepthTexture)
			};
			m_MotionViewBindingSet = m_Device->createBindingSet(desc, m_MotionViewLayout);
			if (!m_MotionViewBindingSet)
			{
				m_Errors.Report("failed to create the motion view's bindings");
				return false;
			}
		}
		nvrhi::GraphicsState state;
		state.pipeline = m_MotionViewPipeline;
		state.framebuffer = m_OutputFramebuffer;
		state.viewport.addViewportAndScissorRect(viewport);
		state.bindings = { m_MotionViewBindingSet };
		commandList->setGraphicsState(state);
		commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		return true;
	}

	void SceneRenderer::DrawDebugLines(nvrhi::ICommandList* commandList, DebugDrawDepth depth, uint32_t firstVertex, uint32_t vertexCount)
	{
		nvrhi::GraphicsState state;
		// Tested against a jittered depth (temporal anti-aliasing): with its bias.
		state.pipeline = depth == DebugDrawDepth::Tested && m_TemporalFrame ? m_JitteredDebugLinePipeline.Get() : m_DebugLinePipelines[static_cast<size_t>(depth)].Get();
		state.framebuffer = m_OverlayFramebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y)));
		state.bindings = { m_DebugLineBindingSet };
		state.vertexBuffers = { nvrhi::VertexBufferBinding { m_DebugLineBuffer, 0, 0 } };
		commandList->setGraphicsState(state);
		commandList->draw(nvrhi::DrawArguments().setVertexCount(vertexCount).setStartVertexLocation(firstVertex));
	}

	bool SceneRenderer::DrawSelectionMask(nvrhi::ICommandList* commandList)
	{
		if (!m_SelectionMaskTexture)
		{
			nvrhi::TextureHandle mask = CreateRenderTarget(m_Device, m_ViewportSize.x, m_ViewportSize.y, nvrhi::Format::R8_UNORM,
				m_Specification.DebugName + ".SelectionMask", false);
			nvrhi::FramebufferHandle framebuffer = mask ? m_Device->createFramebuffer(nvrhi::FramebufferDesc()
				.addColorAttachment(mask)
				.setDepthAttachment(nvrhi::FramebufferAttachment().setTexture(m_DepthTexture).setReadOnly(true))) : nullptr;
			if (!mask || !framebuffer)
			{
				m_Errors.Report(fmt::format("failed to create the selection mask for a {}x{} viewport", m_ViewportSize.x, m_ViewportSize.y));
				return false;
			}
			m_SelectionMaskTexture = std::move(mask);
			m_SelectionMaskFramebuffer = std::move(framebuffer);
			m_OutlineMaskBindingSet = nullptr; // Names the mask
		}
		commandList->clearTextureFloat(m_SelectionMaskTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		if (m_SelectionBatches.empty())
			return true;
		nvrhi::GraphicsState state;
		state.framebuffer = m_SelectionMaskFramebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(m_ViewportSize.x), static_cast<float>(m_ViewportSize.y)));
		state.bindings = { m_SceneBindingSet, Renderer::GetBindlessTextures().GetTable() };
		const uint32_t firstArgument = static_cast<uint32_t>(m_OpaqueBatches.size() + m_TransparentBatches.size() + m_ShadowBatches.size());
		DrawBuckets(commandList, state, m_SelectionBatches, firstArgument,
			[](const SceneDrawBatch& batch) { return PipelineKey { Pass::SelectionMask, batch.DoubleSided, batch.Mirrored, batch.Skinned }; }, nullptr, nullptr);
		return true;
	}

	bool SceneRenderer::PrepareOverlayPasses(Scene& scene, const SceneCamera& camera, const SceneRenderOptions& options, nvrhi::ICommandList* commandList,
		GPUPassTimer& gpuTimer)
	{
		SceneOverlayFrame frame;
		frame.OverlayScene = options.OverlayScene ? options.OverlayScene : &scene;
		// Camera-relative, as the scene's depth was drawn: world-space overlays keep their precision far from the origin.
		frame.ViewProjection = camera.Projection * GetRelativeView(camera);
		frame.CameraPosition = camera.Position;
		frame.HasCamera = frame.OverlayScene == &scene;
		frame.JitteredDepth = m_TemporalFrame;
		frame.ViewportSize = m_ViewportSize;
		frame.ScreenView = options.ScreenView;
		frame.DrawScreenSpace = options.DrawScreenSpaceText;
		frame.CommandList = commandList;
		frame.Atlases = m_FontAtlases.get();
		frame.VisibleTextures = &m_VisibleTextures;
		frame.Stats = &m_Stats.Overlays;
		frame.GPUTimer = &gpuTimer;
		bool draws = false;
		for (const Scope<ISceneOverlayPass>& pass : m_OverlayPasses)
			draws = pass->Prepare(frame) || draws;
		return draws;
	}

	bool SceneRenderer::HasOverlayContent(Scene& scene) const
	{
		for (const Scope<ISceneOverlayPass>& pass : m_OverlayPasses)
		{
			if (pass->HasScreenContent(scene))
				return true;
		}
		return false;
	}

	bool SceneRenderer::RenderOverlays(nvrhi::ICommandList* commandList, Scene& scene, const SceneCamera& camera, const SceneRenderOptions& options, bool drawText,
		bool drawOverlayPasses)
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
			desc.PS = m_OutlineMaskPixelShader;
			m_OutlineMaskPipeline = m_Device->createGraphicsPipeline(desc, info);

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
			SetJitteredDepthBias(desc.renderState.rasterState);
			m_JitteredDebugLinePipeline = m_Device->createGraphicsPipeline(desc, info);
			ST_CORE_VERIFY(m_GridPipeline && m_OutlinePipeline && m_OutlineMaskPipeline && m_StatusTintPipeline && m_DebugLinePipelines[0] && m_DebugLinePipelines[1]
				&& m_JitteredDebugLinePipeline, "SceneRenderer: failed to create the overlay pipelines");
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

		// The motion view and the tints color the scene itself: the other overlays go over them. Without motion vectors (they
		// could not be made: reported) there is no motion view.
		if (options.ShowMotionVectors && m_MotionFrame && !DrawMotionView(commandList, viewport))
			return false;
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
			// Camera-relative, as the scene's shaders draw (the subtraction in double for lines far from the origin).
			m_DebugLineScratch.assign(tested.begin(), tested.end());
			m_DebugLineScratch.insert(m_DebugLineScratch.end(), onTop.begin(), onTop.end());
			const glm::dvec3 cameraPosition(camera.Position);
			for (DebugLineVertex& vertex : m_DebugLineScratch)
				vertex.Position = glm::vec3(glm::dvec3(vertex.Position) - cameraPosition);
			if (!m_DebugLineScratch.empty())
				commandList->writeBuffer(m_DebugLineBuffer, m_DebugLineScratch.data(), m_DebugLineScratch.size() * sizeof(DebugLineVertex), 0);
			testedVertices = static_cast<uint32_t>(tested.size());
			onTopVertices = static_cast<uint32_t>(onTop.size());
			m_Stats.DebugLines = (testedVertices + onTopVertices) / 2;
		}
		if (testedVertices > 0)
			DrawDebugLines(commandList, DebugDrawDepth::Tested, 0, testedVertices);
		if (drawText)
			m_TextRenderer->Draw(commandList, m_OverlayFramebuffer, false, m_TemporalFrame);
		// The game UI placed in the scene, with the world-space text.
		if (drawOverlayPasses)
		{
			for (const Scope<ISceneOverlayPass>& pass : m_OverlayPasses)
				pass->DrawWorld(commandList, m_OverlayFramebuffer);
		}

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
			// The jittered entity IDs would move the outline by a pixel from frame to frame: it follows the selection mask.
			const bool masked = m_TemporalFrame;
			if (masked)
			{
				if (!DrawSelectionMask(commandList))
					return false;
				if (recreated || !m_OutlineMaskBindingSet)
				{
					nvrhi::BindingSetDesc desc;
					desc.bindings = {
						nvrhi::BindingSetItem::Texture_SRV(0, m_SelectionMaskTexture),
						nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_SelectionBuffer),
						nvrhi::BindingSetItem::PushConstants(0, sizeof(RenderData::OutlineParameters))
					};
					m_OutlineMaskBindingSet = m_Device->createBindingSet(desc, m_OutlineBindingLayout);
					if (!m_OutlineMaskBindingSet)
					{
						m_Errors.Report("failed to create the selection outline bindings");
						return false;
					}
				}
			}

			RenderData::OutlineParameters parameters = {};
			parameters.Color = options.SelectionColor;
			parameters.Width = static_cast<int32_t>(std::clamp(options.OutlineWidth, 1u, 8u));
			parameters.SelectedCount = static_cast<uint32_t>(m_SelectionIDs.size());
			nvrhi::GraphicsState state;
			state.pipeline = masked ? m_OutlineMaskPipeline : m_OutlinePipeline;
			state.framebuffer = m_OverlayFramebuffer;
			state.viewport.addViewportAndScissorRect(viewport);
			state.bindings = { masked ? m_OutlineMaskBindingSet : m_OutlineBindingSet };
			commandList->setGraphicsState(state);
			commandList->setPushConstants(&parameters, sizeof(parameters));
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
			m_Stats.OutlinedEntities = parameters.SelectedCount;
		}

		if (onTopVertices > 0)
			DrawDebugLines(commandList, DebugDrawDepth::OnTop, testedVertices, onTopVertices);
		if (drawText)
			m_TextRenderer->Draw(commandList, m_OverlayFramebuffer, true, false); // The HUD goes over everything
		// The game UI over the HUD text.
		if (drawOverlayPasses)
		{
			for (const Scope<ISceneOverlayPass>& pass : m_OverlayPasses)
				pass->Draw(commandList, m_OverlayFramebuffer);
		}
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
