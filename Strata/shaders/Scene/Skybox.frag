#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"
#include "Include/Fog.glsl"

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
	color *= u_Frame.EnvironmentParams.x;
	if (u_Frame.FogParams.x > 0.0)
	{
		// The fog's rays start where the surfaces' do (Include/Fog.glsl): at the camera, or on its plane when orthographic.
		vec3 start = vec3(0.0);
		if (u_Frame.CameraForward.w > 0.5)
		{
			vec3 nearPosition = nearPoint.xyz / nearPoint.w;
			start = nearPosition - direction * dot(nearPosition, direction);
		}
		color = ApplyHeightFogToSky(color, start, direction);
	}
	o_Color = vec4(SanitizeHDR(color * u_Frame.TemporalParams.y), 1.0); // Pre-exposed, like the surfaces
}
