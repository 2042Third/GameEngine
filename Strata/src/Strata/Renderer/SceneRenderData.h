#pragma once

#include <glm/glm.hpp>

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

		constexpr uint32_t c_LightPoint = 0u;
		constexpr uint32_t c_LightSpot = 1u;

		constexpr uint32_t c_SamplerCount = 6; // TextureFilter (2) x TextureWrap (3)

		constexpr uint32_t c_MaxCascades = 4;

		struct FrameConstants
		{
			glm::mat4 View;
			glm::mat4 Projection;
			glm::mat4 ViewProjection;
			glm::mat4 InverseView;
			glm::mat4 InverseProjection;
			glm::mat4 InverseViewProjection;
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
			glm::mat4 CascadeViewProjection[c_MaxCascades];
			glm::vec4 CascadeSplits;             // View distance where each cascade ends
			glm::vec4 CascadeData[c_MaxCascades]; // x: world units per shadow texel, y: depth range in world units, z: penumbra search distance
			glm::vec4 ShadowParams;              // x: 2 tan(light angular radius), y: 1 / map size, w: cascade count
			glm::vec4 ShadowBias;                // x: depth bias (texels), y: normal offset (texels)
			glm::vec4 AOParams;                  // x: 1 when screen-space ambient occlusion is available
		};
		static_assert(sizeof(FrameConstants) == 10 * 64 + 20 * 16);

		struct InstanceData
		{
			glm::mat4 World;
			glm::mat4 NormalMatrix; // Inverse transpose of World (not computed for shadow casters, which have no normals)
			uint32_t MaterialIndex;
			uint32_t EntityID; // Entity identifier + 1 (0 = none)
			uint32_t Flags;    // c_Instance*
			uint32_t Padding;
		};
		static_assert(sizeof(InstanceData) == 144);

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
			uint32_t BaseColorMap; // Bits 0-23: bindless slot, bits 24-31: sampler index
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
			glm::vec4 PositionRange; // xyz: world position, w: range
			glm::vec4 Color;         // rgb: color * intensity, w: 1 / range^2
			glm::vec4 DirectionType; // xyz: direction the light travels, w: type
			glm::vec4 SpotAngles;    // x: cos(inner), y: cos(outer)
		};
		static_assert(sizeof(LightData) == 64);

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
			float Padding;
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
			float Padding0;
			float Padding1;
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

		constexpr uint32_t c_BloomPrefilter = 0;
		constexpr uint32_t c_BloomDownsample = 1;
		constexpr uint32_t c_BloomUpsample = 2;
		constexpr uint32_t c_MaxBloomLevels = 6;

		struct BloomParameters
		{
			float Threshold;
			float Knee;
			uint32_t Mode;
			float Padding;
		};
		static_assert(sizeof(BloomParameters) == 16);

	}

}
