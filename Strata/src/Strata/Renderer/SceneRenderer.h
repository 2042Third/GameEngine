#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Renderer/SceneRenderData.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Material;
	class Mesh;
	class Scene;
	class Texture;
	struct Submesh;

	// The view a scene is rendered from.
	struct SceneCamera
	{
		glm::mat4 View = glm::mat4(1.0f);
		glm::mat4 Projection = glm::mat4(1.0f); // Strata clip space: reversed-Z, depth [0, 1]
		glm::vec3 Position = glm::vec3(0.0f);
		float Near = 0.1f;
		float Far = 1000.0f;
		// Vertical field of view in radians (perspective) or vertical extent in world units (orthographic); used
		// for level-of-detail selection.
		float VerticalFOV = glm::radians(60.0f);
		bool Orthographic = false;
		// Background where nothing is drawn and no sky is shown, as an sRGB-encoded color. It becomes part of the HDR
		// image (linearized), so exposure, tone mapping and grading apply to it like to the rest of the scene: it shows
		// unchanged only with neutral post-processing (manual exposure 0, no tone curve, no vignette).
		glm::vec4 ClearColor = { 0.05f, 0.05f, 0.07f, 1.0f };

		// Camera of a scene entity with a CameraComponent (view from its world transform).
		static SceneCamera FromEntity(const Scene& scene, Entity entity, float aspectRatio);
	};

	struct SceneRendererSpecification
	{
		std::string DebugName = "SceneRenderer";
		// Screen fraction (projected bounding sphere diameter / viewport height) below which each further mesh LOD is
		// used: LOD n is chosen while the fraction is below LODThreshold / 2^(n-1).
		float LODThreshold = 0.5f;
		// Point and spot lights rendered per frame (after frustum culling). When more are visible, the most relevant ones
		// are kept: lights whose range contains the camera, then by the projected size of their range.
		uint32_t MaxLights = 4096;
		// Clustered light assignment: the view is split into LightClusterGrid.x by .y screen tiles and .z depth slices
		// (exponentially spaced). Each cluster lists up to MaxLightsPerCluster lights, the most relevant ones when more
		// reach it. A 1x1x1 grid shades every pixel with every visible light.
		glm::uvec3 LightClusterGrid = { 16, 9, 24 };
		uint32_t MaxLightsPerCluster = 256;
		uint32_t ShadowMapSize = 2048; // Resolution of each directional shadow cascade
		uint32_t ShadowCascades = 4;   // 1-4
	};

	struct SceneRendererStats
	{
		static constexpr size_t c_LODStatCount = 8;

		bool Rendered = false; // False when the last Render call drew nothing (see Render)
		uint32_t DrawCalls = 0;
		uint32_t Instances = 0;       // Drawn submesh instances
		uint32_t CulledInstances = 0; // Mesh renderers outside the view frustum
		uint64_t Triangles = 0;
		// Drawn submesh instances per selected level of detail; the last entry also counts all higher levels.
		std::array<uint32_t, c_LODStatCount> InstancesPerLOD = {};
		uint32_t Lights = 0;          // Rendered lights: the directional light plus the point and spot lights
		uint32_t CulledLights = 0;    // Point and spot lights outside the view frustum
		uint32_t DroppedLights = 0;   // Visible point and spot lights beyond SceneRendererSpecification::MaxLights
		uint32_t Materials = 0;
		uint32_t PendingAssets = 0;   // Meshes, materials or textures still loading (drawn with fallbacks or skipped)
		bool EnvironmentLighting = false; // A sky light's environment map lights the scene
		uint32_t ShadowCasters = 0;       // Instances drawn into shadow cascades (summed over cascades)
	};

	// Renders a scene: depth/normal/entity-id prepass, ground-truth ambient occlusion, forward physically based
	// shading into an HDR target (directional light with cascaded soft shadows, clustered point and spot lights,
	// image-based lighting from a sky light's HDR environment map), the environment background and transparent
	// surfaces; then post-processing from the scene's PostProcessComponent: automatic exposure, bloom, tone mapping,
	// color grading and FXAA into a display target. Meshes, materials and textures come from the active asset manager
	// and render as soon as they are loaded (fallback textures and the default material are used meanwhile).
	// Main thread only.
	class SceneRenderer
	{
	public:
		explicit SceneRenderer(const SceneRendererSpecification& specification = {});
		~SceneRenderer();

		SceneRenderer(const SceneRenderer&) = delete;
		SceneRenderer& operator=(const SceneRenderer&) = delete;

		// Sizes the internal render targets. A size beyond the device's texture limit (or a failed allocation) is
		// logged, and Render then fails until a valid size is set.
		void SetViewportSize(uint32_t width, uint32_t height);
		glm::uvec2 GetViewportSize() const { return m_ViewportSize; }

		// Renders the scene into the output texture, or into `target` when given. A target must have the viewport size
		// (the renderer never rescales) and a single-sampled, non-sRGB UNORM color attachment 0, e.g. the swapchain:
		// values are written sRGB-encoded. Returns false, with GetStats().Rendered false, when nothing was rendered: an
		// empty viewport, or an invalid target or render targets (both logged). Updates the scene's cached world
		// transforms.
		bool Render(Scene& scene, const SceneCamera& camera, nvrhi::IFramebuffer* target = nullptr);

		// RGBA8 UNORM with sRGB-encoded values (display-ready), valid after Render without a target.
		nvrhi::ITexture* GetOutputTexture() const { return m_OutputTexture; }
		nvrhi::ITexture* GetHDRTexture() const { return m_HDRTexture; }
		nvrhi::ITexture* GetDepthTexture() const { return m_DepthTexture; }
		nvrhi::ITexture* GetNormalTexture() const { return m_NormalTexture; }
		nvrhi::ITexture* GetEntityIDTexture() const { return m_EntityIDTexture; }
		nvrhi::ITexture* GetAmbientOcclusionTexture() const { return m_AOTexture; } // 1 = unoccluded
		// Image-based lighting data of the current environment (null without one); the BRDF lookup table always exists.
		nvrhi::ITexture* GetEnvironmentCube() const { return m_Environment.Cube; }
		nvrhi::ITexture* GetIrradianceMap() const { return m_Environment.Irradiance; }
		nvrhi::ITexture* GetPrefilteredMap() const { return m_Environment.Prefiltered; }
		nvrhi::ITexture* GetBRDFLut() const { return m_BRDFLut; }

		nvrhi::ITexture* GetBloomTexture() const { return m_BloomTexture; } // Level 0 of the bloom chain (half resolution)

		// Entity visible at a pixel of the last rendered frame (invalid when none, or when no frame was rendered since
		// the last resize). Blocks until the GPU is idle; meant for editor clicks and tests.
		Entity GetEntityAt(Scene& scene, uint32_t x, uint32_t y);

		// Automatic exposure adapts gradually; this makes the next frame jump to the scene's brightness (camera cuts,
		// scene loads).
		void ResetExposureAdaptation() { m_ResetExposure = true; }

		const SceneRendererSpecification& GetSpecification() const { return m_Specification; }
		const SceneRendererStats& GetStats() const { return m_Stats; }
	private:
		enum class Pass : uint8_t
		{
			Prepass = 0,
			Opaque,
			Transparent,
			Shadow,
			ShadowMasked
		};

		struct PipelineKey
		{
			Pass PassType = Pass::Opaque;
			bool DoubleSided = false;
			bool Mirrored = false; // Instances with a mirroring transform: front faces wind clockwise
			bool operator==(const PipelineKey&) const = default;
		};

		struct ShadowLight
		{
			bool Enabled = false;
			glm::vec3 Direction = { 0.0f, -1.0f, 0.0f };
			float Distance = 80.0f;
			float Softness = 1.0f;
			float Bias = 1.0f;
			float NormalBias = 1.5f;
		};

		struct DrawBatch
		{
			const Mesh* MeshAsset = nullptr;
			uint32_t SubmeshIndex = 0;
			uint32_t LOD = 0;
			uint32_t MaterialIndex = 0;
			bool DoubleSided = false;
			bool Mirrored = false;
			bool AlphaMask = false; // Shadow batches: alpha-tested casters
			uint32_t Cascade = 0;   // Shadow batches
			uint32_t FirstInstance = 0;
			uint32_t InstanceCount = 0;
		};

		// One submesh instance gathered for drawing, sorted into batches. The instance payload stays in a scratch list
		// so sorting moves only these small records.
		struct DrawRecord
		{
			const Mesh* MeshAsset = nullptr;
			uint32_t SubmeshIndex = 0;
			uint32_t LOD = 0;
			uint32_t MaterialIndex = 0;
			uint32_t PayloadIndex = 0;
			float ViewDepth = 0.0f; // Transparent surfaces are drawn back to front
			uint32_t Cascade = 0;   // Shadow casters
			bool DoubleSided = false;
			bool Mirrored = false;
			bool Transparent = false;
			bool AlphaMask = false;
		};

		// Material properties of one submesh of a shadow caster, resolved once per entity.
		struct CasterMaterial
		{
			uint32_t MaterialIndex = 0;
			bool DoubleSided = false;
			bool AlphaMask = false;
			bool Casts = false;
		};

		struct LightCandidate
		{
			RenderData::LightData Data;
			glm::vec4 Bounds; // World-space bounding sphere of the lit region (xyz: center, w: radius)
			float Relevance = 0.0f;
			uint32_t Order = 0; // Registry order, which breaks ties between equally relevant lights
		};

		void CreateResources();
		void CreateIBLResources();
		// Recreates the viewport-sized targets; false (targets released, error logged) when they cannot be created.
		bool CreateRenderTargets();
		void ReleaseRenderTargets();
		// Logs a rendering failure, once until a frame renders successfully again (avoids a message every frame).
		void ReportError(const std::string& message);
		bool ValidateTarget(nvrhi::IFramebuffer* target);
		// Finds the scene's sky light; (re)computes the environment maps when its environment texture changed.
		void UpdateEnvironment(Scene& scene, RenderData::FrameConstants& frame, nvrhi::ICommandList* commandList);
		void ProcessEnvironment(nvrhi::ICommandList* commandList, const Ref<Texture>& source);
		void DispatchIBL(nvrhi::ICommandList* commandList, nvrhi::IComputePipeline* pipeline, nvrhi::ITexture* source, const nvrhi::TextureSubresourceSet& sourceSubresources,
			nvrhi::TextureDimension sourceDimension, nvrhi::ITexture* output, uint32_t outputMip, nvrhi::TextureDimension outputDimension,
			const RenderData::IBLParameters& parameters, uint32_t layers);
		bool CreateSceneBindingSets();
		void RenderAmbientOcclusion(nvrhi::ICommandList* commandList, const PostProcessComponent& settings);
		// Meters the HDR image (automatic exposure) or writes the manual exposure into the exposure buffer.
		void UpdateExposure(nvrhi::ICommandList* commandList, const PostProcessComponent& settings);
		void RenderBloom(nvrhi::ICommandList* commandList, const PostProcessComponent& settings);
		// The scene's first active post-processing settings, or the defaults.
		static PostProcessComponent GetPostProcessSettings(Scene& scene);
		nvrhi::IGraphicsPipeline* GetScenePipeline(const PipelineKey& key);
		// Fullscreen-triangle pipeline for a pixel shader and target format, cached per format.
		nvrhi::IGraphicsPipeline* GetFullscreenPipeline(std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>>& cache, nvrhi::IShader* pixelShader,
			nvrhi::IBindingLayout* layout, const nvrhi::FramebufferInfo& framebufferInfo);
		// Grows a structured buffer to hold elementCount elements; false when the allocation failed.
		bool EnsureBufferCapacity(nvrhi::BufferHandle& buffer, size_t elementCount, size_t elementSize, const char* name, bool& outRecreated);

		uint32_t GetMaterialIndex(const Ref<Material>& material);
		void CountPendingAsset(AssetHandle handle);
		uint32_t ResolveTexture(AssetHandle handle, uint32_t missingSlot, uint32_t loadingSlot);
		void CollectLights(Scene& scene, const SceneCamera& camera, RenderData::FrameConstants& frame);
		void CollectDraws(Scene& scene, const SceneCamera& camera);
		// Computes the cascades and gathers the shadow casters of each; leaves ShadowParams.w at 0 without shadows.
		void CollectShadowCasters(Scene& scene, const SceneCamera& camera, RenderData::FrameConstants& frame);
		// Sorts gathered records into instanced batches (transparent ones back to front, one per record, into
		// outTransparentBatches); the instance payloads are copied in batch order.
		static void BuildBatches(std::vector<DrawRecord>& records, const std::vector<RenderData::InstanceData>& payloads, std::vector<DrawBatch>& outBatches,
			std::vector<DrawBatch>* outTransparentBatches, std::vector<RenderData::InstanceData>& outInstances, bool shadows);
		bool EnsureShadowMap();
		uint32_t SelectLOD(const Submesh& submesh, float screenFraction) const;
		static float GetScreenFraction(const SceneCamera& camera, const glm::vec3& center, float radius);
		void DrawBatches(nvrhi::ICommandList* commandList, Pass pass, bool transparent);
		void DrawShadows(nvrhi::ICommandList* commandList, uint32_t cascadeCount);
		void BuildLightClusters(nvrhi::ICommandList* commandList);
	private:
		SceneRendererSpecification m_Specification;
		nvrhi::IDevice* m_Device = nullptr;
		nvrhi::CommandListHandle m_CommandList;
		glm::uvec2 m_ViewportSize = { 0, 0 };
		bool m_TargetsValid = false;
		bool m_HasRenderedFrame = false; // Since the render targets were (re)created
		std::string m_LastError;
		bool m_WarnedLightOverflow = false;
		SceneRendererStats m_Stats;

		nvrhi::ShaderHandle m_MeshVertexShader;
		nvrhi::ShaderHandle m_PrepassPixelShader;
		nvrhi::ShaderHandle m_ForwardPixelShader;
		nvrhi::ShaderHandle m_ForwardTransparentPixelShader;
		nvrhi::ShaderHandle m_FullscreenVertexShader;
		nvrhi::ShaderHandle m_TonemapPixelShader;
		nvrhi::ShaderHandle m_SkyboxPixelShader;
		nvrhi::InputLayoutHandle m_InputLayout;
		nvrhi::GraphicsPipelineHandle m_SkyboxPipeline;

		// Image-based lighting
		struct EnvironmentMaps
		{
			Ref<Texture> Source;                 // Equirectangular HDR texture the maps were computed from
			nvrhi::TextureHandle Cube;           // Radiance cubemap with mips (background, irradiance source)
			nvrhi::TextureHandle Irradiance;     // Diffuse irradiance
			nvrhi::TextureHandle Prefiltered;    // Specular radiance, one mip per roughness level
		};
		EnvironmentMaps m_Environment;
		nvrhi::ITexture* m_BoundEnvironment = nullptr; // Environment cube referenced by m_SceneBindingSet
		nvrhi::BindingLayoutHandle m_IBLBindingLayout;
		nvrhi::ComputePipelineHandle m_EquirectToCubePipeline;
		nvrhi::ComputePipelineHandle m_CubeDownsamplePipeline;
		nvrhi::ComputePipelineHandle m_IrradiancePipeline;
		nvrhi::ComputePipelineHandle m_PrefilterPipeline;
		nvrhi::TextureHandle m_BRDFLut;
		nvrhi::SamplerHandle m_LinearClampSampler;
		nvrhi::SamplerHandle m_EquirectSampler;

		// Directional shadows
		ShadowLight m_ShadowLight;
		nvrhi::TextureHandle m_ShadowMap;      // Depth array, one layer per cascade (created on first use)
		nvrhi::TextureHandle m_DummyShadowMap; // Bound while no shadows are rendered
		nvrhi::ITexture* m_BoundShadowMap = nullptr;
		std::vector<nvrhi::FramebufferHandle> m_ShadowFramebuffers;
		nvrhi::BufferHandle m_ShadowInstanceBuffer;
		nvrhi::BindingSetHandle m_ShadowBindingSet; // Scene bindings with the shadow instances
		nvrhi::BindingLayoutHandle m_ShadowPushLayout;
		nvrhi::BindingSetHandle m_ShadowPushBindingSet;
		nvrhi::SamplerHandle m_ShadowCompareSampler;
		nvrhi::ShaderHandle m_ShadowVertexShader;
		nvrhi::ShaderHandle m_ShadowMaskPixelShader;
		std::vector<RenderData::InstanceData> m_ShadowInstances;
		std::vector<DrawBatch> m_ShadowBatches;

		// Clustered lighting
		nvrhi::BindingLayoutHandle m_ClusterBindingLayout;
		nvrhi::BindingSetHandle m_ClusterBindingSet;
		nvrhi::ComputePipelineHandle m_ClusterPipeline;
		nvrhi::BufferHandle m_LightBoundsBuffer; // View-space bounding spheres, parallel to m_LightBuffer
		nvrhi::BufferHandle m_ClusterBuffer;     // Per cluster: light count, then light indices (Include/Clusters.glsl)

		nvrhi::BindingLayoutHandle m_SceneBindingLayout;
		nvrhi::BindingLayoutHandle m_TonemapBindingLayout;
		nvrhi::BindingSetHandle m_SceneBindingSet;
		nvrhi::BindingSetHandle m_TonemapBindingSet;
		std::vector<nvrhi::SamplerHandle> m_MaterialSamplers;
		nvrhi::SamplerHandle m_PointClampSampler;

		nvrhi::BufferHandle m_FrameConstantBuffer;
		nvrhi::BufferHandle m_InstanceBuffer;
		nvrhi::BufferHandle m_MaterialBuffer;
		nvrhi::BufferHandle m_LightBuffer;

		nvrhi::TextureHandle m_DepthTexture;
		nvrhi::TextureHandle m_NormalTexture;
		nvrhi::TextureHandle m_EntityIDTexture;
		nvrhi::TextureHandle m_HDRTexture;
		nvrhi::TextureHandle m_OutputTexture;
		nvrhi::TextureHandle m_AORawTexture; // Noisy GTAO output
		nvrhi::TextureHandle m_AOTexture;    // Blurred
		nvrhi::BindingLayoutHandle m_AOBindingLayout;
		nvrhi::BindingSetHandle m_GTAOBindingSet;
		nvrhi::BindingSetHandle m_AOBlurBindingSet;
		nvrhi::ComputePipelineHandle m_GTAOPipeline;
		nvrhi::ComputePipelineHandle m_AOBlurPipeline;
		nvrhi::FramebufferHandle m_PrepassFramebuffer;
		nvrhi::FramebufferHandle m_ForwardFramebuffer;
		nvrhi::FramebufferHandle m_OutputFramebuffer;

		// Post-processing
		nvrhi::BindingLayoutHandle m_ExposureBindingLayout;
		nvrhi::BindingSetHandle m_ExposureBindingSet;
		nvrhi::ComputePipelineHandle m_HistogramPipeline;
		nvrhi::ComputePipelineHandle m_ExposurePipeline;
		nvrhi::BufferHandle m_HistogramBuffer;
		nvrhi::BufferHandle m_ExposureBuffer; // See Include/Exposure.glsl
		bool m_ResetExposure = true;
		std::optional<std::chrono::steady_clock::time_point> m_LastExposureTime;
		nvrhi::BindingLayoutHandle m_BloomBindingLayout;
		nvrhi::ComputePipelineHandle m_BloomPipeline;
		nvrhi::TextureHandle m_BloomTexture;
		uint32_t m_BloomLevels = 0;
		std::vector<nvrhi::BindingSetHandle> m_BloomDownsampleSets; // [0]: scene color -> level 0, [n]: level n-1 -> n
		std::vector<nvrhi::BindingSetHandle> m_BloomUpsampleSets;   // [n]: level n+1 -> n
		nvrhi::ShaderHandle m_FXAAPixelShader;
		nvrhi::BindingLayoutHandle m_FXAABindingLayout;
		nvrhi::BindingSetHandle m_FXAABindingSet;
		nvrhi::TextureHandle m_LDRTexture; // Tone-mapped image before anti-aliasing
		nvrhi::FramebufferHandle m_LDRFramebuffer;

		std::vector<std::pair<PipelineKey, nvrhi::GraphicsPipelineHandle>> m_ScenePipelines;
		std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>> m_TonemapPipelines;
		std::vector<std::pair<nvrhi::FramebufferInfo, nvrhi::GraphicsPipelineHandle>> m_FXAAPipelines;

		// Per-frame gathered data (kept between frames to reuse the allocations)
		std::vector<RenderData::InstanceData> m_Instances;
		std::vector<RenderData::MaterialData> m_Materials;
		std::vector<RenderData::LightData> m_Lights;
		std::vector<RenderData::LightBounds> m_LightBounds;
		std::vector<LightCandidate> m_LightCandidates;
		std::vector<DrawRecord> m_DrawRecords;
		std::vector<DrawRecord> m_ShadowRecords;
		std::vector<RenderData::InstanceData> m_InstancePayloads;
		std::vector<RenderData::InstanceData> m_ShadowPayloads;
		std::vector<CasterMaterial> m_CasterMaterials;
		std::vector<DrawBatch> m_OpaqueBatches;
		std::vector<DrawBatch> m_TransparentBatches;
		std::unordered_map<const Material*, uint32_t> m_MaterialIndices;
		std::vector<Ref<Material>> m_FrameMaterials; // Keep this frame's assets alive while batches point at them
		std::vector<Ref<Mesh>> m_FrameMeshes;
	};

}
