#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"

// Environment background, drawn with a fullscreen triangle at the far plane where no geometry was rendered.

layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 o_Color;

void main()
{
	// Two points along the pixel's view ray; their difference is the ray direction for any projection.
	vec2 ndc = UVToNDC(v_UV);
	vec4 nearPoint = u_Frame.InverseViewProjection * vec4(ndc, 1.0, 1.0);
	vec4 farPoint = u_Frame.InverseViewProjection * vec4(ndc, 0.5, 1.0);
	vec3 direction = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);

	vec3 color = textureLod(samplerCube(u_EnvironmentMap, u_LinearClampSampler), RotateEnvironmentDirection(direction), u_Frame.SkyParams.x).rgb;
	o_Color = vec4(SanitizeHDR(color * u_Frame.EnvironmentParams.x), 1.0);
}
