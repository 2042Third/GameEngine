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
			uint32_t Padding0;
		};
		static_assert(sizeof(TonemapConstants) == 64);
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

		// Include/Exposure.glsl: the exposure buffer and the luminance histogram.
		constexpr uint32_t c_ExposureLogLuminance = 0;
		constexpr uint32_t c_ExposureMultiplier = 1;
		constexpr uint32_t c_HistogramBins = 256;
		constexpr float c_HistogramMinLog2 = -12.0f;
		constexpr float c_HistogramMaxLog2 = 16.0f;

		// PostProcess/Histogram.comp and Exposure.comp (push constants).
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

		// PostProcess/Bloom.comp's modes, and the deepest level of its chain.
		constexpr uint32_t c_BloomPrefilter = 0;
		constexpr uint32_t c_BloomDownsample = 1;
		constexpr uint32_t c_BloomUpsample = 2;
		constexpr uint32_t c_MaxBloomLevels = 6;

		// PostProcess/Bloom.comp (push constants).
		struct BloomParameters
		{
			float Threshold;
			float Knee;
			uint32_t Mode;
			float InversePreExposure; // 1 / the pre-exposure the scene color carries
		};
		static_assert(sizeof(BloomParameters) == 16);

	}

}
