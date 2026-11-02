#ifndef STRATA_MOTION_BLUR_SLANG
#define STRATA_MOTION_BLUR_SLANG

#include "Include/Motion.slang"

// The motion blur's passes (PostProcess/MotionBlurTiles.comp, MotionBlurNeighbors.comp, MotionBlur.comp; SceneRendererPost.cpp,
// RenderMotionBlur): the frame constants at b0 (Include/Scene.glsl: the camera's motion, the depth's view distance), the
// parameters below as push constants. Every fetch is a point fetch (texelFetch), so no sampler is declared. SRV 0-4 alias
// Include/Scene.glsl's declarations, which these shaders never use, as Overlay/MotionView.frag's do.

// RenderData::MotionBlurParameters (Renderer/PostProcessData.h).
struct MotionBlurParameters
{
	float ShutterFraction; // MotionBlurShutterAngle / 360: the share of a frame's motion the shutter sees
	float MaxLength;       // Pixels: the longest blur (MotionBlurMaxLength times the viewport's height)
	float MinLength;       // Pixels: blurs shorter than this are dropped, fading in up to twice it (the height / 1080)
	uint TileSize;         // Pixels each way of a tile of the tile maxima
	uint SampleCount;      // The gather's samples (8, 12 or 16)
	uint SourceMask;       // 1: the source's alpha is the depth of field's mask, kept where the gather copies the color
	uint Padding0;
	uint Padding1;
};
[[vk::push_constant]] ConstantBuffer<MotionBlurParameters> u_MotionBlur;

// The blur of a pixel, in pixels (+y down): its motion since the previous frame (the camera's where nothing was drawn) times
// the shutter's share, clamped to MaxLength (its direction kept), dropped below MinLength and faded in up to twice it. A
// sample-and-hold display already smears eye-tracked motion by its whole length: a blur of a pixel or two only softens
// texture during slow pans.
float2 GetBlurVelocity(float2 velocityUV, float depth, float2 uv)
{
	float2 motion = depth > 0.0 ? velocityUV : GetCameraMotion(uv, 0.0);
	float2 velocity = motion * u_Frame.ViewportSize.xy * u_MotionBlur.ShutterFraction;
	float speed = length(velocity);
	if (!(speed > 0.0))
		return float2(0.0);
	float clamped = min(speed, u_MotionBlur.MaxLength);
	float kept = clamped * smoothstep(u_MotionBlur.MinLength, 2.0 * u_MotionBlur.MinLength, clamped);
	return velocity * (kept / speed);
}

// The distance from the camera of what a pixel shows (the view's depth along its forward axis), at most
// c_MotionBlurSkyDistance, the largest half float: the sky, where nothing was drawn, lies there (infinitely far).
static const float c_MotionBlurSkyDistance = 65504.0;

float GetViewDistance(float2 uv, float depth)
{
	if (depth <= 0.0)
		return c_MotionBlurSkyDistance;
	float4 point = mul(u_Frame.InverseProjection, float4(UVToNDC(uv), depth, 1.0));
	return clamp(-point.z / point.w, 1e-4, c_MotionBlurSkyDistance);
}

// What the gather reads of every sample, written by the tile pass for every pixel (an r32ui target): the pixel's blur radius
// (half its blur's length, at least half a pixel) and its view distance, as two half floats.
uint PackBlurPixel(float radius, float viewDistance)
{
	return PackHalf2x16(float2(radius, viewDistance));
}

float2 UnpackBlurPixel(uint packed)
{
	return UnpackHalf2x16(packed);
}

float GetBlurRadius(float2 velocity)
{
	return max(0.5 * length(velocity), 0.5);
}

// A neighborhood whose every blur lies within this many pixels of its largest (or 5% of its length) blurs as one surface
// moving uniformly: MotionBlur.comp's gather then reads only colors, with the weights its depth-aware path gives samples of
// one surface (a turning camera: every point moves alike on screen, whatever its depth).
float GetUniformTolerance(float neighborLength)
{
	return max(0.5, 0.05 * neighborLength);
}

#endif
