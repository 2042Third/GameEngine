#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace Strata
{

	// GPU data layouts shared with shaders/Include/Scene.glsl. Every struct is std140/std430 compatible (16-byte
	// aligned members only); keep both sides in sync.
	namespace RenderData
	{

		constexpr uint32_t c_MaterialAlphaMask = 1u;
		constexpr uint32_t c_MaterialAlphaBlend = 2u;
		constexpr uint32_t c_MaterialUnlit = 4u;
		constexpr uint32_t c_MaterialDoubleSided = 8u;

		// InstanceData::Flags
		constexpr uint32_t c_InstanceMirrored = 1u; // Negative determinant: winding and tangent handedness are flipped
		constexpr uint32_t c_InstanceSkinned = 2u;  // Posed: the skinned pipelines blend its palette (InstanceData::SkinPalette)

		constexpr uint32_t c_LightPoint = 0u;
		constexpr uint32_t c_LightSpot = 1u;

		constexpr uint32_t c_SamplerCount = 6; // TextureFilter (2) x TextureWrap (3)

		// Texture table entries (TextureTable::PackEntry): the bindless slot in bits 0-23, the sampler index in bits 24-30 and
		// this bit for one-channel textures that stand for gray (TextureSpecification::Grayscale), sampled as (r, r, r, 1).
		constexpr uint32_t c_TextureSlotGrayscale = 1u << 31;

		constexpr uint32_t c_MaxCascades = 4;

		struct FrameConstants
		{
			glm::mat4 View;
			glm::mat4 Projection;
			glm::mat4 ViewProjection;
			glm::mat4 InverseView;
			glm::mat4 InverseProjection;
			glm::mat4 InverseViewProjection;
			// What motion vectors measure positions with: this frame's view-projection without a temporal jitter, and the
			// renderer's previous frame's, which takes positions relative to this frame's camera to that frame's clip space.
			glm::mat4 UnjitteredViewProjection;
			glm::mat4 PreviousViewProjection;
			glm::vec4 CameraPosition;            // xyz: world position, w: near clip distance
			glm::vec4 CameraForward;             // xyz: world view direction, w: 1 for orthographic projections
			glm::vec4 ViewportSize;              // xy: size in pixels, zw: 1 / size
			glm::vec4 DirectionalLightDirection; // xyz: direction the light travels, w: 1 when the light exists
			glm::vec4 DirectionalLightColor;     // rgb: color * intensity
			glm::vec4 AmbientColor;
			glm::vec4 EnvironmentParams;         // x: intensity, y: rotation (radians), z: specular mip count, w: enabled
			glm::uvec4 LightCounts;              // x: punctual lights in the light buffer
			glm::uvec4 ClusterGrid;              // xyz: light clusters along screen x, screen y and depth, w: lights per cluster
			glm::vec4 ClusterDepth;              // Depth slice = log(view depth) * x + y; z, w: near and far view depth
			glm::vec4 TimeParams;                // x: scene time in seconds
			glm::vec4 SkyParams;                 // x: background mip (blur), y: 1 when the environment is the background
			glm::vec4 CascadeSplits;             // View distance where each cascade ends (their projections: CascadeData)
			glm::vec4 ShadowParams;              // x: 2 tan(light angular radius), y: 1 / map size, z: cascade blend, w: cascade count
			glm::vec4 ShadowBias;                // x: depth bias (texels), y: normal offset (texels)
			glm::vec4 AOParams;                  // x: 1 when screen-space ambient occlusion is available
			glm::vec4 MaterialParams;            // x: 1 when raised texture levels fade in (SceneRendererSpecification::FadeTextureLevels)
			// x: 1 when the motion history holds the renderer's previous frame; y: the pre-exposure, the power of two every writer of
			// the HDR target multiplies its luminance by (SceneRendererStats::PreExposure); z: the frame's rotation of per-pixel noise
			// (PCSS, ambient occlusion) in turns, 0 without temporal anti-aliasing
			glm::vec4 TemporalParams;
			// Temporal anti-aliasing's offset of the frame's samples in clip units (added to Projection, ViewProjection and their
			// inverses; 0 without it): xy this frame's, zw the renderer's previous frame's
			glm::vec4 JitterParams;
			// Height fog (HeightFogComponent; FogParams.x is 0 without one, and the scene shaders skip it):
			glm::vec4 FogDensity;  // x: Density at BaseHeight (per meter), y: HeightFalloff, z: the camera's height above BaseHeight, w: StartDistance
			glm::vec4 FogColor;    // rgb: Color (the scattering's albedo), w: MaxOpacity
			// rgb: the light the fog scatters besides the sun's (the procedural sky's HorizonColor times its intensity, the
			// ambient color without an environment, 1 when ColorFromSky is off), w: 1 when an environment map's irradiance from
			// above lights it instead, 2 when the atmosphere's sky at the horizon does (toward the ray's azimuth)
			glm::vec4 FogLight;
			glm::vec4 FogParams;   // x: 1 when the scene has fog, y: the asymmetry of its scattering of the directional light
			// The atmosphere's sun disk (SkyLightSource::Atmosphere; Scene/Skybox.frag's atmosphere variant): xyz toward the sun,
			// w its angular radius (0: no disk); rgb its illuminance at the ground in lux, through the atmosphere.
			glm::vec4 SunDiskDirection;
			glm::vec4 SunDiskColor;
		};
		static_assert(sizeof(FrameConstants) == 8 * 64 + 25 * 16);

		// A directional shadow cascade as shaders read it (u_Cascades, Include/Scene.glsl): written on the GPU by
		// Scene/CascadeFit.comp, which fits its depth range to the casters (CascadeFitConstants).
		struct CascadeData
		{
			glm::mat4 ViewProjection;           // Light clip space, reversed-Z
			// x: world units per shadow texel, y: depth range in world units, z: penumbra search distance, w: the near plane's light
			// view z (camera-relative: the cascades' occlusion culling places depths with it)
			glm::vec4 Parameters = glm::vec4(0.0f);
		};
		static_assert(sizeof(CascadeData) == 80);

		// What Scene/CascadeFit.comp fits the cascades from (a constant buffer, std140): each cascade's square in light view
		// space and the bounding sphere of its view slice, and the nearest caster toward the light, which the CPU path finds
		// and the GPU-driven path's culling does (u_CasterDepths).
		struct CascadeFitConstants
		{
			glm::mat4 LightView = glm::mat4(1.0f);
			// Light view space relative to the camera's position in it (the shaders' positions are camera-relative): min.xy, max.xy
			glm::vec4 Boxes[c_MaxCascades] = {};
			glm::vec4 Depths[c_MaxCascades] = {};    // x: the far side's z, y: the sphere's near side's z, z: world units per texel, w: the sphere's radius
			glm::vec4 CasterNearZ = glm::vec4(0.0f); // Per cascade: the nearest drawn caster's (relative) z, as the CPU found it
			uint32_t CascadeCount = 0;
			uint32_t CasterDepthsFromGPU = 0;        // 1: the nearest casters come from the GPU's culling instead of CasterNearZ
			float MaxCasterDistance = 0.0f;          // How far beyond its sphere a cascade's depth range may reach toward the light
			uint32_t Reserved = 0;
		};
		static_assert(sizeof(CascadeFitConstants) == 64 + 9 * 16 + 16);

		// The upper three rows of an affine world matrix, whose fourth row is (0, 0, 0, 1). Shaders transform points with
		// one dot product per row and derive the normal matrix from the rows (no inverse on the CPU).
		struct AffineTransform
		{
			glm::vec4 Rows[3];

			static AffineTransform FromMatrix(const glm::mat4& matrix)
			{
				AffineTransform transform;
				for (int row = 0; row < 3; row++)
					transform.Rows[row] = glm::vec4(matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]);
				return transform;
			}

			glm::mat4 ToMatrix() const
			{
				glm::mat4 matrix(1.0f);
				for (int column = 0; column < 4; column++)
				{
					for (int row = 0; row < 3; row++)
						matrix[column][row] = Rows[row][column];
				}
				return matrix;
			}
		};
		static_assert(sizeof(AffineTransform) == 48);

		// One drawn submesh of a render proxy (RenderScene). The buffer of instances persists between frames; draws read
		// it through the frame's draw list, which holds an entry (MakeDrawEntry) per drawn instance.
		struct InstanceData
		{
			AffineTransform World;
			uint32_t MaterialIndex; // Material slot (RenderScene::GetMaterialHandle), index into the material buffer
			uint32_t EntityID;      // Entity identifier + 1 (0 = none)
			uint32_t Flags;         // c_Instance*
			// Posed instances (c_InstanceSkinned): the first joint matrix of the proxy's palette in the joint buffer
			// (RenderScene::GetPalette, uploaded where it changed), one per joint of the mesh's skin; 0 otherwise.
			uint32_t SkinPalette;
		};
		static_assert(sizeof(InstanceData) == 64);

		// A draw list entry (the u_DrawList the scene's vertex shaders read): the instance in the low c_DrawEntryInstanceBits, and
		// above them the level-of-detail cross-fade code of the draw (LODSelection::UpdateLODFade): 0 draws every pixel, 1 to
		// c_LODFadeFrames - 1 the level fading in (on the pixels whose LODSelection::GetLODFadeThreshold is below the code), the
		// same plus c_LODFadeOutgoing the level fading out (on the others). Render scenes hold at most c_MaxRenderInstances
		// instances.
		constexpr uint32_t c_DrawEntryInstanceBits = 27;
		constexpr uint32_t c_DrawEntryInstanceMask = (1u << c_DrawEntryInstanceBits) - 1;
		constexpr uint32_t c_MaxRenderInstances = 1u << c_DrawEntryInstanceBits;
		constexpr uint32_t c_LODFadeFrames = 16; // A fade's steps: the 4x4 dither's 16 thresholds
		constexpr uint32_t c_LODFadeOutgoing = 16;
		static_assert(c_LODFadeOutgoing + c_LODFadeFrames <= (1u << (32 - c_DrawEntryInstanceBits)));

		constexpr uint32_t MakeDrawEntry(uint32_t instance, uint32_t fadeCode)
		{
			return instance | (fadeCode << c_DrawEntryInstanceBits);
		}

		struct MaterialData
		{
			glm::vec4 BaseColor;
			glm::vec4 Emissive;
			float Metallic;
			float Roughness;
			float NormalScale;
			float OcclusionStrength;
			float AlphaCutoff;
			uint32_t Flags;
			// The maps: texture table indices (TextureTable), whose entries hold the bindless slot, the sampler and the grayscale bit.
		uint32_t BaseColorMap;
			uint32_t MetallicRoughnessMap;
			glm::vec4 UVTransform; // xy: tiling, zw: offset
			uint32_t NormalMap;
			uint32_t OcclusionMap;
			uint32_t EmissiveMap;
			uint32_t Padding;
		};
		static_assert(sizeof(MaterialData) == 96);

		struct LightData
		{
			glm::vec4 PositionRange; // xyz: camera-relative world position (as uploaded), w: range
			glm::vec4 Color;         // rgb: color * intensity, w: 1 / range^2
			glm::vec4 DirectionType; // xyz: direction the light travels, w: type
			glm::vec4 SpotAngles;    // x: cos(inner), y: cos(outer), z: its shadow slot plus one (0: no shadow; LocalShadowData)
		};
		static_assert(sizeof(LightData) == 64);

		// A point or spot light of a scene's render lights (RenderLights; mirrored by RenderSceneGPU), in world space: the GPU
		// culls the records into the light buffer (Scene/LightCull.comp and LightCompact.comp: LightData, camera-relative,
		// and LightBounds).
		struct LightRecord
		{
			glm::vec4 PositionRange = glm::vec4(0.0f); // xyz: world position, w: range
			glm::vec4 Color = glm::vec4(0.0f);         // rgb: color * intensity, w: 1 / range^2
			glm::vec4 DirectionType = glm::vec4(0.0f); // xyz: direction the light travels, w: type
			// x: cos(inner), y: cos(outer), z: c_LightRecordDrawn or 0, w: c_LightRecordShadowed or 0
			glm::vec4 SpotAngles = glm::vec4(0.0f);
			glm::vec4 Bounds = glm::vec4(0.0f);        // xyz: world center, w: radius of the region it reaches
		};
		static_assert(sizeof(LightRecord) == 80);
		// LightRecord::SpotAngles.z of a light that is drawn (active, lit, with a range).
		constexpr float c_LightRecordDrawn = 1.0f;
		// LightRecord::SpotAngles.w of a drawn light that casts shadows (its component's CastShadows).
		constexpr float c_LightRecordShadowed = 1.0f;

		// Shadows of point and spot lights (SceneRenderer's local shadows): at most this many lights per view have them, a spot
		// light with one view of the light's atlas, a point light with six (a cube's faces: +X, -X, +Y, -Y, +Z, -Z).
		constexpr uint32_t c_MaxShadowedLocalLights = 16;
		constexpr uint32_t c_LocalShadowFaces = 6;
		// A shadowed light as the shaders read it (u_LocalShadows, Include/LocalShadows.glsl), by its shadow slot: the shadow
		// pass draws view `slot * c_LocalShadowFaces + face` with its matrix, the forward pass samples the views' tiles.
		struct LocalShadowData
		{
			glm::mat4 ViewProjection[c_LocalShadowFaces]; // Camera-relative world space to the view's clip space (reversed-Z)
			// Per view: xy its tile's corner and z its size in atlas UV, w the world size of a texel at unit depth along the view
			// (2 tan(half its field of view) / its tile's texels)
			glm::vec4 Tiles[c_LocalShadowFaces];
			// x: depth bias, y: normal bias (texels of the light's tiles), z: filter radius (texels), w: strength (0: unshadowed,
			// 1: full shadow; between while it fades)
			glm::vec4 Parameters;
			glm::vec4 Light; // xyz: the light's camera-relative position, w: 1 for a point light (six views), 0 for a spot light
		};
		static_assert(sizeof(LocalShadowData) == c_LocalShadowFaces * 64 + c_LocalShadowFaces * 16 + 2 * 16);

		// Reflection probes: a renderer keeps at most this many in view,
		// each capture converted into a slot of its cube array (a prefiltered cube, one level per roughness step, c_ReflectionProbeSize
		// texels across) with its irradiance as spherical-harmonic coefficients.
		constexpr uint32_t c_MaxReflectionProbes = 16;
		constexpr uint32_t c_ReflectionProbeSize = 128;
		constexpr uint32_t c_ReflectionProbeMipCount = 6; // Roughness 0, 0.2, ... 1.0, as the sky's prefiltered map
		constexpr uint32_t c_ReflectionProbeSHCoefficients = 9; // Bands 0 to 2
		// A probe in view as the forward pass reads it, in the frame's list (strongest first: priority, then the smaller box).
		struct ReflectionProbeData
		{
			glm::vec4 CenterBlend; // xyz: the box's camera-relative center (where it was captured), w: its blend distance (meters)
			// The box's unit axes in world space, w: its half extent along each (meters)
			glm::vec4 AxisX;
			glm::vec4 AxisY;
			glm::vec4 AxisZ;
			glm::vec4 Parameters; // x: intensity, y: 1 for box-projected lookups, z: its slot of the cube array, w: unused
		};
		static_assert(sizeof(ReflectionProbeData) == 5 * 16);
		// The probe buffer (scene set SRV 10): the slots' irradiance, written on the GPU when a capture is converted, then the
		// frame's list, written by the CPU every frame.
		struct ReflectionProbeBuffer
		{
			// Per slot, coefficients of the irradiance over pi (what the sky's irradiance map holds) on the real spherical
			// harmonics of bands 0 to 2, band by band; w unused.
			glm::vec4 Irradiance[c_MaxReflectionProbes * c_ReflectionProbeSHCoefficients];
			glm::uvec4 Counts; // x: probes in the list
			ReflectionProbeData Probes[c_MaxReflectionProbes];
		};
		static_assert(sizeof(ReflectionProbeBuffer) == c_MaxReflectionProbes * c_ReflectionProbeSHCoefficients * 16 + 16 + c_MaxReflectionProbes * 80);
		// Where the CPU's part of the buffer starts.
		constexpr size_t c_ReflectionProbeListOffset = offsetof(ReflectionProbeBuffer, Counts);

		// The push constants of the GPU's light culling passes (Include/Lights.glsl).
		struct LightCullParameters
		{
			glm::vec4 Planes[6] = {};   // The view frustum in camera-relative space, inward (Frustum::GetPlanes)
			uint32_t RecordCount = 0;   // Light records of the scene's GPU copy
			uint32_t GroupCount = 0;    // Work groups of c_LightGroupSize records
			uint32_t Padding[2] = {};
		};
		static_assert(sizeof(LightCullParameters) == 112);
		constexpr uint32_t c_LightGroupSize = 64; // ST_LIGHT_GROUP_SIZE

		// Bounding sphere of a light's influence in view space, input of the light cluster assignment.
		struct LightBounds
		{
			glm::vec4 CenterRadius; // xyz: view-space center, w: radius
		};
		static_assert(sizeof(LightBounds) == 16);

		// Push constants shared by the image-based lighting compute shaders.
		struct IBLParameters
		{
			uint32_t OutputSize;
			uint32_t SourceSize;
			float Roughness;
			uint32_t SampleCount;
		};

		// IBL/ProceduralSky.comp: what the procedural environment cube is computed from. The renderer compares it with the
		// parameters of the current cube and regenerates the cube only when they differ.
		struct ProceduralSkyParameters
		{
			glm::vec4 ZenithColor;  // rgb: linear radiance straight up
			glm::vec4 HorizonColor; // rgb: linear radiance at the horizon
			glm::vec4 GroundColor;  // rgb: linear radiance below the horizon
			glm::vec4 SunDirection; // xyz: unit direction toward the sun, w: angular radius of its disk in radians
			glm::vec4 SunRadiance;  // rgb: linear radiance of the disk
			glm::uvec4 Size;        // x: face size of the cube in texels

			bool operator==(const ProceduralSkyParameters&) const = default;
		};
		static_assert(sizeof(ProceduralSkyParameters) == 96);

		// IBL/Atmosphere*.comp (Include/AtmospherePass.glsl): the atmosphere's medium (Atmosphere::GetMedium), the sun and the
		// viewer the tables are computed for.
		struct AtmosphereConstants
		{
			glm::vec4 RayleighScattering; // rgb: per kilometer
			glm::vec4 MieScattering;
			glm::vec4 MieExtinction;
			glm::vec4 OzoneAbsorption;
			glm::vec4 GroundAlbedo;       // rgb
			glm::vec4 SunDirection;       // xyz: toward the sun
			glm::vec4 SunColor;           // rgb: the sun's color over its largest component
			glm::vec4 Viewer;             // x: the viewer's distance from the planet's center (km)
		};
		static_assert(sizeof(AtmosphereConstants) == 8 * 16);

		struct AtmospherePassParameters
		{
			uint32_t FirstFace; // IBL/AtmosphereCube.comp: the cube face of the dispatch's first layer
			uint32_t FaceSize;
			uint32_t Padding0;
			uint32_t Padding1;
		};
		static_assert(sizeof(AtmospherePassParameters) == 16);

		struct AOParameters
		{
			float Radius;
			float Intensity;
			uint32_t SliceCount;
			uint32_t StepsPerSide;
		};

		struct TonemapParameters
		{
			float BloomIntensity;
			float BloomScale;
			float Saturation;
			float Vignette;
			float Contrast;
			int32_t Operator;
			uint32_t BloomAdditive; // 1: the bloom holds only light above a threshold and is added instead of mixed
			float InversePreExposure; // 1 / the pre-exposure the scene color carries (FrameConstants::TemporalParams.y)
		};
		static_assert(sizeof(TonemapParameters) == 32);

		// Include/Exposure.glsl: the exposure buffer and the luminance histogram.
		constexpr uint32_t c_ExposureLogLuminance = 0;
		constexpr uint32_t c_ExposureMultiplier = 1;
		constexpr uint32_t c_HistogramBins = 256;
		constexpr float c_HistogramMinLog2 = -12.0f;
		constexpr float c_HistogramMaxLog2 = 16.0f;

		struct ExposureParameters
		{
			float MinLog2;
			float MaxLog2;
			float AdaptFactor;
			float Compensation;
			float LowPercentile;
			float HighPercentile;
			float InversePreExposure; // 1 / the pre-exposure the scene color carries: the histogram meters luminance
			float Padding;
		};
		static_assert(sizeof(ExposureParameters) == 32);

		// Overlay/Grid.frag
		struct GridParameters
		{
			glm::vec4 MinorColor;
			glm::vec4 MajorColor;
			glm::vec4 AxisXColor;
			glm::vec4 AxisZColor;
			float Spacing;
			float MajorEvery;
			float FadeDistance;
			float Padding;
		};
		static_assert(sizeof(GridParameters) == 80);

		// Overlay/Outline.frag
		struct OutlineParameters
		{
			glm::vec4 Color;
			int32_t Width;
			uint32_t SelectedCount;
			uint32_t Padding0;
			uint32_t Padding1;
		};
		static_assert(sizeof(OutlineParameters) == 32);

		// Overlay/StatusTint.frag
		struct StatusTintParameters
		{
			uint32_t Count; // Entries of the tint buffer
			uint32_t Padding0;
			uint32_t Padding1;
			uint32_t Padding2;
		};
		static_assert(sizeof(StatusTintParameters) == 16);

		// Overlay/Text.vert
		struct TextParameters
		{
			glm::vec2 ViewportSize;
			uint32_t ScreenSpace; // 1: positions are pixels (+Y down), 0: world space
			uint32_t Padding;
		};
		static_assert(sizeof(TextParameters) == 16);

		constexpr uint32_t c_BloomPrefilter = 0;
		constexpr uint32_t c_BloomDownsample = 1;
		constexpr uint32_t c_BloomUpsample = 2;
		constexpr uint32_t c_MaxBloomLevels = 6;

		struct BloomParameters
		{
			float Threshold;
			float Knee;
			uint32_t Mode;
			float InversePreExposure; // 1 / the pre-exposure the scene color carries
		};
		static_assert(sizeof(BloomParameters) == 16);

		// PostProcess/TAA.comp: how the frame's image and the temporal history are blended.
		struct TemporalParameters
		{
			float CurrentWeight;     // 1 / the frames the history holds with this one (1: the history is not read)
			float MinWeight;         // The current frame's least weight
			float MotionWeightScale; // The current frame's weight per pixel of motion...
			float MaxMotionWeight;   // ...up to this
			// The frame's pre-exposure over the history's (a power of two, exact): the history is brought to the frame's scale.
			float HistoryScale;
			float InversePreExposure; // 1 / the frame's pre-exposure: the luminance weights are of exposed values
			float Padding0;
			float Padding1;
		};
		static_assert(sizeof(TemporalParameters) == 32);

		// Scene/MotionCopy.comp: copies the listed elements of a buffer into a renderer's motion history.
		struct MotionCopyParameters
		{
			uint32_t First;       // The dispatch's first list entry
			uint32_t Count;       // List entries in all
			uint32_t StrideWords; // 32-bit words per element
			uint32_t ListOffset;  // The list's first entry in the list buffer
		};
		static_assert(sizeof(MotionCopyParameters) == 16);

	}

}
