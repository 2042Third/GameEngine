#include "Include/Common.slang"

#include "Include/Scene.slang"

// Ground-truth ambient occlusion (horizon-based, cosine-weighted visibility per screen-space slice) from the
// prepass depth and normals. Each pixel integrates a few slices turned by a fixed 4x4 pattern (never by the frame: nothing
// needs a history), which AODenoise.comp resolves. A thin occluder's raised horizon relaxes back toward the samples behind
// it, so poles and plates do not draw dark halos on the walls behind them. With ST_AO_HALF (registered as
// Effects/GTAOHalf.comp) it writes a half-precision target.

Texture2D<float4> u_Depth : register(t8);
Texture2D<float4> u_Normals : register(t9);
#ifdef ST_AO_HALF
[vk::image_format("r16f")] RWTexture2D<float4> o_AmbientOcclusion : register(u0);
#else
[vk::image_format("r32f")] RWTexture2D<float4> o_AmbientOcclusion : register(u0);
#endif

struct AOParameters
{
	float Radius;            // Meters
	float Intensity;         // Exponent applied to the visibility
	uint SliceCount;
	uint StepsPerSide;
	float MinRadiusPixels;   // The radius on screen at least...
	float MaxRadiusPixels;   // ...and at most this many pixels
	float ThinOccluderDecay; // How far a horizon relaxes toward a lower sample per step
	uint Padding;
};
[[vk::push_constant]] ConstantBuffer<AOParameters> u_AO;

// The 4x4 ordered-dither order (as IsLODFadeDrawn's): every 4x4 block of pixels holds each of the 16 values once.
static const uint c_PatternOrder[16] = { 0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u };

float3 ViewPositionFromDepth(float2 uv, float depth)
{
	float4 view = mul(u_Frame.InverseProjection, float4(UVToNDC(uv), depth, 1.0));
	return view.xyz / view.w;
}

void st_Main()
{
	int2 pixel = int2(st_GlobalInvocationID.xy);
	int2 size = int2(u_Frame.ViewportSize.xy);
	if (pixel.x >= size.x || pixel.y >= size.y)
		return;

	float depth = u_Depth.Load(int3(pixel, 0)).r;
	if (depth <= 0.0) // Background (reversed-Z far plane)
	{
		o_AmbientOcclusion[pixel] = float4(1.0);
		return;
	}

	float2 uv = (float2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	float3 position = ViewPositionFromDepth(uv, depth);
	// Toward the viewer: along the view axis for orthographic projections, toward the eye for perspective ones.
	float3 view = u_Frame.CameraForward.w > 0.5 ? float3(0.0, 0.0, 1.0) : normalize(-position);
	float3 normal = OctahedralDecode(u_Normals.Load(int3(pixel, 0)).rg);

	// World radius projected to pixels (orthographic projections have a constant scale).
	float pixelsPerUnit = u_Frame.Projection[1][1] * 0.5 * u_Frame.ViewportSize.y;
	if (u_Frame.CameraForward.w < 0.5)
		pixelsPerUnit /= max(-position.z, 1e-4);
	// The radius on screen within its fractions of the view's height (cost and look independent of the resolution: distant
	// creases keep a line, near surfaces do not reach across the screen); the world radius follows it.
	float radiusPixels = clamp(u_AO.Radius * pixelsPerUnit, u_AO.MinRadiusPixels, u_AO.MaxRadiusPixels);
	float radius = radiusPixels / pixelsPerUnit;

	// The slices' rotation and the steps' offset from the 4x4 pattern (two orders of it, so they do not follow each other).
	uint2 cell = uint2(pixel) & 3u;
	float noise = (float(c_PatternOrder[cell.y * 4u + cell.x]) + 0.5) / 16.0;
	float jitter = (float(c_PatternOrder[cell.x * 4u + cell.y]) + 0.5) / 16.0;
	float falloffStart = radius * 0.6;
	// Samples closer than this many pixels would read the pixel itself.
	float minStep = 1.3 / radiusPixels;
	float visibility = 0.0;
	for (uint slice = 0u; slice < u_AO.SliceCount; slice++)
	{
		float phi = (float(slice) + noise) * PI / float(u_AO.SliceCount);
		float3 direction = float3(cos(phi), sin(phi), 0.0); // View space; screen space y points down
		float2 screenOffset = float2(direction.x, -direction.y) * radiusPixels;

		// Normal projected into the slice plane, and its angle to the view vector.
		float3 orthoDirection = direction - dot(direction, view) * view;
		float3 axis = normalize(cross(direction, view));
		float3 projectedNormal = normal - axis * dot(normal, axis);
		float projectedLength = length(projectedNormal);
		if (projectedLength < 1e-4)
		{
			visibility += 1.0;
			continue;
		}
		float signNormal = sign(dot(orthoDirection, projectedNormal));
		float cosNormal = clamp(dot(projectedNormal, view) / projectedLength, 0.0, 1.0);
		float n = signNormal * acos(cosNormal);

		// Horizons start at the edge of the hemisphere around the normal (nothing occludes).
		float2 lowHorizonCos = float2(cos(n + 0.5 * PI), cos(n - 0.5 * PI));
		float2 horizonCos = lowHorizonCos;
		for (int side = 0; side < 2; side++)
		{
			float sideSign = side == 0 ? 1.0 : -1.0;
			for (uint step = 0u; step < u_AO.StepsPerSide; step++)
			{
				float t = (float(step) + jitter) / float(u_AO.StepsPerSide);
				// Denser samples near the pixel. Samples are snapped to pixel centers: a position rebuilt from another
				// point of the pixel than its depth belongs to would leave the surface and occlude flat ground.
				int2 samplePixel = pixel + int2(round(screenOffset * (t * t + minStep))) * int(sideSign);
				if (any((samplePixel < int2(0))) || any((samplePixel >= size)))
					break;
				float2 sampleUV = (float2(samplePixel) + 0.5) * u_Frame.ViewportSize.zw;
				float sampleDepth = u_Depth.Load(int3(samplePixel, 0)).r;
				if (sampleDepth <= 0.0)
					continue;
				float3 delta = ViewPositionFromDepth(sampleUV, sampleDepth) - position;
				float distance = length(delta);
				if (distance < 1e-5)
					continue;
				// Occluders fade out toward the radius so distant geometry does not darken.
				float weight = clamp((radius - distance) / max(radius - falloffStart, 1e-4), 0.0, 1.0);
				float sampleCos = lerp(lowHorizonCos[side], dot(delta / distance, view), weight);
				// A sample above the horizon raises it; one below relaxes it toward itself (a thin occluder is seen by few
				// samples, after which they fall back; a thick one's stay high and keep its occlusion).
				horizonCos[side] = sampleCos > horizonCos[side] ? sampleCos
					: max(lerp(horizonCos[side], sampleCos, u_AO.ThinOccluderDecay), lowHorizonCos[side]);
			}
		}

		// Horizon angles relative to the view vector, clamped to the hemisphere around the normal.
		float h0 = n + clamp(-acos(horizonCos.y) - n, -0.5 * PI, 0.5 * PI);
		float h1 = n + clamp(acos(horizonCos.x) - n, -0.5 * PI, 0.5 * PI);
		float sinN = sin(n);
		float arc0 = -cos(2.0 * h0 - n) + cosNormal + 2.0 * h0 * sinN;
		float arc1 = -cos(2.0 * h1 - n) + cosNormal + 2.0 * h1 * sinN;
		visibility += projectedLength * 0.25 * (arc0 + arc1);
	}

	float ao = clamp(visibility / float(u_AO.SliceCount), 0.0, 1.0);
	o_AmbientOcclusion[pixel] = float4(pow(ao, u_AO.Intensity));
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
