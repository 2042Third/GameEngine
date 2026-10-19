#ifndef STRATA_SDF_TEXT_GLSL
#define STRATA_SDF_TEXT_GLSL

// Signed distance field glyphs (FontAtlas): the outline is where the distance crosses the on-edge value, and the value
// rises by 128 / 255 over FontFace::c_DistancePadding (4) texels toward the inside.
const float c_SDFOnEdgeValue = 128.0 / 255.0;
const float c_SDFValuePerTexel = 32.0 / 255.0;

// Coverage of a distance sample around an edge value: ramps over about one screen pixel, so text stays sharp and smooth at
// any size. Call in uniform control flow (it takes the distance's screen derivatives).
float SDFCoverage(float distanceValue, float edgeValue)
{
	float width = max(fwidth(distanceValue) * 0.7, 1e-4);
	return smoothstep(edgeValue - width, edgeValue + width, distanceValue);
}

#endif
