#ifndef STRATA_FILMIC_GLSL
#define STRATA_FILMIC_GLSL

#include "Include/FilmicConstants.glsl"

// The Filmic tone curve (TonemapOperator::Filmic; its CPU reference is Renderer/PostProcessMath.h's ApplyFilmic, its form and
// fit Tools/FitFilmic.py). Per channel, for an exposed value x and t = log2(x / 0.18), the stops from middle gray: below
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

// The curve per channel on the inset color, outset back: bright saturated colors go to white without skewing their hue.
// Display-linear, in [0, 1].
vec3 TonemapFilmic(vec3 color, float toeSlope, float whiteStops, float shoulderPower)
{
	vec3 inset = c_FilmicInset * max(color, vec3(0.0));
	vec3 curved = vec3(FilmicCurve(inset.r, toeSlope, whiteStops, shoulderPower), FilmicCurve(inset.g, toeSlope, whiteStops, shoulderPower),
		FilmicCurve(inset.b, toeSlope, whiteStops, shoulderPower));
	return clamp(c_FilmicOutset * curved, vec3(0.0), vec3(1.0));
}

#endif
