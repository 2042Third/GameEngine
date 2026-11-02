#include "Include/Common.slang"

#include "Include/Scene.slang"

// Ambient occlusion's denoiser, which needs no history: GTAO.comp turns its slices by a fixed 4x4 pattern of 16 rotations
// (never by the frame), so every 4x4 block of pixels holds all of them. Per pixel this pass first resolves the pattern, a
// 5x5 bilateral average whose tent weights (0.5, 1, 1, 1, 0.5) in x and y weigh each of the 16 phases exactly once while the
// kernel stays centered, kept to the pixel's surface by depth and normal weights; then it blurs the resolved occlusion over
// 5x5 pixels within depth edges (Strata 0.6.0's AOBlur). Both run separably, a row of 5 then a column of 5, each tap weighed
// against the pixel it is gathered for: on a surface facing the camera that is the 5x5 kernels' weighting exactly, and on
// any other within the small depth differences of neighbors, at under half of their taps (Strata 0.6.0's GPU.AO cost at
// Medium). One dispatch of 16x16 groups: each loads the 24x24 tile around its pixels (raw occlusion, linear depth
// and normal) into group-shared memory, resolves rows then columns for the 20x20 pixels its blur reads, and blurs rows then
// columns into its 16x16. Background pixels (depth 0) write 1 and weigh nothing. With ST_AO_HALF (registered as
// Effects/AODenoiseHalf.comp) it writes a half-precision target.

Texture2D<float4> u_Depth : register(t8);
Texture2D<float4> u_Normals : register(t9);
Texture2D<float4> u_Input : register(t10);
#ifdef ST_AO_HALF
[vk::image_format("r16f")] RWTexture2D<float4> o_Output : register(u0);
#else
[vk::image_format("r32f")] RWTexture2D<float4> o_Output : register(u0);
#endif

static const int c_Group = 16;
static const int c_Threads = c_Group * c_Group;
static const int c_TileBorder = 4;                    // The resolve's 2 pixels plus the blur's 2
static const int c_Tile = c_Group + 2 * c_TileBorder; // 24: tile coordinates 0 to 23, the group's pixels at 4 to 19
static const int c_Resolved = c_Group + 4;            // 20: the resolved pixels the blur reads, tile coordinates 2 to 21
static const float c_DepthSharpness = 50.0;           // exp(-50 x relative depth difference), as the 0.6.0 blur

groupshared float s_Occlusion[c_Tile * c_Tile];
groupshared float4 s_NormalDepth[c_Tile * c_Tile];        // xyz: the normal, w: linear view depth (0 for the background)
groupshared float s_ResolvedRows[c_Resolved * c_Tile];  // Columns 2 to 21, rows 0 to 23: the resolve along rows
groupshared float s_Resolved[c_Resolved * c_Resolved];  // Columns and rows 2 to 21: the pattern resolved
groupshared float s_BlurredRows[c_Group * c_Resolved];  // Columns 4 to 19, rows 2 to 21: the blur along rows

float LinearDepth(float depth)
{
	// Distance along the view axis; with reversed-Z, depth falls toward 0 with distance.
	float4 view = mul(u_Frame.InverseProjection, float4(0.0, 0.0, depth, 1.0));
	return -view.z / view.w;
}

float TentWeight(int offset)
{
	return abs(offset) == 2 ? 0.5 : 1.0;
}

// The normal weight: the cosine between the normals to the 8th power.
float NormalWeight(float3 a, float3 b)
{
	float cosine = max(dot(a, b), 0.0);
	float square = cosine * cosine;
	float fourth = square * square;
	return fourth * fourth;
}

int TileIndex(int x, int y)
{
	return y * c_Tile + x;
}

// The pattern's resolve at tile pixel `center` over the 5 values from offset -2 to 2 along a row (`alongRows`: the raw
// occlusion) or a column (the row resolve's results), weighted by the tent and by the taps' depth and normal against the
// center's.
float ResolveAlong(int2 center, bool alongRows)
{
	float4 centerNormalDepth = s_NormalDepth[TileIndex(center.x, center.y)];
	if (centerNormalDepth.w <= 0.0)
		return 1.0;
	int2 step = alongRows ? int2(1, 0) : int2(0, 1);
	float depthScale = -c_DepthSharpness / centerNormalDepth.w;
	float sum = 0.0;
	float weightSum = 0.0;
	for (int offset = -2; offset <= 2; offset++)
	{
		int2 tap = center + step * offset;
		float4 normalDepth = s_NormalDepth[TileIndex(tap.x, tap.y)];
		if (normalDepth.w <= 0.0)
			continue;
		float weight = TentWeight(offset) * exp(depthScale * abs(normalDepth.w - centerNormalDepth.w))
			* NormalWeight(normalDepth.xyz, centerNormalDepth.xyz);
		float value = alongRows ? s_Occlusion[TileIndex(tap.x, tap.y)] : s_ResolvedRows[tap.y * c_Resolved + tap.x - 2];
		sum += value * weight;
		weightSum += weight;
	}
	return sum / weightSum; // The center weighs 1
}

void st_Main()
{
	int2 size = int2(u_Frame.ViewportSize.xy);
	int2 tileOrigin = int2(st_WorkGroupID.xy) * c_Group - c_TileBorder;
	int thread = int(st_LocalInvocationIndex);

	// The tile, clamped to the image (edge pixels repeat, as the 0.6.0 blur's samples did).
	for (int index = thread; index < c_Tile * c_Tile; index += c_Threads)
	{
		int2 pixel = clamp(tileOrigin + int2(index % c_Tile, index / c_Tile), int2(0), size - 1);
		float depth = u_Depth.Load(int3(pixel, 0)).r;
		s_Occlusion[index] = u_Input.Load(int3(pixel, 0)).r;
		s_NormalDepth[index] = depth > 0.0 ? float4(OctahedralDecode(u_Normals.Load(int3(pixel, 0)).rg), LinearDepth(depth)) : float4(0.0);
	}
	GroupMemoryBarrierWithGroupSync();

	// The resolve along rows: columns 2 to 21 of every row.
	for (int index = thread; index < c_Resolved * c_Tile; index += c_Threads)
	{
		s_ResolvedRows[index] = ResolveAlong(int2(index % c_Resolved + 2, index / c_Resolved), true);
	}
	GroupMemoryBarrierWithGroupSync();

	// Then along columns: rows 2 to 21 of those columns.
	for (int index = thread; index < c_Resolved * c_Resolved; index += c_Threads)
	{
		s_Resolved[index] = ResolveAlong(int2(index % c_Resolved + 2, index / c_Resolved + 2), false);
	}
	GroupMemoryBarrierWithGroupSync();

	// The 0.6.0 blur over the resolved occlusion, 5 by 5 weighted by depth: along rows (columns 4 to 19, rows 2 to 21)...
	for (int index = thread; index < c_Group * c_Resolved; index += c_Threads)
	{
		int2 center = int2(index % c_Group + c_TileBorder, index / c_Group + 2);
		float centerDepth = s_NormalDepth[TileIndex(center.x, center.y)].w;
		float result = 1.0;
		if (centerDepth > 0.0)
		{
			float depthScale = -c_DepthSharpness / centerDepth;
			float sum = 0.0;
			float weightSum = 0.0;
			for (int offset = -2; offset <= 2; offset++)
			{
				float sampleDepth = s_NormalDepth[TileIndex(center.x + offset, center.y)].w;
				if (sampleDepth <= 0.0)
					continue;
				float weight = exp(depthScale * abs(sampleDepth - centerDepth));
				sum += s_Resolved[(center.y - 2) * c_Resolved + center.x + offset - 2] * weight;
				weightSum += weight;
			}
			result = weightSum > 0.0 ? sum / weightSum : 1.0;
		}
		s_BlurredRows[index] = result;
	}
	GroupMemoryBarrierWithGroupSync();

	// ...then along columns into the group's pixels.
	int2 pixel = int2(st_GlobalInvocationID.xy);
	if (pixel.x >= size.x || pixel.y >= size.y)
		return;
	int2 center = int2(st_LocalInvocationID.xy) + c_TileBorder;
	float centerDepth = s_NormalDepth[TileIndex(center.x, center.y)].w;
	if (centerDepth <= 0.0)
	{
		o_Output[pixel] = float4(1.0);
		return;
	}
	float depthScale = -c_DepthSharpness / centerDepth;
	float sum = 0.0;
	float weightSum = 0.0;
	for (int offset = -2; offset <= 2; offset++)
	{
		float sampleDepth = s_NormalDepth[TileIndex(center.x, center.y + offset)].w;
		if (sampleDepth <= 0.0)
			continue;
		float weight = exp(depthScale * abs(sampleDepth - centerDepth));
		sum += s_BlurredRows[(center.y + offset - 2) * c_Group + center.x - c_TileBorder] * weight;
		weightSum += weight;
	}
	o_Output[pixel] = float4(weightSum > 0.0 ? sum / weightSum : 1.0);
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
