#include "Include/Common.slang"

#include "Include/GlobalIllumination.slang"
#include "Include/Scene.slang"

// The voxels debug view (SceneLightingView::Voxels): global illumination's voxels as the GI sees the scene, replacing the
// finished image. From the camera, a march through the voxel levels (each point read from the finest level that holds it,
// blended into the next by GetLevelWeight; steps of ST_GI_STEP_VOXELS voxels of the level read, at most 512) to the first
// sample whose trilinear occupancy reaches 0.5. Its surface is read where the run of occupied samples began (less than a step
// past the last empty voxel center: the layer the ray entered, never the one behind it): the radiance of the surface voxels
// around that point (rgb / a) weighted trilinearly in the levels it blends, through the frame's exposure (its pre-exposure, as
// the probes view) and the display curve x / (1 + x), and no darker than their albedo times c_DarkAlbedo (where no light
// arrives, dark geometry stays legible); two-sided voxels in the two-sided color, nothing hit in the background color
// (LightingViewColors, as push constants). sRGB display colors.

Texture3D<float4> u_Material0 : register(t0);
Texture3D<float4> u_Material1 : register(t1);
Texture3D<float4> u_Material2 : register(t2);
Texture3D<float4> u_Material3 : register(t3);
Texture3D<float4> u_Normal0 : register(t4);
Texture3D<float4> u_Normal1 : register(t5);
Texture3D<float4> u_Normal2 : register(t6);
Texture3D<float4> u_Normal3 : register(t7);
Texture3D<float4> u_Radiance0 : register(t8);
Texture3D<float4> u_Radiance1 : register(t9);
Texture3D<float4> u_Radiance2 : register(t10);
Texture3D<float4> u_Radiance3 : register(t11);
SamplerState u_Repeat : register(s0);

// RenderData::VoxelViewParameters: display colors (sRGB, RGBA8 packed) of two-sided voxels and of nothing hit.
struct VoxelViewParameters
{
	uint TwoSided;
	uint Background;
	uint Padding0;
	uint Padding1;
};
[[vk::push_constant]] ConstantBuffer<VoxelViewParameters> u_View;

static const uint c_MaxSteps = 512u;
static const float c_DarkAlbedo = 0.03;

float4 SampleMaterial(uint level, float3 coordinate)
{
	if (level == 0u)
		return u_Material0.SampleLevel(u_Repeat, coordinate, 0.0);
	if (level == 1u)
		return u_Material1.SampleLevel(u_Repeat, coordinate, 0.0);
	if (level == 2u)
		return u_Material2.SampleLevel(u_Repeat, coordinate, 0.0);
	return u_Material3.SampleLevel(u_Repeat, coordinate, 0.0);
}

float4 LoadMaterial(uint level, int3 texel)
{
	if (level == 0u)
		return u_Material0.Load(int4(texel, 0));
	if (level == 1u)
		return u_Material1.Load(int4(texel, 0));
	if (level == 2u)
		return u_Material2.Load(int4(texel, 0));
	return u_Material3.Load(int4(texel, 0));
}

float4 LoadRadiance(uint level, int3 texel)
{
	if (level == 0u)
		return u_Radiance0.Load(int4(texel, 0));
	if (level == 1u)
		return u_Radiance1.Load(int4(texel, 0));
	if (level == 2u)
		return u_Radiance2.Load(int4(texel, 0));
	return u_Radiance3.Load(int4(texel, 0));
}

float4 LoadNormal(uint level, int3 texel)
{
	if (level == 0u)
		return u_Normal0.Load(int4(texel, 0));
	if (level == 1u)
		return u_Normal1.Load(int4(texel, 0));
	if (level == 2u)
		return u_Normal2.Load(int4(texel, 0));
	return u_Normal3.Load(int4(texel, 0));
}

// The occupancy at a camera-relative point, blended over the levels that hold it, and the finest of them (ST_GI_MAX_VOXEL_LEVELS
// when none does).
float SampleOccupancy(float3 position, out uint finest)
{
	float occupancy = 0.0;
	float remaining = 1.0;
	finest = ST_GI_MAX_VOXEL_LEVELS;
	for (uint level = 0u; level < u_GI.Dimensions.w && remaining > 0.0; level++)
	{
		float weight = GetLevelWeight(level, position);
		if (weight <= 0.0)
			continue;
		if (finest == ST_GI_MAX_VOXEL_LEVELS)
			finest = level;
		occupancy += remaining * weight * SampleMaterial(level, GetVoxelTextureCoordinate(level, position)).a;
		remaining *= 1.0 - weight;
	}
	return occupancy;
}

struct VoxelSurface
{
	float3 Albedo;
	float4 Radiance; // rgb weighted (luminance times ST_GI_RADIANCE_SCALE), a the weight of the voxels holding light
	float3 Normal;
	float TwoSided; // The share of the surface voxels' weight that is two-sided
	float Weight;   // Of the surface voxels read (0: none)
};

// The surface voxels of `level` around a camera-relative point, weighted as trilinear filtering weighs them.
VoxelSurface ReadSurface(uint level, float3 position)
{
	VoxelSurface surface = (VoxelSurface)0;
	GIVoxelLevel data = u_GI.Levels[level];
	float3 local = (position - data.Origin.xyz) / data.Origin.w - 0.5;
	int3 base = int3(floor(local));
	float3 fraction = local - float3(base);
	float twoSided = 0.0;
	for (uint corner = 0u; corner < 8u; corner++)
	{
		int3 offset = int3(int(corner & 1u), int((corner >> 1) & 1u), int((corner >> 2) & 1u));
		float3 weights = select(offset != int3(0, 0, 0), fraction, 1.0 - fraction);
		float weight = weights.x * weights.y * weights.z;
		int3 texel = GetVoxelTexel(level, base + offset);
		float4 normal = LoadNormal(level, texel);
		if (normal.w == 0.0 || weight <= 0.0)
			continue;
		surface.Albedo += weight * LoadMaterial(level, texel).rgb;
		surface.Radiance += weight * LoadRadiance(level, texel);
		surface.Normal += weight * normal.xyz;
		twoSided += normal.w < 0.0 ? weight : 0.0;
		surface.Weight += weight;
	}
	if (surface.Weight > 0.0)
	{
		surface.Albedo /= surface.Weight;
		surface.TwoSided = twoSided / surface.Weight;
		float normalLength = length(surface.Normal);
		surface.Normal = normalLength > 0.0 ? surface.Normal / normalLength : float3(0.0, 0.0, 0.0);
	}
	return surface;
}

// The surface around a camera-relative point as the levels holding it blend there (GetLevelWeight), as the occupancy is: in a
// blend the coarser level's wider footprint finds the surface first.
VoxelSurface ReadBlendedSurface(float3 position)
{
	VoxelSurface blended = (VoxelSurface)0;
	float remaining = 1.0;
	float twoSided = 0.0;
	for (uint level = 0u; level < u_GI.Dimensions.w && remaining > 0.0; level++)
	{
		float weight = GetLevelWeight(level, position);
		if (weight <= 0.0)
			continue;
		VoxelSurface surface = ReadSurface(level, position);
		float share = remaining * weight * surface.Weight;
		blended.Albedo += share * surface.Albedo;
		blended.Radiance += remaining * weight * surface.Radiance;
		blended.Normal += share * surface.Normal;
		twoSided += share * surface.TwoSided;
		blended.Weight += share;
		remaining *= 1.0 - weight;
	}
	if (blended.Weight > 0.0)
	{
		blended.Albedo /= blended.Weight;
		blended.TwoSided = twoSided / blended.Weight;
		float normalLength = length(blended.Normal);
		blended.Normal = normalLength > 0.0 ? blended.Normal / normalLength : float3(0.0, 0.0, 0.0);
	}
	return blended;
}

float4 main(float4 fragCoord : SV_Position) : SV_Target0
{
	// The pixel's ray, camera-relative: from the near plane (depth 1 with reversed-Z) through a point behind it, which serves
	// perspective and orthographic cameras alike.
	float2 uv = fragCoord.xy * u_Frame.ViewportSize.zw;
	float2 ndc = UVToNDC(uv);
	float4 nearPoint = mul(u_Frame.InverseViewProjection, float4(ndc, 1.0, 1.0));
	float4 farPoint = mul(u_Frame.InverseViewProjection, float4(ndc, 0.5, 1.0));
	float3 origin = nearPoint.xyz / nearPoint.w;
	float3 direction = normalize(farPoint.xyz / farPoint.w - origin);

	float t = 0.0;
	bool inRun = false;
	VoxelSurface surface = (VoxelSurface)0;
	for (uint marched = 0u; marched < c_MaxSteps; marched++)
	{
		float3 position = origin + direction * t;
		uint level;
		float occupancy = SampleOccupancy(position, level);
		if (level == ST_GI_MAX_VOXEL_LEVELS)
			break; // Outside every level
		if (occupancy > 0.0)
		{
			// The surface where the run began, or (where that read found only interior voxels) the first sample of the run that
			// found one.
			if (!inRun || surface.Weight <= 0.0)
				surface = ReadBlendedSurface(position);
			inRun = true;
			if (occupancy >= 0.5)
			{
				if (!(surface.Weight > 0.0))
					break; // Only interior voxels where the run began: no surface to show
				if (surface.TwoSided > 0.5)
					return float4(UnpackUnorm4x8(u_View.TwoSided).rgb, 1.0);
				float3 radiance = surface.Radiance.a > 0.0 ? max(surface.Radiance.rgb, float3(0.0, 0.0, 0.0)) / surface.Radiance.a : float3(0.0, 0.0, 0.0);
				float3 exposed = radiance / ST_GI_RADIANCE_SCALE * u_Frame.TemporalParams.y;
				return float4(LinearToSRGB(max(exposed / (1.0 + exposed), surface.Albedo * c_DarkAlbedo)), 1.0);
			}
		}
		else
		{
			inRun = false;
		}
		t += ST_GI_STEP_VOXELS * u_GI.Levels[level].Origin.w;
	}
	return float4(UnpackUnorm4x8(u_View.Background).rgb, 1.0);
}
