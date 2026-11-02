#include "Include/Common.slang"

#include "Include/Scene.slang"
#include "Include/OrderedFloat.slang"

// The receivers the frame's prepass depth holds, for the sun's cascades fitted to what the camera sees (ShadowFitTracker,
// Renderer/ShadowFit.h; RenderData::ShadowFitResult): the nearest and farthest view distance of any of them, how many pixels
// hold one, and per quarter stop of view distance (RenderData::c_ShadowFitBins from c_ShadowFitBinBase) the box of their
// positions in the camera's view space, which the CPU re-projects into later cameras and lights, and how many pixels hold them.
// Every box field reduces with atomicMax (the minima stored complemented) into a buffer cleared to 0, the counts with
// atomicAdd. Each thread reduces a
// 4 x 4 block of pixels in registers (neighbors mostly share a bin) before the group's shared copy, which adds its non-zero
// entries to the buffer once each. Both paths run it on the complete prepass depth, in GPU.ScreenShadows.

Texture2D<float4> u_Depth : register(t22);

// RenderData::ShadowFitResult as words: Nearest, Farthest, SurfacePixels, Reserved, then per bin MinX, MinY, MinD, MaxX, MaxY,
// MaxD and Pixels.
static const uint c_Bins = 80u;
static const uint c_FieldsPerBin = 7u;
static const uint c_BinPixels = 6u;
static const uint c_FirstBin = 4u;
static const uint c_Words = c_FirstBin + c_Bins * c_FieldsPerBin;
static const uint c_SurfacePixels = 2u;
static const float c_BinBase = 0.0625;
static const int c_BlockPixels = 4;

RWStructuredBuffer<uint> o_Fit : register(u0);

struct ShadowFitParameters
{
	uint2 Size; // The depth's pixels
	uint2 Reserved;
};
[[vk::push_constant]] ConstantBuffer<ShadowFitParameters> u_Parameters;

groupshared uint s_Fit[c_Words];

// Adds a bin's box (x, y and view distance d) and pixels to the group's copy.
void AddBin(int bin, float3 boxMin, float3 boxMax, uint pixels)
{
	if (bin < 0)
		return;
	uint first = c_FirstBin + uint(bin) * c_FieldsPerBin;
	InterlockedMax(s_Fit[first + 0u], ~EncodeOrderedFloat(boxMin.x));
	InterlockedMax(s_Fit[first + 1u], ~EncodeOrderedFloat(boxMin.y));
	InterlockedMax(s_Fit[first + 2u], ~EncodeOrderedFloat(boxMin.z));
	InterlockedMax(s_Fit[first + 3u], EncodeOrderedFloat(boxMax.x));
	InterlockedMax(s_Fit[first + 4u], EncodeOrderedFloat(boxMax.y));
	InterlockedMax(s_Fit[first + 5u], EncodeOrderedFloat(boxMax.z));
	InterlockedAdd(s_Fit[first + c_BinPixels], pixels);
}

void st_Main()
{
	uint thread = st_LocalInvocationIndex;
	for (uint word = thread; word < c_Words; word += 256u)
		s_Fit[word] = 0u;
	GroupMemoryBarrierWithGroupSync();

	int2 size = int2(u_Parameters.Size);
	float2 inverseSize = 1.0 / float2(size);
	int2 firstPixel = int2(st_GlobalInvocationID.xy) * c_BlockPixels;
	int bin = -1;
	float3 boxMin = float3(0.0);
	float3 boxMax = float3(0.0);
	uint binPixels = 0u;
	uint pixels = 0u;
	float nearest = 0.0;
	float farthest = 0.0;
	for (int y = 0; y < c_BlockPixels; y++)
	{
		for (int x = 0; x < c_BlockPixels; x++)
		{
			int2 pixel = firstPixel + int2(x, y);
			if (any((pixel >= size)))
				continue;
			float depth = u_Depth.Load(int3(pixel, 0)).r;
			if (!(depth > 0.0))
				continue; // Nothing drawn (reversed-Z: the far plane is 0)
			// View space, as the camera's own (rotation-only View) sees it.
			float4 point = mul(u_Frame.InverseProjection, float4(UVToNDC((float2(pixel) + 0.5) * inverseSize), depth, 1.0));
			float3 position = point.xyz / point.w;
			float distance = -position.z;
			if (!(distance > 0.0) || isinf(distance) || any(isinf(position.xy)) || any(isnan(position.xy)))
				continue;
			int pixelBin = clamp(int(floor(4.0 * log2(distance / c_BinBase))), 0, int(c_Bins) - 1);
			float3 sample3 = float3(position.xy, distance);
			if (pixelBin != bin)
			{
				AddBin(bin, boxMin, boxMax, binPixels);
				bin = pixelBin;
				boxMin = sample3;
				boxMax = sample3;
				binPixels = 0u;
			}
			else
			{
				boxMin = min(boxMin, sample3);
				boxMax = max(boxMax, sample3);
			}
			binPixels++;
			nearest = pixels == 0u ? distance : min(nearest, distance);
			farthest = max(farthest, distance);
			pixels++;
		}
	}
	AddBin(bin, boxMin, boxMax, binPixels);
	if (pixels > 0u)
	{
		InterlockedMax(s_Fit[0], ~EncodeOrderedFloat(nearest));
		InterlockedMax(s_Fit[1], EncodeOrderedFloat(farthest));
		InterlockedAdd(s_Fit[c_SurfacePixels], pixels);
	}
	GroupMemoryBarrierWithGroupSync();

	for (uint word = thread; word < c_Words; word += 256u)
	{
		uint value = s_Fit[word];
		if (value == 0u)
			continue;
		if (word == c_SurfacePixels || (word >= c_FirstBin && (word - c_FirstBin) % c_FieldsPerBin == c_BinPixels))
			InterlockedAdd(o_Fit[word], value);
		else
			InterlockedMax(o_Fit[word], value);
	}
}

// --- Entry point ---
[numthreads(16, 16, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
