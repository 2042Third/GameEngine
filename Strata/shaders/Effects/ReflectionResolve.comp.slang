#include "Include/Common.slang"

#include "Include/Scene.slang"

// The spatial resolve of screen-space reflections at Spatial and SpatialHigh (SceneRenderer::TraceSpatialReflections, after
// Effects/ScreenSpaceReflectionsSpatial.comp, in GPU.Reflections), which needs no history: per pixel, the 3x3 neighborhood's
// traced reflections (premultiplied by their confidence) averaged as a ratio estimator. A neighbor's ray left along its own
// mirror direction L; the weight of what it found is the center's GGX lobe D toward L (the half vector of the center's view
// and L) over the density the neighbor's ray stands for (its own lobe's peak, 1 / (pi alpha^2)), at most the center's own
// weight 1, times depth and normal weights as ambient occlusion's denoiser has them. A smooth center keeps its own sharp
// reflection (its lobe is far narrower than the neighbors' differences); a rough one borrows its neighbors' rays, which hides
// the march's fixed 4x4 offsets. One dispatch of 8x8 groups, each loading the 10x10 pixels around its own. Writes the texture
// the forward pass reads at SRV 18 (rgb times the confidence, a the confidence); background pixels write 0.

Texture2D<float4> u_Depth : register(t22);
Texture2D<float4> u_Surface : register(t23); // xy: octahedral view-space normal (normal-mapped), z: roughness
Texture2D<float4> u_Traced : register(t25);
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Reflections : register(u0);

static const int c_Group = 8;
static const int c_Tile = c_Group + 2;
static const float c_DepthSharpness = 50.0; // exp(-50 x relative depth difference), as AODenoise.comp's
static const float c_NormalPower = 8.0;
static const float c_MinRoughness = 0.045;  // PBR.glsl's lower bound of the perceptual roughness

groupshared float4 s_Traced[c_Tile * c_Tile];
groupshared float4 s_NormalAlpha[c_Tile * c_Tile]; // xyz: the view-space normal, w: alpha^2 (0 for the background)
groupshared float4 s_DirectionDepth[c_Tile * c_Tile]; // xyz: the view-space mirror direction, w: the linear depth

void st_Main()
{
	int2 size = int2(u_Frame.ViewportSize.xy);
	int2 tileOrigin = int2(st_WorkGroupID.xy) * c_Group - 1;
	bool orthographic = u_Frame.CameraForward.w > 0.5;
	for (uint index = st_LocalInvocationIndex; index < uint(c_Tile * c_Tile); index += uint(c_Group * c_Group))
	{
		int2 pixel = clamp(tileOrigin + int2(int(index) % c_Tile, int(index) / c_Tile), int2(0), size - 1);
		float depth = u_Depth.Load(int3(pixel, 0)).r;
		s_Traced[index] = u_Traced.Load(int3(pixel, 0));
		if (depth <= 0.0)
		{
			s_NormalAlpha[index] = float4(0.0);
			s_DirectionDepth[index] = float4(0.0);
			continue;
		}
		float4 surface = u_Surface.Load(int3(pixel, 0));
		float3 normal = OctahedralDecode(surface.xy);
		float roughness = max(surface.z, c_MinRoughness);
		float alpha = roughness * roughness;
		float2 uv = (float2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
		float4 viewPoint = mul(u_Frame.InverseProjection, float4(UVToNDC(uv), depth, 1.0));
		float3 position = viewPoint.xyz / viewPoint.w;
		float3 incident = orthographic ? float3(0.0, 0.0, -1.0) : normalize(position);
		s_NormalAlpha[index] = float4(normal, alpha * alpha);
		s_DirectionDepth[index] = float4(reflect(incident, normal), -position.z);
	}
	GroupMemoryBarrierWithGroupSync();

	int2 pixel = int2(st_GlobalInvocationID.xy);
	if (pixel.x >= size.x || pixel.y >= size.y)
		return;
	int2 center = int2(st_LocalInvocationID.xy) + 1;
	int centerIndex = center.y * c_Tile + center.x;
	float4 centerNormalAlpha = s_NormalAlpha[centerIndex];
	if (centerNormalAlpha.w <= 0.0)
	{
		o_Reflections[pixel] = float4(0.0);
		return;
	}
	float3 centerNormal = centerNormalAlpha.xyz;
	float centerAlpha2 = centerNormalAlpha.w;
	float centerDepth = s_DirectionDepth[centerIndex].w;
	// The center's direction toward the camera: its mirror direction reflected back about its normal is the incident one.
	float3 view = -reflect(s_DirectionDepth[centerIndex].xyz, centerNormal);
	float4 sum = s_Traced[centerIndex];
	float weightSum = 1.0;
	for (int y = -1; y <= 1; y++)
	{
		for (int x = -1; x <= 1; x++)
		{
			if (x == 0 && y == 0)
				continue;
			int sampleIndex = (center.y + y) * c_Tile + center.x + x;
			float4 normalAlpha = s_NormalAlpha[sampleIndex];
			if (normalAlpha.w <= 0.0)
				continue;
			float4 directionDepth = s_DirectionDepth[sampleIndex];
			float depthWeight = exp(-c_DepthSharpness * abs(directionDepth.w - centerDepth) / centerDepth);
			float normalWeight = pow(max(dot(normalAlpha.xyz, centerNormal), 0.0), c_NormalPower);
			float3 halfVector = normalize(view + directionDepth.xyz);
			float nDotH = max(dot(centerNormal, halfVector), 0.0);
			float denominator = nDotH * nDotH * (centerAlpha2 - 1.0) + 1.0;
			// pi alpha_i^2 x D_center(H), with D = alpha_c^2 / (pi denominator^2): the pis cancel.
			float lobeWeight = min(normalAlpha.w * centerAlpha2 / max(denominator * denominator, 1e-12), 1.0);
			float weight = lobeWeight * depthWeight * normalWeight;
			sum += s_Traced[sampleIndex] * weight;
			weightSum += weight;
		}
	}
	o_Reflections[pixel] = sum / weightSum;
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
