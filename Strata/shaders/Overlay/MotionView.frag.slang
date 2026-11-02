#include "Include/Common.slang"

#include "Include/Motion.slang"

// The motion debug view over the finished image (SceneRenderOptions::ShowMotionVectors): each pixel's motion since the
// renderer's previous frame as a color whose hue is the direction on screen (right red, up yellow-green, left cyan, down
// violet) and whose strength grows with the speed up to c_FullSpeed pixels a frame, over the image darkened where nothing
// moves. Where nothing was drawn, the motion is the camera's (GetCameraMotion).

Texture2D<float4> u_Velocity : register(t0);
Texture2D<float4> u_Depth : register(t1);

static float4 o_Color;

static const float c_FullSpeed = 16.0;

float3 HueToRGB(float hue)
{
	return clamp(abs(frac(hue + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

void st_Main()
{
	int2 pixel = int2(st_FragCoord.xy);
	float depth = u_Depth.Load(int3(pixel, 0)).r;
	float2 motion = depth > 0.0 ? u_Velocity.Load(int3(pixel, 0)).rg : GetCameraMotion((float2(pixel) + 0.5) * u_Frame.ViewportSize.zw, 0.0);
	float2 pixels = motion * u_Frame.ViewportSize.xy;
	float speed = length(pixels);
	// The square root shows sub-pixel motion too.
	float strength = sqrt(clamp(speed / c_FullSpeed, 0.0, 1.0));
	// Rows grow downward: screen up is -y.
	float hue = speed > 0.0 ? frac(atan2(-pixels.y, pixels.x) / TWO_PI) : 0.0;
	o_Color = float4(HueToRGB(hue) * strength, lerp(0.65, 1.0, strength));
}

// --- Entry point ---
struct ST_Input
{
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Color : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	st_Main();
	ST_Output output;
	output.o_Color = o_Color;
	return output;
}
