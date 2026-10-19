#ifndef STRATA_TEXTURE_STREAMING_GLSL
#define STRATA_TEXTURE_STREAMING_GLSL

// Texture streaming's measures of how fine a surface shows (TextureStreamingPolicy, Strata/Renderer/TextureStreamer.cpp),
// for visibility feedback. Keep them in sync.

#define ST_FLT_MAX 3.402823466e+38

// TextureStreamingPolicy::GetWorldPixels: the pixels one world unit spans at the point of the bounds nearest to the camera
// (at least `nearDistance` away); `unitPixels` is the pixels a unit spans at distance 1 (perspective) or anywhere
// (orthographic).
float GetWorldPixels(vec3 cameraPosition, bool orthographic, float unitPixels, float nearDistance, vec3 boundsMin, vec3 boundsMax)
{
	if (orthographic)
		return unitPixels;
	vec3 center = (boundsMin + boundsMax) * 0.5;
	vec3 extents = (boundsMax - boundsMin) * 0.5;
	vec3 outside = max(abs(cameraPosition - center) - extents, vec3(0.0));
	return unitPixels / max(length(outside), nearDistance);
}

// TextureStreamingPolicy::GetUVPixels: the pixels a texture coordinate unit spans, from the surface's world pixels, the
// submesh's texture coordinate density per axis and the instance's scale along each axis (the sparsest surfaces decide).
float GetUVPixels(float worldPixels, vec3 uvDensity, vec3 axisScale)
{
	vec3 spanned = vec3(axisScale.y * axisScale.z, axisScale.x * axisScale.z, axisScale.x * axisScale.y);
	float density = ST_FLT_MAX;
	bool found = false;
	for (int axis = 0; axis < 3; axis++)
	{
		if (uvDensity[axis] > 0.0)
		{
			density = min(density, uvDensity[axis] / sqrt(max(abs(spanned[axis]), 1e-12)));
			found = true;
		}
	}
	if (!found || !(density > 0.0) || isinf(density) || isnan(density))
		return ST_FLT_MAX;
	return worldPixels / density;
}

#endif
