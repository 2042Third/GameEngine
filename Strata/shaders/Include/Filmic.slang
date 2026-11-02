#ifndef STRATA_FILMIC_SLANG
#define STRATA_FILMIC_SLANG

#include "Include/FilmicConstants.slang"

// The Filmic tone curve (TonemapOperator::Filmic; its CPU reference is Renderer/PostProcessMath.h's ApplyFilmic, its form and
// fit Tools/FitFilmic.py). For an exposed value x and t = log2(x / 0.18), the stops from middle gray: below
// middle gray y = 0.18 * 2^(Ct t + (C0 - Ct) tau (exp(t / tau) - 1)), whose log-log slope turns from C0 at middle gray to the
// toe's Ct below it; above it y = 0.18 + 0.82 (1 - (1 - t / Tw)^q) up to white, reached with zero slope Tw stops above
// middle gray. Middle gray shows as middle gray.
float FilmicCurve(float x, float toeSlope, float whiteStops, float shoulderPower)
{
	if (x <= 0.0)
		return 0.0;
	float t = log2(x / c_FilmicMiddleGray);
	if (t < 0.0)
		return c_FilmicMiddleGray * exp2(toeSlope * t + (c_FilmicContrast - toeSlope) * c_FilmicToeWidth * (exp(t / c_FilmicToeWidth) - 1.0));
	if (t >= whiteStops)
		return 1.0;
	return c_FilmicMiddleGray + (1.0 - c_FilmicMiddleGray) * (1.0 - pow(1.0 - t / whiteStops, shoulderPower));
}

// The curve on the inset color, outset back. In the highlights it works per channel, so bright saturated colors go to white
// without skewing their hue; below middle gray it scales the inset color as a whole (the curve of its largest channel over
// that channel), so the toe's slope, above 1, does not raise the channels' ratios to its power: shadows and dark skies keep
// the scene's chroma instead of deepening into primaries. The two blend over c_FilmicChannelStart to c_FilmicChannelEnd
// stops of the largest channel, which is on the curve either way. Display-linear, in [0, 1].
float3 TonemapFilmic(float3 color, float toeSlope, float whiteStops, float shoulderPower)
{
	float3 inset = mul(c_FilmicInset, max(color, float3(0.0)));
	float largest = max(inset.r, max(inset.g, inset.b));
	if (!(largest > 0.0))
		return float3(0.0);
	float largestCurved = FilmicCurve(largest, toeSlope, whiteStops, shoulderPower);
	float3 perChannel = float3(FilmicCurve(inset.r, toeSlope, whiteStops, shoulderPower), FilmicCurve(inset.g, toeSlope, whiteStops, shoulderPower),
		FilmicCurve(inset.b, toeSlope, whiteStops, shoulderPower));
	float channel = smoothstep(c_FilmicChannelStart, c_FilmicChannelEnd, log2(largest / c_FilmicMiddleGray));
	float3 curved = lerp(inset * (largestCurved / largest), perChannel, channel);
	return clamp(mul(c_FilmicOutset, curved), float3(0.0), float3(1.0));
}

#endif
