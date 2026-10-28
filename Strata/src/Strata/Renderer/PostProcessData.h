#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>

namespace Strata
{

	// GPU data of the post-processing passes (SceneRendererPost.cpp, shaders/PostProcess): their constant buffers, push
	// constants and buffer slots. Each struct matches its shader's declaration (std140 for constant buffers): change both
	// together.
	namespace RenderData
	{

		// PostProcess/Tonemap.frag's constant buffer (binding ST_CBV(0)), written once per frame. std140: it grows only by
		// appending vec4-aligned blocks, whose offsets the static_asserts below check.
		struct TonemapConstants
		{
			float BloomIntensity;       // 0: fraction of the light scattered into the glow; 0 = off
			float BloomScale;           //    normalizes the sum of the bloom levels
			float Saturation;
			float Contrast;             //    around middle gray; 1 = unchanged
			int32_t Operator;           // 16: TonemapOperator
			uint32_t BloomAdditive;     //     1: the bloom holds only light above a threshold and is added instead of mixed
			float InversePreExposure;   //     1 / the pre-exposure the scene color carries (FrameConstants::TemporalParams.y)
			float Sharpening;           //     of the displayed image, on frames the TAA resolved; 0 = off
			float DitherLevels;         // 32: 2^bits - 1 of the target this pass writes; 0: no dither
			float Vignette;
			glm::vec2 VignetteTangents; //     tangents of the half field of view per axis (a lens's vignette); (0, 0): none
			float FilmicToeSlope;       // 48: the Filmic curve's look (PostProcessMath::FilmicCurve)
			float FilmicWhiteStops;
			float FilmicShoulderPower;
			uint32_t ColorFlags;        //     c_TonemapColorGrading: the grading below applies (else skipped: bit-identical to none)
			glm::vec4 ColorRows[3];     // 64: PostProcessMath::ColorGrading::Matrix (white balance times slope) as rows (w unused)
			glm::vec4 ColorOffset;      // 112: xyz: the offset in units of light (ColorOffset times middle gray), added before the power
			glm::vec4 ColorPower;       // 128: xyz: the power around middle gray
		};
		static_assert(sizeof(TonemapConstants) == 144);
		static_assert(offsetof(TonemapConstants, ColorRows) == 64 && offsetof(TonemapConstants, ColorOffset) == 112);
		static_assert(offsetof(TonemapConstants, ColorPower) == 128);
		// TonemapConstants::ColorFlags: the grading applies; the source's alpha marks the pixels the motion blur or the depth of
		// field blurred (no sharpening there).
		constexpr uint32_t c_TonemapColorGrading = 1u;
		constexpr uint32_t c_TonemapBlurMask = 2u;

		// PostProcess/MotionBlurTiles.comp, MotionBlurNeighbors.comp, MotionBlur.comp (push constants, slot 1: b0 is the frame's
		// constants).
		struct MotionBlurParameters
		{
			float ShutterFraction; // PostProcessComponent::MotionBlurShutterAngle / 360
			float MaxLength;       // Pixels: MotionBlurMaxLength times the viewport's height
			float MinLength;       // Pixels: the viewport's height / 1080
			uint32_t TileSize;     // Pixels: max(8, ceil(MaxLength / 2))
			uint32_t SampleCount;  // 8, 12 or 16 (RendererQualitySettings::MotionBlurQuality)
			uint32_t SourceMask;   // 1: the source's alpha is the depth of field's mask, kept where the motion blur copies the color
			uint32_t Padding0;
			uint32_t Padding1;
		};
		static_assert(sizeof(MotionBlurParameters) == 32);
		// The smallest tile: the tile targets are made for it, so a change of MotionBlurMaxLength never reallocates.
		constexpr uint32_t c_MotionBlurMinTileSize = 8;

		// PostProcess/DepthOfField*.comp (push constants, slot 1: b0 is the frame's constants).
		struct DepthOfFieldParameters
		{
			float FocusDistance;  // Meters
			float CoCScale;       // Full-resolution pixels: the circle of confusion's radius for a point infinitely far
			float MaxCoC;         // Full-resolution pixels: the largest radius either way (c_DepthOfFieldMaxDiameter of the height, halved)
			uint32_t TileSize;    // Half-resolution pixels: max(c_DepthOfFieldMinTileSize, ceil(MaxCoC / 2))
			uint32_t SampleCount; // The gather's samples per field (c_DepthOfFieldSamples)
			uint32_t Padding0;
			uint32_t Padding1;
			uint32_t Padding2;
		};
		static_assert(sizeof(DepthOfFieldParameters) == 32);
		// The largest circle of confusion, as a fraction of the image height (its diameter), the gather's samples, and the
		// smallest tile of the near field's maxima (half-resolution pixels; the tile targets are made for it).
		constexpr float c_DepthOfFieldMaxDiameter = 0.02f;
		constexpr uint32_t c_DepthOfFieldSamples = 48;
		constexpr uint32_t c_DepthOfFieldMinTileSize = 8;
		static_assert(offsetof(TonemapConstants, Operator) == 16 && offsetof(TonemapConstants, DitherLevels) == 32);
		static_assert(offsetof(TonemapConstants, VignetteTangents) == 40 && offsetof(TonemapConstants, FilmicToeSlope) == 48);

		// PostProcess/FXAA.frag (push constants).
		struct FXAAParameters
		{
			float DitherLevels; // 2^bits - 1 of the target FXAA writes; 0: no dither
			float Padding0;
			float Padding1;
			float Padding2;
		};
		static_assert(sizeof(FXAAParameters) == 16);

		// Include/Exposure.glsl: the exposure buffer's floats (the adapted log2 luminance, the multiplier of luminance the tone
		// mapping applies, the exposure key's stops) and the luminance histogram.
		constexpr uint32_t c_ExposureLogLuminance = 0;
		constexpr uint32_t c_ExposureMultiplier = 1;
		constexpr uint32_t c_ExposureKeyStops = 2;
		constexpr uint32_t c_ExposureValues = 4;
		constexpr uint32_t c_HistogramBins = 256;
		constexpr float c_HistogramMinLog2 = -12.0f;
		constexpr float c_HistogramMaxLog2 = 16.0f;

		// PostProcess/Histogram.comp and Exposure.comp (push constants).
		struct ExposureParameters
		{
			float MinLog2;            // 0: the range the metered average is clamped to
			float MaxLog2;
			float AdaptLight;         //    the fraction of the way to a brighter target this frame (1: jump to it)
			float AdaptDark;          //    and to a darker one
			float Compensation;       // 16: 2^Exposure
			float LowPercentile;      //     the metering window
			float HighPercentile;
			float InversePreExposure; //     1 / the pre-exposure the scene color carries: the histogram meters luminance
			float KeyStrength;        // 32: the exposure key (PostProcessComponent::AutoExposureKey); 0 without a directional light
			float KeyReferenceLog2;   //     log2 of the luminance of a gray card (albedo 0.18) facing the directional light
			float KeyMinStops;        //     the key's range
			float KeyMaxStops;
			float MeteringAspect;     // 48: width / height of the metered image
			uint32_t Metering;        //     ExposureMetering
			float Padding0;
			float Padding1;
		};
		static_assert(sizeof(ExposureParameters) == 64);
		static_assert(offsetof(ExposureParameters, Compensation) == 16 && offsetof(ExposureParameters, KeyStrength) == 32);
		static_assert(offsetof(ExposureParameters, MeteringAspect) == 48);
		// The exposure key's range in stops: a scene metering below the sun's gray card shows at most this much darker, one
		// above it at most this much brighter.
		constexpr float c_ExposureKeyMinStops = -2.0f;
		constexpr float c_ExposureKeyMaxStops = 1.0f;

		// PostProcess/Bloom.comp's modes, and the most levels its chain allocates (every level down to 4 pixels: a frame uses
		// as many as PostProcessComponent::BloomRadius asks for, so a change of the radius never reallocates).
		constexpr uint32_t c_BloomPrefilter = 0;
		constexpr uint32_t c_BloomDownsample = 1;
		constexpr uint32_t c_BloomUpsample = 2;
		constexpr uint32_t c_MaxBloomLevels = 8;

		// PostProcess/Bloom.comp (push constants).
		struct BloomParameters
		{
			float Threshold;
			float Knee;
			uint32_t Mode;
			float InversePreExposure; // 1 / the pre-exposure the scene color carries
			float UpsampleWeight;     // Upsamples: the weight of the smaller level added (the deepest level's share; else 1)
			float Padding0;
			float Padding1;
			float Padding2;
		};
		static_assert(sizeof(BloomParameters) == 32);

	}

}
