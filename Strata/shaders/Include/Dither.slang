#ifndef STRATA_DITHER_SLANG
#define STRATA_DITHER_SLANG

// The output's dither (Renderer/PostProcessMath.h holds its CPU twin, in the same integer arithmetic: the tests compare
// them). The noise is a static hash of the pixel (no frame index): one code of it is invisible as a pattern, nothing needs
// temporal accumulation, and still frames and captures stay deterministic.

// A 32-bit integer finalizer of the xorshift-multiply kind: three xorshifts and two multiplications by odd constants chosen
// when this was written and fixed since (Renderer.PostProcessMath's statistics accept them).
uint MixBits(uint x)
{
	x ^= x >> 16u;
	x *= 0xA3B195E5u;
	x ^= x >> 15u;
	x *= 0x6C8E9CF7u;
	x ^= x >> 16u;
	return x;
}

// A uniform value in [0, 1) for a pixel and a stream (independent streams per use): the top 24 bits of the hash.
float PixelRandom(uint2 pixel, uint stream)
{
	uint bits = MixBits(pixel.x ^ MixBits(pixel.y ^ MixBits(stream)));
	return float(bits >> 8u) * (1.0 / 16777216.0);
}

// Values within this many codes of black or white are black or white: the sRGB encode leaves 1.0 a few ulps below 1 in float.
static const float c_DitherEndSnap = 1.0 / 256.0;

// The dithered code values of a display-encoded color for a target of `levels` + 1 codes per channel (255, 1023), as
// normalized values; `levels` 0 returns the color unchanged. A value v in codes (clamped to [0, levels]) becomes q / levels:
// q = floor(v + n + 0.5) with triangular noise n = u0 + u1 - 1 of one code where that cannot leave [0, levels] (its noise
// power does not depend on v), and q = floor(v + u0) within half a code of black or white, so the mean of q is v everywhere
// and black and white stay exact. Streams 0-5 (two per channel): the channels are independent. q / levels lies within half
// an ulp of code q, which UNORM targets store as q.
float3 DitherToCodes(float3 encoded, uint2 pixel, float levels)
{
	if (levels <= 0.0)
		return encoded;
	float3 result;
	for (int channel = 0; channel < 3; channel++)
	{
		float v = clamp(encoded[channel] * levels, 0.0, levels);
		v = v < c_DitherEndSnap ? 0.0 : (v > levels - c_DitherEndSnap ? levels : v);
		float u0 = PixelRandom(pixel, uint(2 * channel));
		float u1 = PixelRandom(pixel, uint(2 * channel + 1));
		float q = (v >= 0.5 && v <= levels - 0.5) ? floor(v + (u0 + u1 - 1.0) + 0.5) : floor(v + u0);
		result[channel] = q / levels;
	}
	return result;
}

#endif
