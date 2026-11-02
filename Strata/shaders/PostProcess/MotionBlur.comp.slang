#include "Include/Common.slang"

#include "Include/Dither.slang"
#include "Include/MotionBlur.slang"

// The motion blur's gather (PostProcessMath::MotionBlurProfile is its CPU model): a reconstruction filter on the principle of
// the tile-max / neighbor-max filters, designed here. Where the neighborhood's largest blur is under half a pixel, the pixel
// keeps its color exactly (alpha 0). Otherwise SampleCount point samples spread over the neighborhood's blur, stratified over
// its whole span with one static offset per pixel (no ghost copies, nothing temporal); even samples follow the neighborhood's
// direction, odd ones the pixel's own when it moves at least 1.5 pixels. Each sample counts by depth: a nearer sample whose blur
// reaches the center spreads over it (a moving foreground over the background), a farther one counts when the center's own
// blur reaches it, and parts of one surface blur together; the center counts as a sample at distance 0. A neighborhood whose
// blurs all lie within GetUniformTolerance of its largest moves as one surface: its samples take that surface's weights from
// their distance alone and read only colors. Alpha 1 marks the blurred pixels (the tone mapping does not sharpen them); where
// the color is copied, the depth of field's mark is kept when the source is its output (SourceMask), else 0.

Texture2D<float4> u_Velocity : register(t0);
Texture2D<float4> u_Depth : register(t1);
Texture2D<float4> u_NeighborMax : register(t2);
Texture2D<float4> u_Color : register(t3);
Texture2D<uint4> u_Pixels : register(t4);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Blurred : register(u0);

// Depth classification: a sample within 5% of the center's distance lies on the same surface.
static const float c_DepthTolerance = 0.05;

float Cover(float distance, float radius)
{
	return clamp(1.0 - distance / radius, 0.0, 1.0);
}

float Span(float distance, float radius)
{
	return 1.0 - smoothstep(0.95 * radius, 1.05 * radius, distance);
}

float SampleWeight(float distance, float sampleDistance, float sampleRadius, float centerDistance, float centerRadius)
{
	float inFront = clamp(1.0 - (sampleDistance - centerDistance) / (c_DepthTolerance * centerDistance), 0.0, 1.0);
	float behind = clamp(1.0 - (centerDistance - sampleDistance) / (c_DepthTolerance * centerDistance), 0.0, 1.0);
	return inFront * Cover(distance, sampleRadius) + behind * Cover(distance, centerRadius)
		+ 2.0 * Span(distance, sampleRadius) * Span(distance, centerRadius);
}

// A sample's position along the span, in [-1, 1]: stratified, the pixel's static offset `jitter` in [-0.5, 0.5).
float SamplePosition(uint index, uint count, float jitter)
{
	return -1.0 + (2.0 * float(index) + 1.0 + jitter) / float(count);
}

int2 SamplePixel(int2 pixel, float2 offset, int2 size)
{
	return clamp(int2(floor(float2(pixel) + 0.5 + offset)), int2(0), size - 1);
}

void st_Main()
{
	int2 pixel = int2(st_GlobalInvocationID.xy);
	int2 size = TextureSize(u_Color, 0);
	if (any((pixel >= size)))
		return;
	float4 center = u_Color.Load(int3(pixel, 0));
	int2 tile = pixel / int(u_MotionBlur.TileSize);
	float3 neighbor = u_NeighborMax.Load(int3(tile, 0)).xyz;
	float neighborLength = length(neighbor.xy);
	float neighborRadius = 0.5 * neighborLength;
	if (neighborRadius < 0.5)
	{
		o_Blurred[pixel] = float4(center.rgb, u_MotionBlur.SourceMask != 0u ? center.a : 0.0);
		return;
	}
	float2 neighborDirection = neighbor.xy / neighborLength;
	uint count = u_MotionBlur.SampleCount;
	float jitter = PixelRandom(uint2(pixel), 6u) - 0.5;

	if (neighbor.z <= GetUniformTolerance(neighborLength))
	{
		// One surface moving uniformly: every radius is the neighborhood's, every sample on the center's surface, so a sample's
		// weight is SampleWeight's of one surface (2 Cover + 2 Span^2) at its distance, and no sample reaches the span's ends
		// (|position| <= 1 - 0.5 / count < 0.95), where Span would fall.
		float totalWeight = 4.0;
		float3 sum = center.rgb * totalWeight;
		for (uint index = 0u; index < count; index++)
		{
			float position = SamplePosition(index, count, jitter);
			float weight = 2.0 * (1.0 - abs(position)) + 2.0;
			sum += u_Color.Load(int3(SamplePixel(pixel, position * neighborRadius * neighborDirection, size), 0)).rgb * weight;
			totalWeight += weight;
		}
		o_Blurred[pixel] = float4(min(sum / totalWeight, float3(c_MaxHDRValue)), 1.0);
		return;
	}

	float2 uv = (float2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	float2 centerVelocity = GetBlurVelocity(u_Velocity.Load(int3(pixel, 0)).rg, u_Depth.Load(int3(pixel, 0)).r, uv);
	float centerSpeed = length(centerVelocity);
	float2 centerPixel = UnpackBlurPixel(u_Pixels.Load(int3(pixel, 0)).r);
	float centerRadius = centerPixel.x;
	float centerDistance = centerPixel.y;
	float2 ownDirection = centerSpeed >= 1.5 ? centerVelocity / centerSpeed : neighborDirection;

	// The center: a sample at distance 0 on its own surface.
	float totalWeight = SampleWeight(0.0, centerDistance, centerRadius, centerDistance, centerRadius);
	float3 sum = center.rgb * totalWeight;
	for (uint index = 0u; index < count; index++)
	{
		float position = SamplePosition(index, count, jitter);
		float2 direction = (index & 1u) == 0u ? neighborDirection : ownDirection;
		int2 samplePixel = SamplePixel(pixel, position * neighborRadius * direction, size);
		float2 sampleData = UnpackBlurPixel(u_Pixels.Load(int3(samplePixel, 0)).r);
		float weight = SampleWeight(abs(position) * neighborRadius, sampleData.y, sampleData.x, centerDistance, centerRadius);
		sum += u_Color.Load(int3(samplePixel, 0)).rgb * weight;
		totalWeight += weight;
	}
	o_Blurred[pixel] = float4(min(sum / totalWeight, float3(c_MaxHDRValue)), 1.0);
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
