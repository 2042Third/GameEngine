#ifndef STRATA_MOTION_GLSL
#define STRATA_MOTION_GLSL

#include "Include/Scene.glsl"

// The motion (UV units, current minus previous position, as the velocity target holds it) of a point that did not move, seen
// at a pixel's UV and depth: the camera's motion alone. Where nothing was drawn (depth 0: the sky) the point is infinitely far,
// a direction, which the camera's translation does not move; orthographic views take the far plane there.
vec2 GetCameraMotion(vec2 uv, float depth)
{
	vec4 point = u_Frame.InverseViewProjection * vec4(UVToNDC(uv), depth, 1.0);
	if (depth <= 0.0 && u_Frame.CameraForward.w < 0.5)
		point = vec4(point.xyz / point.w, 0.0);
	return GetClipMotion(u_Frame.UnjitteredViewProjection * point, u_Frame.PreviousViewProjection * point);
}

#endif
