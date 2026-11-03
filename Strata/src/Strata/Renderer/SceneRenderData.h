#pragma once

// The post-processing passes' data (tone mapping, exposure, bloom) lives in its own header.
#include "Strata/Renderer/PostProcessData.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace Strata
{

	// GPU data layouts shared with shaders/Include/Scene.slang. Every struct is std140/std430 compatible (16-byte
	// aligned members only); keep both sides in sync.
	namespace RenderData
	{

		constexpr uint32_t c_MaterialAlphaMask = 1u;
		constexpr uint32_t c_MaterialAlphaBlend = 2u;
		constexpr uint32_t c_MaterialUnlit = 4u;
		constexpr uint32_t c_MaterialDoubleSided = 8u;
		constexpr uint32_t c_MaterialClearcoat = 16u;    // Clearcoat > 0 (and lit): the coat's lobe is evaluated
		constexpr uint32_t c_MaterialTransmissive = 32u; // Transmission > 0 (and lit): drawn as glass

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
			// x: tan(light angular radius), the penumbra's half-width per unit of separation; y: 1 / map size, z: cascade blend, w:
			// cascade count
			glm::vec4 ShadowParams;
			// x: depth bias (texels), y: normal offset (texels), z: the wide penumbrae's taps (RendererQualitySettings::
			// ShadowFilterQuality), w: unused (0)
			glm::vec4 ShadowBias;
			// x: 1 when screen-space ambient occlusion is available; y: 1 when occlusion's multi-bounce applies
			// (PostProcessComponent::AmbientOcclusionMultiBounce)
			glm::vec4 AOParams;
			// x: 1 when raised texture levels fade in (SceneRendererSpecification::FadeTextureLevels); y: the lighting debug view
			// (SceneLightingView as a float, 0: none); z: specular anti-aliasing's strength (1 with
			// SceneRendererSpecification::SpecularAntiAliasing, else 0); w: 1 in a reflection probe's capture (the lights' specular
			// left out, SceneRenderOptions::ProbeCapture), else 0
			glm::vec4 MaterialParams;
			// x: 1 when the motion history holds the renderer's previous frame; y: the pre-exposure, the power of two every writer of
			// the HDR target multiplies its luminance by (SceneRendererStats::PreExposure); z: the frame's rotation of per-pixel noise
			// (local shadows' PCF, the checkerboard screen-space reflections) in turns, 0 without temporal anti-aliasing
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
			// x: 1 when the scene has fog, y: the asymmetry of its scattering of the directional light (SetHeightFog); z: aerial
			// perspective's far distance in meters (0: none), w: its distance scale (SetAerialPerspective; light-and-materials
			// claims z and w)
			glm::vec4 FogParams;
			// The atmosphere's sun disk (SkyLightSource::Atmosphere; Scene/Skybox.frag's atmosphere variant): xyz toward the sun,
			// w its angular radius (0: no disk); rgb its illuminance at the ground in lux, through the atmosphere.
			glm::vec4 SunDiskDirection;
			glm::vec4 SunDiskColor;
			// x: 1 when the frame traced screen-space reflections (SRV 18 holds them); y: 1 when the frame's indirect light
			// comes from the radiance cascades (SRV 35), z: their intensity (radiance-cascades); w: reflections'. y, z and w
			// are written 0 until those streams use them.
			glm::vec4 ReflectionParams;
		};
		static_assert(sizeof(FrameConstants) == 8 * 64 + 26 * 16);

		// A directional shadow cascade as shaders read it (u_Cascades, Include/Scene.slang): written on the GPU by
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
			uint32_t ClearcoatMap;
			float Clearcoat;           // 0 without a coat (c_MaterialClearcoat)
			float ClearcoatRoughness;
			float Transmission;        // 0 when not transmissive (c_MaterialTransmissive)
			float IOR;
			float Thickness;           // In the mesh's units
			uint32_t ClearcoatRoughnessMap;
			uint32_t ClearcoatNormalMap;
			uint32_t TransmissionMap;
			// rgb: what the volume absorbs per world unit (Beer's law: -ln(AttenuationColor) / AttenuationDistance; 0 absorbs
			// nothing), w: unused.
			glm::vec4 Absorption;
		};
		static_assert(sizeof(MaterialData) == 144);

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
		// A shadowed light as the shaders read it (u_LocalShadows, Include/LocalShadows.slang), by its shadow slot: the shadow
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

		// Reflection probes (ReflectionProbeComponent; Include/ReflectionProbes.slang): a renderer keeps at most this many in
		// view, each capture converted into a slot of its cube array (a prefiltered cube, one level per roughness step,
		// c_ReflectionProbeSize texels across) with its irradiance as spherical-harmonic coefficients.
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
			// harmonics of bands 0 to 2, in the order of Include/ReflectionProbes.slang's ProbeSHBasis; w unused.
			glm::vec4 Irradiance[c_MaxReflectionProbes * c_ReflectionProbeSHCoefficients];
			glm::uvec4 Counts; // x: probes in the list
			ReflectionProbeData Probes[c_MaxReflectionProbes];
		};
		static_assert(sizeof(ReflectionProbeBuffer) == c_MaxReflectionProbes * c_ReflectionProbeSHCoefficients * 16 + 16 + c_MaxReflectionProbes * 80);
		// Where the CPU's part of the buffer starts.
		constexpr size_t c_ReflectionProbeListOffset = offsetof(ReflectionProbeBuffer, Counts);
		// IBL/ProbeIrradiance.comp: projects a slot's capture (a level of its cube, SourceSize texels across) onto the slot's
		// irradiance coefficients.
		struct ProbeIrradianceParameters
		{
			uint32_t SourceSize = 0;
			uint32_t Slot = 0;
			uint32_t Padding[2] = {};
		};
		static_assert(sizeof(ProbeIrradianceParameters) == 16);

		// The push constants of the GPU's light culling passes (Include/Lights.slang).
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

		// IBL/Atmosphere*.comp (Include/AtmospherePass.slang): the atmosphere's medium (Atmosphere::GetMedium), the sun and the
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

		// Aerial perspective's volume (IBL/AtmosphereAerial.comp): columns across the screen each way, and depth slices (the
		// texture holds two slabs of them: the in-scattered light, then the transmittance).
		constexpr uint32_t c_AerialSize = 32;
		constexpr uint32_t c_AerialSlices = 32;
		constexpr float c_AerialMaxDistance = 32000.0f; // Meters: the volume reaches at most this far (or the camera's far plane)
		// IBL/AtmosphereAerial.comp's push constants: the view rays through the screen's corners, unjittered, in camera-relative
		// world space (not normalized: they interpolate linearly across the screen), and what the march needs.
		struct AerialPerspectiveParameters
		{
			glm::vec4 RayTopLeft;     // w: the volume's far distance D (meters)
			glm::vec4 RayTopRight;    // w: the distance scale
			glm::vec4 RayBottomLeft;  // w: the viewer's distance from the planet's center (km): bottom radius + clamp(camera y, 0, 20 km)
			glm::vec4 RayBottomRight; // w: unused
		};
		static_assert(sizeof(AerialPerspectiveParameters) == 64);

		// Effects/GTAO.comp's and Effects/AODenoise.comp's push constants.
		struct AOParameters
		{
			float Radius;            // Meters (PostProcessComponent::AmbientOcclusionRadius, clamped to [0.01, 10])
			float Intensity;         // Exponent of the visibility ([0, 8])
			uint32_t SliceCount;     // The quality's (RendererQuality::GetAmbientOcclusionSamples)
			uint32_t StepsPerSide;
			float MinRadiusPixels;   // max(c_AOMinRadiusFraction * viewport height, 1.5): the radius on screen at least...
			float MaxRadiusPixels;   // ...and at most c_AOMaxRadiusFraction * viewport height
			float ThinOccluderDecay; // 1 - 0.9^(6 / StepsPerSide): how far a horizon relaxes toward a lower sample per step
			uint32_t Padding;
		};
		static_assert(sizeof(AOParameters) == 32);
		// Ambient occlusion's radius on screen, as fractions of the viewport's height: a far surface keeps at least 0.3% (6.5
		// pixels at 2160 rows: distant creases keep a line), a near one at most 12% (259 pixels at 2160 rows; 0.6.0 capped it at
		// 256 pixels at every size), so its cost and look do not depend on the resolution.
		constexpr float c_AOMinRadiusFraction = 0.003f;
		constexpr float c_AOMaxRadiusFraction = 0.12f;
		constexpr float c_AOMinRadiusPixels = 1.5f;

		// Effects/ScreenSpaceReflections.comp: the trace of a frame's screen-space reflections.
		struct ScreenReflectionParameters
		{
			float Intensity;    // PostProcessComponent::ScreenSpaceReflectionsIntensity: the confidence's scale
			float MaxRoughness; // PostProcessComponent::ScreenSpaceReflectionsMaxRoughness: rougher surfaces reflect nothing
			float HistoryScale; // The frame's pre-exposure over the TAA history's (what the trace fetches is the previous frame)
			uint32_t Frame;     // The TAA history's frame count: which pixel of each 2x2 block traces, and the steps' offset
		};
		static_assert(sizeof(ScreenReflectionParameters) == 16);

		// Effects/ScreenSpaceReflectionsSpatial.comp's push constants (RendererQualitySettings::Reflections Spatial, SpatialHigh).
		struct SpatialReflectionParameters
		{
			float Intensity;        // As ScreenReflectionParameters'
			float MaxRoughness;
			float HistoryScale;     // The frame's pre-exposure over the previous frame's color copy's
			uint32_t Steps;         // Steps of the march along the ray (RendererQuality::GetScreenReflectionSteps)
			uint32_t RefineSteps;   // Bisections of a hit
			uint32_t ColorLevels;   // Levels of the half-resolution color copy
			uint32_t MaxCandidates = 1; // Refined candidates a ray may reject before it finds nothing
			uint32_t Padding = 0;
		};
		static_assert(sizeof(SpatialReflectionParameters) == 32);

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

		// PostProcess/TAA.comp: how the frame's image and the temporal history are blended (TemporalAntiAliasingSettings).
		struct TemporalParameters
		{
			float CurrentWeight;     // 1 / the frames the history holds with this one (1: the history is not read)
			float MinWeight;         // The current frame's least weight: 1 / HistoryFrames
			float MotionWeightScale; // The current frame's weight per pixel of motion...
			float MaxMotionWeight;   // ...up to this
			// The frame's pre-exposure over the history's (a power of two, exact): the history is brought to the frame's scale.
			float HistoryScale;
			float InversePreExposure; // 1 / the frame's pre-exposure: the luminance weights are of exposed values
			uint32_t ClampMode;       // TemporalHistoryClamp
			float ClipWidth;          // Variance clipping's half width, in standard deviations
			float TransparentWeight;  // The current frame's least weight under blended transparent surfaces; 0 = off
			float DisocclusionThreshold; // A surface nearer by this fraction of the distance in the previous frame covered the pixel; 0 = off
			float PreviousDepthOffset;   // dot(camera position - the previous camera's, PreviousForward)
			float ResampleWeight;        // The current frame's least weight halfway between the history's texels; 0 = off
			glm::vec4 PreviousForward;   // xyz: the previous camera's forward; w: the depth scale of the rejection's margin
		};
		static_assert(sizeof(TemporalParameters) == 64 && offsetof(TemporalParameters, PreviousForward) == 48);
		// The history's alpha holds the pixel's signed view distance within this (the largest half float), the sky's.
		constexpr float c_TemporalSkyDistance = 65504.0f;

		// Scene/MotionCopy.comp: copies the listed elements of a buffer into a renderer's motion history.
		struct MotionCopyParameters
		{
			uint32_t First;       // The dispatch's first list entry
			uint32_t Count;       // List entries in all
			uint32_t StrideWords; // 32-bit words per element
			uint32_t ListOffset;  // The list's first entry in the list buffer
		};
		static_assert(sizeof(MotionCopyParameters) == 16);

		// The shadows debug view (SceneRenderOptions::ShowShadowView, Include/ShadowView.slang): screen pixels per shadow texel,
		// binned per cascade in eighths of a stop over [2^c_ShadowViewMinStops, 2^(c_ShadowViewMinStops + c_ShadowViewBins / 8)):
		// bin b covers [2^(b / 8 - 4), 2^((b + 1) / 8 - 4)), and ratios outside the range count in the first or last bin.
		constexpr uint32_t c_ShadowViewBins = 64;
		constexpr uint32_t c_ShadowViewBinsPerStop = 8;
		constexpr int32_t c_ShadowViewMinStops = -4;

		// Overlay/ShadowView.frag: the ramp's colors at 1 (or fewer), 2 and 4 (or more) screen pixels per shadow texel.
		struct ShadowViewParameters
		{
			glm::vec4 Colors[3];
		};
		static_assert(sizeof(ShadowViewParameters) == 48);

		// Debug/ShadowViewStats.comp's result: per cascade the receiver pixels of each bin, and the frame's surface pixels
		// (anything drawn: receivers in a cascade and those outside every one), sky pixels (nothing drawn) and surface pixels
		// outside every cascade.
		struct ShadowViewCounts
		{
			uint32_t Bins[c_MaxCascades][c_ShadowViewBins];
			uint32_t SurfacePixels;
			uint32_t SkyPixels;
			uint32_t UncoveredPixels;
			uint32_t Reserved;
		};
		static_assert(sizeof(ShadowViewCounts) == 1040);

		// The sun shadow cascades' depth ranges (Scene/ShadowRanges.comp, Include/Shadows.slang): levels of cells of
		// c_ShadowRangeCellTexels << level shadow texels each way, the nearest and farthest depth of each packed in 16 bits.
		constexpr uint32_t c_ShadowRangeLevels = 3;
		constexpr uint32_t c_ShadowRangeCellTexels = 4;

		// Scene/ShadowRanges.comp: one level's sizes, and the cascade layers it builds (the others keep their ranges).
		struct ShadowRangeParameters
		{
			glm::uvec2 SourceSize;
			glm::uvec2 DestinationSize;
			uint32_t LayerMask;
			uint32_t Reserved[3];
		};
		static_assert(sizeof(ShadowRangeParameters) == 32);

		// Effects/ContactShadows.frag: the sun's contact shadows (the pass does not run while ContactSteps is 0).
		struct ScreenShadowParameters
		{
			float ContactLength;         // Meters toward the sun
			uint32_t ContactSteps;
			float ContactMaxPixels;      // The march's longest reach on screen
			float ContactFadeStart;      // View distances over which contact shadows fade out
			float ContactFadeEnd;
			float ContactThickness;      // Meters behind a surface that still occlude...
			float ContactThicknessScale; // ...plus this much per meter of view distance
			uint32_t Reserved;
		};
		static_assert(sizeof(ScreenShadowParameters) == 32);

		// Effects/ScreenShadowsBlur.frag: one direction of the mask's blur.
		struct ScreenShadowBlurParameters
		{
			int32_t DirectionX;
			int32_t DirectionY;
			float PlaneTolerance; // How far from a pixel's plane a tap may lie, as a fraction of its view distance
			uint32_t Reserved;
		};
		static_assert(sizeof(ScreenShadowBlurParameters) == 16);

		// Scene/ShadowFit.comp: the receivers the frame's depth holds, binned by view distance in quarter stops from
		// c_ShadowFitBinBase (bin = floor(4 log2(d / c_ShadowFitBinBase)), clamped to the bins: 6.25 cm to 65 km), for the
		// cascades' fit to what the camera sees (ShadowFitTracker, Renderer/ShadowFit.h).
		constexpr uint32_t c_ShadowFitBins = 80;
		constexpr float c_ShadowFitBinBase = 0.0625f;
		constexpr uint32_t c_ShadowFitBinsPerStop = 4;

		// A bin's receivers as a box in the camera's view space: x and y of their positions and their view distance d, each an
		// EncodeOrderedFloat (Include/OrderedFloat.slang), and how many pixels hold them. The minima are stored complemented, so
		// every box field reduces with atomicMax (the count with atomicAdd) and a buffer cleared to 0 holds empty bins (MaxD 0:
		// no float encodes to 0).
		struct ShadowFitBin
		{
			uint32_t MinX;
			uint32_t MinY;
			uint32_t MinD;
			uint32_t MaxX;
			uint32_t MaxY;
			uint32_t MaxD;
			uint32_t Pixels;
		};
		static_assert(sizeof(ShadowFitBin) == 28);

		// Scene/ShadowFit.comp's result: the nearest (complemented) and farthest view distance of any receiver, how many pixels
		// hold one, and the bins.
		struct ShadowFitResult
		{
			uint32_t Nearest;
			uint32_t Farthest;
			uint32_t SurfacePixels;
			uint32_t Reserved;
			ShadowFitBin Bins[c_ShadowFitBins];
		};
		static_assert(sizeof(ShadowFitResult) == 2256);

		// Scene/ShadowFit.comp's push constants: the depth's size in pixels.
		struct ShadowFitParameters
		{
			uint32_t Width;
			uint32_t Height;
			uint32_t Reserved[2];
		};
		static_assert(sizeof(ShadowFitParameters) == 16);
		// The lighting debug views' statistics (Effects/LightingViewStats.comp; SceneRenderer::GetLightingViewStats): histograms of
		// a frame's EV100 (log2 of its luminance in nits, plus 3) in quarter-EV bins from c_LightingViewMinEV100 on, and of the
		// value views' values (occlusion, the albedo's luminance) in bins of 1/64 over [0, 1].
		constexpr uint32_t c_LightingViewEVBins = 128;
		constexpr float c_LightingViewMinEV100 = -8.0f;
		constexpr float c_LightingViewBinsPerEV = 4.0f;
		constexpr uint32_t c_LightingViewValueBins = 64;
		// The albedo view marks base colors whose luminance lies outside [c_AlbedoLow, c_AlbedoHigh]: darker than charcoal or
		// brighter than fresh snow, which no measured material is.
		constexpr float c_AlbedoLow = 0.03f;
		constexpr float c_AlbedoHigh = 0.85f;
		// Effects/LightingViewStats.comp's output, cleared before each dispatch (one 32-bit word per entry; Include/LightingView.slang
		// names the offsets).
		struct LightingViewStatsData
		{
			uint32_t SurfaceEV[c_LightingViewEVBins];    // Pixels with a surface (a depth was drawn)
			uint32_t BackgroundEV[c_LightingViewEVBins]; // The others (the sky or the clear color)
			uint32_t Value[c_LightingViewValueBins];     // Surface pixels of the value views
			uint32_t SurfacePixels;
			uint32_t BackgroundPixels;
			uint32_t BelowRange; // Albedo: surface pixels below c_AlbedoLow
			uint32_t AboveRange; // Albedo: surface pixels above c_AlbedoHigh
			// rgb: the sky's light on a horizontal surface over pi: the irradiance map at +Y times EnvironmentParams.x, or the
			// constant ambient without an environment
			glm::vec4 SkyIrradiance;
		};
		static_assert(sizeof(LightingViewStatsData) == (2 * c_LightingViewEVBins + c_LightingViewValueBins + 4) * 4 + 16);
		constexpr uint32_t c_LightingViewLuminanceColors = 12;
		// Push constants (b1) of Overlay/LightingView.frag and Effects/LightingViewStats.comp.
		struct LightingViewParameters
		{
			uint32_t View;             // SceneLightingView
			float InversePreExposure;  // 1 / the frame's pre-exposure (TemporalParams.y): the HDR target holds luminance times it
			float LuminanceFirstEV100; // The luminance view's first band's lower edge
			uint32_t Padding0;
			uint32_t LuminanceColors[c_LightingViewLuminanceColors]; // RGBA8, sRGB, packed (red in the low byte)
			uint32_t AlbedoLow;
			uint32_t AlbedoHigh;
			uint32_t Background;
			uint32_t Padding1;
		};
		static_assert(sizeof(LightingViewParameters) == 80);

		// Global illumination: world-space radiance cascades over a voxel clipmap (Renderer/GlobalIlluminationLayout.h,
		// VoxelClipmap.h; Docs/DeveloperGuide.md, "Global illumination"). Include/GlobalIllumination.slang mirrors them as ST_GI_*.
		constexpr uint32_t c_GIScrollVoxels = 4;        // A level's window moves in steps of this many voxels: fewer, larger slabs
		constexpr uint32_t c_GILevelGuardVoxels = 2;    // A level is never sampled this close to its window's (or valid box's) faces
		constexpr uint32_t c_GILevelBlendVoxels = 8;    // Samples blend into the next level over this many voxels (by distance from the camera)
		constexpr uint32_t c_GIRegionBrick = 4;         // Changed boxes snap outward to bricks of this many voxels
		constexpr uint32_t c_GIMaxRegionVoxels = 64;    // A region is at most this many voxels a side (slabs and windows are cut so)
		// Windows are addressed from an anchor (a multiple of c_GIAnchorVoxels voxels), kept while the camera stays within
		// c_GIAnchorMeters of it: what voxelizes relative to the same anchor gives the same voxels whatever the camera.
		constexpr int64_t c_GIAnchorVoxels = 1024;
		constexpr double c_GIAnchorMeters = 1000.0;
		constexpr uint32_t c_GIMaxVoxelLevels = 4;
		// Voxelization (VoxelClipmap, GI/Voxel*.comp): what a frame voxelizes, within its budget of triangles.
		constexpr uint32_t c_GIVoxelizeTrianglesPerFrame = 500000; // Guess: the voxelization's share of the stage at most 0.5 ms on the 5090
		constexpr uint32_t c_GIVoxelizeTrianglesOnCut = 4000000;   // Guess: a rebuild's or a jump's frame (Open World's whole scene)
		constexpr uint32_t c_GIMaxWorkItems = 65536;               // Work items a frame (VoxelWorkItem)
		constexpr uint32_t c_GIMaxTriangleGroups = 65536;          // Groups of c_GIGroupTriangles triangles a frame (VoxelBatch)
		constexpr uint32_t c_GIGroupTriangles = 64;
		constexpr uint32_t c_GIMaxRegions = 4096;                  // Regions a frame
		constexpr uint32_t c_GISmallTriangleVoxels = 64; // A triangle whose bounds within its region hold more voxels goes to tiles
		constexpr uint32_t c_GITileVoxels = 8;           // A tile is 8^3 voxels, a group of 64 threads with 8 voxels each
		constexpr uint32_t c_GIMaxTiles = 32768;         // Tiles per batch; beyond them a triangle walks its tiles itself (counted)
		constexpr uint32_t c_GIMovingFrames = 30;        // A proxy placed anew within this many frames is moving: left out of the voxels
		constexpr float c_GIMaskedOccupancy = 0.15f;     // Guess: alpha-masked surfaces' occupancy
		constexpr float c_GITwoSidedSum = 0.5f;          // A voxel whose triangles' unit normals sum to less is two-sided
		constexpr float c_GINormalFixedPoint = 256.0f;   // Normals add as integers times this
		constexpr float c_GIStepVoxels = 0.75f;          // Marches step this many voxels of the level they sample

		// VoxelWorkItem::Flags
		constexpr uint32_t c_VoxelItemClosed = 1u << 0;      // A closed mesh whose submeshes are all opaque: its triangles add winding steps
		constexpr uint32_t c_VoxelItemAlphaMasked = 1u << 1; // Voxels of class 1 (c_GIMaskedOccupancy)
		constexpr uint32_t c_VoxelItemDoubleSided = 1u << 2; // Lit from either side; adds no normal
		constexpr uint32_t c_VoxelItemMirrored = 1u << 3;    // Negative determinant: normals and winding steps reverse
		constexpr uint32_t c_VoxelItemFillOnly = 1u << 4;    // Outside the region: only its winding steps (closed meshes below it)

		// A box of voxels the frame voxelizes, contiguous in its level's textures (Include/Voxelize.slang).
		struct VoxelRegion
		{
			glm::ivec4 Minimum; // xyz: the region's first voxel, relative to the level's anchor voxel; w: the level
			glm::ivec4 Maximum; // xyz: one past its last voxel; w: its first voxel in the batch's scratch
			glm::ivec4 Texel;   // xyz: the texel of Minimum; w: its first column (x, z) among the batch's
		};
		static_assert(sizeof(VoxelRegion) == 48);

		// A submesh of a proxy instance voxelized into a region.
		struct VoxelWorkItem
		{
			uint32_t Instance;       // Into the render scene GPU copy's instances (world rows, material slot)
			uint32_t MeshSubmeshLOD; // Mesh slot (bits 0-19), submesh (20-27), level of detail (28-31)
			uint32_t Region;         // Into the frame's regions
			uint32_t Flags;          // c_VoxelItem*
		};
		static_assert(sizeof(VoxelWorkItem) == 16);
		constexpr uint32_t c_VoxelItemMaxMeshSlot = (1u << 20) - 1;
		constexpr uint32_t c_VoxelItemMaxSubmesh = 255;

		// c_GIGroupTriangles triangles of a work item, a group of the voxelizer's threads.
		struct VoxelBatch
		{
			uint32_t Item;
			uint32_t FirstTriangle;
		};
		static_assert(sizeof(VoxelBatch) == 8);

		// A material slot as the voxelizer reads it (GIMaterialBuffer): the CPU's means of its maps, so no compute pass reads
		// bindless textures.
		struct GIMaterial
		{
			uint32_t Albedo;   // Linear diffuse albedo as RGB8 (BaseColor.rgb * (1 - Metallic) times its base color map's mean)
			uint32_t Emission; // RGB9E5 of its emission (nits) times its emissive map's mean
			uint32_t Flags;    // c_CullMaterial* (GPUSceneData.h) of the material as drawn
			uint32_t Padding;
		};
		static_assert(sizeof(GIMaterial) == 16);

	}

}
