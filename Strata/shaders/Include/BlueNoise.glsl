#ifndef STRATA_BLUE_NOISE_GLSL
#define STRATA_BLUE_NOISE_GLSL

#include "Include/Common.glsl"

// A 64x64 tile of blue noise (Renderer::GetBlueNoiseTexture) at scene slot SRV 33: per-pixel values whose neighbors differ,
// fixed per pixel (nothing here turns with the frame). Every shader that includes this file enables
// GL_EXT_samplerless_texture_functions.
layout(set = 0, binding = ST_SRV(33)) uniform texture2D u_BlueNoise;

// The tile's value at a pixel, shifted by `shift` (decorrelated uses take different shifts), in (0, 1).
float BlueNoise(ivec2 pixel, ivec2 shift)
{
	return texelFetch(u_BlueNoise, (pixel + shift) & 63, 0).r + 0.5 / 256.0;
}

#endif
