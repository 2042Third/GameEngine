#ifndef STRATA_BLUE_NOISE_SLANG
#define STRATA_BLUE_NOISE_SLANG

#include "Include/Common.slang"

// A 64x64 tile of blue noise (Renderer::GetBlueNoiseTexture) at scene slot SRV 33: per-pixel values whose neighbors differ,
// fixed per pixel (nothing here turns with the frame), read with Load (no sampler).
Texture2D<float4> u_BlueNoise : register(t33);

// The tile's value at a pixel, shifted by `shift` (decorrelated uses take different shifts): (k + 0.5) / 256 for the byte k
// there, so the 256 values lie evenly in (0, 1) with mean 1/2. The R8_UNORM tile reads as k / 255.
float BlueNoise(int2 pixel, int2 shift)
{
	return u_BlueNoise.Load(int3((pixel + shift) & 63, 0)).r * (255.0 / 256.0) + 0.5 / 256.0;
}

#endif
