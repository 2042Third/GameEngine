#ifndef STRATA_GRAIN_SLANG
#define STRATA_GRAIN_SLANG

#include "Include/Dither.slang"

// Film grain (PostProcessComponent::FilmGrain), added by the pass that writes the display image, before its dither: the tone
// mapping, or FXAA's or the morphological blend when one follows it. Added earlier, the anti-aliasing would take the grain's
// differences between neighbors for edges and blend them away, so the same strength would show softer under FXAA and the
// morphological anti-aliasing than under TAA or none.

// The grain's strength in the encoded domain at FilmGrain 1 (RenderData::c_FilmGrainAmplitude).
static const float c_FilmGrainAmplitude = 0.04;

// The display-encoded color with grain of `strength` (0: unchanged): most visible in the mid-tones, as film's (sqrt(4 Y (1 - Y))
// of the encoded luminance Y), the same on every channel, near-Gaussian (four uniform streams from 8, past the dither's),
// new every frame (salted with `frame`) and zero-mean, so it adds texture without moving the image's brightness.
float3 AddFilmGrain(float3 encoded, uint2 pixel, float strength, uint frame)
{
	if (!(strength > 0.0))
		return encoded;
	uint salt = 8u + 4u * frame;
	float noise = (PixelRandom(pixel, salt) + PixelRandom(pixel, salt + 1u) + PixelRandom(pixel, salt + 2u) + PixelRandom(pixel, salt + 3u) - 2.0) * 1.7320508;
	float encodedLuminance = clamp(dot(encoded, float3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
	return clamp(encoded + strength * c_FilmGrainAmplitude * sqrt(4.0 * encodedLuminance * (1.0 - encodedLuminance)) * noise, 0.0, 1.0);
}

#endif
