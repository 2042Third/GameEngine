#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Motion.glsl"

// The motion debug view over the finished image (SceneRenderOptions::ShowMotionVectors): each pixel's motion since the
// renderer's previous frame as a color whose hue is the direction on screen (right red, up yellow-green, left cyan, down
// violet) and whose strength grows with the speed up to c_FullSpeed pixels a frame, over the image darkened where nothing
// moves. Where nothing was drawn, the motion is the camera's (GetCameraMotion).

layout(set = 0, binding = ST_SRV(0)) uniform texture2D u_Velocity;
layout(set = 0, binding = ST_SRV(1)) uniform texture2D u_Depth;

layout(location = 0) out vec4 o_Color;

const float c_FullSpeed = 16.0;

vec3 HueToRGB(float hue)
{
	return clamp(abs(fract(hue + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	float depth = texelFetch(u_Depth, pixel, 0).r;
	vec2 motion = depth > 0.0 ? texelFetch(u_Velocity, pixel, 0).rg : GetCameraMotion((vec2(pixel) + 0.5) * u_Frame.ViewportSize.zw, 0.0);
	vec2 pixels = motion * u_Frame.ViewportSize.xy;
	float speed = length(pixels);
	// The square root shows sub-pixel motion too.
	float strength = sqrt(clamp(speed / c_FullSpeed, 0.0, 1.0));
	// Rows grow downward: screen up is -y.
	float hue = speed > 0.0 ? fract(atan(-pixels.y, pixels.x) / TWO_PI) : 0.0;
	o_Color = vec4(HueToRGB(hue) * strength, mix(0.65, 1.0, strength));
}
