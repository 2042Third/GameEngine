#include "Include/Common.slang"

#include "Include/DepthOfField.slang"

// Depth of field's composite, at full resolution, into the post source of what follows: a pixel whose circle of confusion is
// under a pixel across keeps the sharp image exactly; its own blurred field fades in over the next pixel of diameter; the near
// field (premultiplied) goes over everything. Alpha 1 marks the pixels it changed (the tone mapping does not sharpen them).
//
// The depth is this frame's, which temporal anti-aliasing jitters by up to half a pixel, while the color is the resolved
// image without jitter. A pixel beside an in-focus silhouette would see the subject in some frames' depth and the background
// in others, and switch between the resolved edge and the blurred background every frame. So the blur fades in with the
// distance from in-focus depth: none within a pixel of it (an edge pixel always finds the subject in its neighborhood,
// whatever the jitter), half two pixels away, all of it from three. What still changes with the jitter, a pixel two or three
// away from the subject, switches between two images of the background only, half as far.

Texture2D<float4> u_Color : register(t0);
Texture2D<float4> u_Depth : register(t1);
Texture2D<float4> u_Near : register(t2);
Texture2D<float4> u_Own : register(t3);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Composite : register(u0);

// How far, in pixels, the blur's fade-in looks for in-focus depth; the group's tile with that margin each way.
static const int c_FocusReach = 2;
static const int c_FocusTile = 8 + 2 * c_FocusReach;

// Whether each pixel of the tile shows something in focus (its circle under the smoothstep's start below).
groupshared uint s_InFocus[c_FocusTile * c_FocusTile];

float GetRadius(int2 pixel, int2 size)
{
	pixel = clamp(pixel, int2(0), size - 1);
	float2 uv = (float2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	return abs(GetCircleOfConfusion(uv, u_Depth.Load(int3(pixel, 0)).r));
}

void st_Main()
{
	int2 size = TextureSize(u_Color, 0);
	int2 origin = int2(st_WorkGroupID.xy) * 8 - c_FocusReach;
	for (uint index = st_LocalInvocationIndex; index < uint(c_FocusTile * c_FocusTile); index += 64u)
	{
		int2 offset = int2(int(index) % c_FocusTile, int(index) / c_FocusTile);
		s_InFocus[index] = GetRadius(origin + offset, size) < 0.5 ? 1u : 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	int2 pixel = int2(st_GlobalInvocationID.xy);
	if (any((pixel >= size)))
		return;
	float3 color = u_Color.Load(int3(pixel, 0)).rgb;
	float2 uv = (float2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	float radius = abs(GetCircleOfConfusion(uv, u_Depth.Load(int3(pixel, 0)).r));
	float changed = 0.0;
	float blend = smoothstep(0.5, 1.0, radius);
	if (blend > 0.0)
	{
		// The distance (the larger of x and y) to the nearest in-focus pixel within reach; beyond it, c_FocusReach + 1.
		int focusDistance = c_FocusReach + 1;
		int2 local = int2(st_LocalInvocationID.xy) + c_FocusReach;
		for (int y = -c_FocusReach; y <= c_FocusReach; y++)
		{
			for (int x = -c_FocusReach; x <= c_FocusReach; x++)
			{
				if (s_InFocus[(local.y + y) * c_FocusTile + local.x + x] != 0u)
					focusDistance = min(focusDistance, max(abs(x), abs(y)));
			}
		}
		blend *= clamp(float(focusDistance - 1) / float(c_FocusReach), 0.0, 1.0);
	}
	if (blend > 0.0)
	{
		color = lerp(color, u_Own.SampleLevel(u_LinearClampSampler, uv, 0.0).rgb, blend);
		changed = 1.0;
	}
	float4 near = u_Near.SampleLevel(u_LinearClampSampler, uv, 0.0);
	if (near.a > 0.0)
	{
		color = color * (1.0 - near.a) + near.rgb;
		changed = 1.0;
	}
	o_Composite[pixel] = float4(min(color, float3(c_MaxHDRValue)), changed);
}

// --- Entry point ---
[numthreads(8, 8, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
