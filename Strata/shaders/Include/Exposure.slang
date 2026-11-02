#ifndef STRATA_EXPOSURE_SLANG
#define STRATA_EXPOSURE_SLANG

// Exposure state shared by the post-processing passes. Keep in sync with Renderer/PostProcessData.h.
//
// The exposure buffer holds floats written by Exposure.comp (or by the CPU with manual exposure): [0] the adapted log2 average
// luminance, [1] the linear exposure multiplier applied to the scene color, [2] the exposure key's stops (0 without a key).
#define ST_EXPOSURE_LOG_LUMINANCE 0
#define ST_EXPOSURE_MULTIPLIER    1
#define ST_EXPOSURE_KEY           2

// Luminance histogram: bin 0 counts pixels darker than 2^MIN_LOG2 (including black), which the metering ignores;
// bins 1..255 split [MIN_LOG2, MAX_LOG2] evenly in log2 space.
#define ST_HISTOGRAM_BINS     256u
#define ST_HISTOGRAM_MIN_LOG2 -12.0
#define ST_HISTOGRAM_MAX_LOG2 16.0

float Luminance(float3 color)
{
	return dot(color, float3(0.2126, 0.7152, 0.0722));
}

uint LuminanceToHistogramBin(float luminance)
{
	// The negated comparison also sends NaN to bin 0.
	if (!(luminance > exp2(ST_HISTOGRAM_MIN_LOG2)))
		return 0u;
	float t = clamp((log2(luminance) - ST_HISTOGRAM_MIN_LOG2) / (ST_HISTOGRAM_MAX_LOG2 - ST_HISTOGRAM_MIN_LOG2), 0.0, 1.0);
	return 1u + uint(t * float(ST_HISTOGRAM_BINS - 2u));
}

float HistogramBinToLog2(uint bin)
{
	if (bin == 0u)
		return ST_HISTOGRAM_MIN_LOG2;
	return ST_HISTOGRAM_MIN_LOG2 + (float(bin - 1u) + 0.5) / float(ST_HISTOGRAM_BINS - 2u) * (ST_HISTOGRAM_MAX_LOG2 - ST_HISTOGRAM_MIN_LOG2);
}

#endif
