#include "Include/Common.slang"

#include "Include/Scene.slang"
#include "Include/Atmosphere.slang"
#include "Include/Fog.slang"

// Environment background, drawn with a fullscreen triangle at the far plane where no geometry was rendered.

static float2 v_UV;
static float4 o_Color;

void st_Main()
{
	// Two points along the pixel's view ray; their difference is the ray direction for any projection.
	float2 ndc = UVToNDC(v_UV);
	float4 nearPoint = mul(u_Frame.InverseViewProjection, float4(ndc, 1.0, 1.0));
	float4 farPoint = mul(u_Frame.InverseViewProjection, float4(ndc, 0.5, 1.0));
	float3 direction = normalize(farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w);

#ifdef ST_ATMOSPHERE
	// The atmosphere (SkyLightSource::Atmosphere): its sky-view table, per lux of sun, and the sun's disk, which carries the
	// light's illuminance at the ground over its solid angle (a real sun's disk: about 1.3 billion nits, which the
	// pre-exposure keeps within half floats), its edge smoothed over a pixel.
	float3 color = u_SkyViewTable.SampleLevel(u_Samplers[0], DirectionToSkyViewUV(direction), 0.0).rgb * u_Frame.EnvironmentParams.x;
	float radius = u_Frame.SunDiskDirection.w;
	if (radius > 0.0)
	{
		float chord = length(direction - u_Frame.SunDiskDirection.xyz);
		float angle = 2.0 * asin(min(chord * 0.5, 1.0));
		float pixelAngle = max(length(fwidth(direction)), 1.0e-6);
		float coverage = clamp((radius - angle) / pixelAngle + 0.5, 0.0, 1.0);
		float solidAngle = TWO_PI * (1.0 - cos(radius));
		color += u_Frame.SunDiskColor.rgb * (coverage / max(solidAngle, 1.0e-12));
	}
#else
	float3 color = u_EnvironmentMap.SampleLevel(u_LinearClampSampler, RotateEnvironmentDirection(direction), u_Frame.SkyParams.x).rgb;
	color *= u_Frame.EnvironmentParams.x;
#endif
	if (u_Frame.FogParams.x > 0.0)
	{
		// The fog's rays start where the surfaces' do (Include/Fog.slang): at the camera, or on its plane when orthographic.
		float3 start = float3(0.0);
		if (u_Frame.CameraForward.w > 0.5)
		{
			float3 nearPosition = nearPoint.xyz / nearPoint.w;
			start = nearPosition - direction * dot(nearPosition, direction);
		}
		color = ApplyHeightFogToSky(color, start, direction);
	}
	o_Color = float4(SanitizeHDR(color * u_Frame.TemporalParams.y), 1.0); // Pre-exposed, like the surfaces
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float2 v_UV : TEXCOORD0;
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Color : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_UV = input.v_UV;
	st_Main();
	ST_Output output;
	output.o_Color = o_Color;
	return output;
}
