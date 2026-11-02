#ifndef STRATA_MOTION_SLANG
#define STRATA_MOTION_SLANG

#include "Include/Scene.slang"

// The motion (UV units, current minus previous position, as the velocity target holds it) of a point that did not move, seen
// at a pixel's UV and depth: the camera's motion alone. Where nothing was drawn (depth 0: the sky) the point is infinitely far,
// a direction, which the camera's translation does not move; orthographic views take the far plane there.
float2 GetCameraMotion(float2 uv, float depth)
{
	float4 point = mul(u_Frame.InverseViewProjection, float4(UVToNDC(uv), depth, 1.0));
	if (depth <= 0.0 && u_Frame.CameraForward.w < 0.5)
		point = float4(point.xyz / point.w, 0.0);
	return GetClipMotion(mul(u_Frame.UnjitteredViewProjection, point), mul(u_Frame.PreviousViewProjection, point));
}

#endif
