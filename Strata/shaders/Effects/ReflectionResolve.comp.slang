#include "Include/Common.slang"

#include "Include/Scene.slang"

// The spatial resolve of screen-space reflections at Spatial and SpatialHigh (SceneRenderer::TraceSpatialReflections, after
// Effects/ScreenSpaceReflectionsSpatial.comp, in GPU.Reflections), which needs no history: per pixel, the 5x5 neighborhood's
// traced reflections (premultiplied by their confidence) averaged as a ratio estimator under the separable kernel
// (0.5, 1, 1, 1, 0.5). Over five pixels that kernel gives each of the march's four offsets of an axis (its 4x4 order) the
// same weight, 1, so with weights uniform over a plane the offsets' pattern cancels exactly in both axes (the 3x3 window it
// replaces kept a third of it: the screen door of a rough sheet). A neighbor's ray left along its own mirror direction L; the
// weight of what it found is the kernel's times the center's GGX lobe D toward L (the half vector of the center's view and L)
// over the density the neighbor's ray stands for (its own lobe's peak, 1 / (pi alpha^2)), at most the center's own weight 1,
// times a plane and a normal weight. Those are uniform over a plane, whatever the angle it is seen at: the distance of the
// neighbor's view-space point from the center's tangent plane (its geometric normal) relative to the center's depth, as
// ambient occlusion's sharpness has it, and the geometric normals' agreement (normal maps do not make it vary across a
// sheet); a depth difference instead grows with every pixel along a grazing sheet (0.25% a pixel at 9 degrees) and weighed
// far neighbors less than near ones. A rough center borrows its neighbors' rays; a mirror's neighbors weigh less the more
// their mirror directions differ from its own (at 3840x2160 a pixel's angle still leaves them 0.97, a softening of about two
// pixels, which keeps no screen door either). One dispatch of 8x8 groups, each loading the 12x12 pixels
// around its own. Writes the texture the forward pass reads at SRV 18 (rgb times the confidence, a the confidence);
// background pixels write 0.

Texture2D<float4> u_Depth : register(t22);
Texture2D<float4> u_Surface : register(t23); // xy: octahedral view-space normal (normal-mapped), z: roughness
Texture2D<float4> u_Traced : register(t25);
Texture2D<float4> u_Normals : register(t26); // The prepass's geometric view-space normals (octahedral)
[vk::image_format("rgba16f")] RWTexture2D<float4> o_Reflections : register(u0);

static const int c_Group = 8;
static const int c_Tile = c_Group + 4;
static const float c_Kernel[5] = { 0.5, 1.0, 1.0, 1.0, 0.5 };
static const float c_PlaneSharpness = 50.0; // exp(-50 x the distance from the center's plane over its depth), as AODenoise.comp's
static const float c_NormalPower = 8.0;
static const float c_MinRoughness = 0.045;  // PBR.slang's lower bound of the perceptual roughness

groupshared float4 s_Traced[c_Tile * c_Tile];
groupshared float4 s_NormalAlpha[c_Tile * c_Tile]; // xyz: the view-space shading normal, w: alpha^2 (0 for the background)
groupshared float4 s_DirectionDepth[c_Tile * c_Tile]; // xyz: the view-space mirror direction, w: the linear depth
groupshared float4 s_Position[c_Tile * c_Tile]; // xyz: the view-space position
groupshared float4 s_Geometric[c_Tile * c_Tile]; // xyz: the view-space geometric normal

void st_Main()
{
	int2 size = int2(u_Frame.ViewportSize.xy);
	int2 tileOrigin = int2(st_WorkGroupID.xy) * c_Group - 2;
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
			s_Position[index] = float4(0.0);
			s_Geometric[index] = float4(0.0);
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
		s_Position[index] = float4(position, 0.0);
		s_Geometric[index] = float4(OctahedralDecode(u_Normals.Load(int3(pixel, 0)).rg), 0.0);
	}
	GroupMemoryBarrierWithGroupSync();

	int2 pixel = int2(st_GlobalInvocationID.xy);
	if (pixel.x >= size.x || pixel.y >= size.y)
		return;
	int2 center = int2(st_LocalInvocationID.xy) + 2;
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
	float3 centerPosition = s_Position[centerIndex].xyz;
	float3 centerGeometric = s_Geometric[centerIndex].xyz;
	// The center's direction toward the camera: its mirror direction reflected back about its normal is the incident one.
	float3 view = -reflect(s_DirectionDepth[centerIndex].xyz, centerNormal);
	float4 sum = s_Traced[centerIndex];
	float weightSum = 1.0;
	for (int y = -2; y <= 2; y++)
	{
		for (int x = -2; x <= 2; x++)
		{
			if (x == 0 && y == 0)
				continue;
			int sampleIndex = (center.y + y) * c_Tile + center.x + x;
			float4 normalAlpha = s_NormalAlpha[sampleIndex];
			if (normalAlpha.w <= 0.0)
				continue;
			float4 directionDepth = s_DirectionDepth[sampleIndex];
			float planeDistance = abs(dot(s_Position[sampleIndex].xyz - centerPosition, centerGeometric));
			float planeWeight = exp(-c_PlaneSharpness * planeDistance / centerDepth);
			float normalWeight = pow(max(dot(s_Geometric[sampleIndex].xyz, centerGeometric), 0.0), c_NormalPower);
			float3 halfVector = normalize(view + directionDepth.xyz);
			float nDotH = max(dot(centerNormal, halfVector), 0.0);
			float denominator = nDotH * nDotH * (centerAlpha2 - 1.0) + 1.0;
			// pi alpha_i^2 x D_center(H), with D = alpha_c^2 / (pi denominator^2): the pis cancel.
			float lobeWeight = min(normalAlpha.w * centerAlpha2 / max(denominator * denominator, 1e-12), 1.0);
			float weight = c_Kernel[x + 2] * c_Kernel[y + 2] * lobeWeight * planeWeight * normalWeight;
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
