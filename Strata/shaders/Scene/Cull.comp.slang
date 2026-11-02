#include "Include/Common.slang"

#include "Include/Culling.slang"
#include "Include/TextureStreaming.slang"

// GPU culling, one thread per render proxy: the view's frustum and screen-size tests, or with ST_CULL_SHADOW
// (registered as Scene/CullShadow.comp) the shadow cascades' boxes and sub-texel test and the local shadow views' (point and
// spot lights: range, frustum and sub-texel tests, a batch of views per pass), then per instance its material
// (transparent ones are the CPU's), its level of detail (ChooseLOD, Include/LODSelection.glsl) and its bucket. The decisions are the
// CPU path's (SceneRendererExtraction.cpp: CullView, FindShadowCasters, GatherDraws, GatherShadowDraws) on the same data.
//
// Mode 0 counts the instances of each (bucket, LOD record) and the frame's statistics; CullBuild.comp then places each
// record's range in the draw list; mode 1 runs the same decisions again and writes each instance into its record's range
// (counting the record down to zero). The order of instances within a record follows GPU scheduling, which opaque
// pixels do not depend on.
//
// With occlusion culling these passes are each phase's: the first draws what was visible in the previous frame (u_Visibility),
// the second (bit 1 of the mode, after a depth pyramid of the first phase's depth) tests everything else and draws what it
// finds visible. The view's phases run around its depth prepass; the cascades' (u_Cull.ShadowOcclusion) around each
// cascade's first-phase shadow map, with the pyramids of Scene/HZBBuildArray.comp in u_ShadowPyramid.
//
// Mode 0 also writes visibility feedback (with u_Cull.Feedback): the mesh and material slots of proxies within the prefetch
// distance, in the view (frustum and size tests) or casting into a cascade, whether their meshes are loaded or not
// (unloaded ones are placed by their recorded bounds), with the largest projected size and, for textured materials, UV
// pixels. CullFeedback.comp packs the touched slots for the CPU's requests (SceneRenderer::ApplyFeedback).
//
// The local shadow views a frame draws (u_LocalShadowViews) are culled in batches of at most ST_CULL_MAX_LOCAL_VIEWS: the
// first with the cascades, later ones (frames that draw more views) by passes over the local views alone (ST_CULL_MODE_LOCAL),
// each batch's draws made after the previous batch's were drawn.

struct CullParameters
{
	// Bit 0: write the draw list (else count); bit 1: the second phase (occlusion culling) of the view or the cascades; bit 3: the
	// local shadow views alone (no cascades); bits 8-15: the batch's first local shadow view, bits 16-23 its views
	uint Mode;
};
[[vk::push_constant]] ConstantBuffer<CullParameters> u_Parameters;

#define ST_CULL_MODE_WRITE        1u
#define ST_CULL_MODE_SECOND_PHASE 2u
#define ST_CULL_MODE_LOCAL        8u
#define ST_SHARED_COUNTERS (ST_COUNTER_LOCAL_SHADOW_INSTANCES + 1)

bool IsCounting()
{
	return (u_Parameters.Mode & ST_CULL_MODE_WRITE) == 0u;
}

// The workgroup's statistics, added to the frame's counters once per workgroup.
groupshared uint s_Counters[ST_SHARED_COUNTERS];

void Count(int counter)
{
	if (IsCounting())
		InterlockedAdd(s_Counters[counter], 1u);
}

void Emit(uint instance, uint bucket, uint lodRecord)
{
	uint dense = u_BucketTable[bucket];
	if (dense == ST_CULL_NONE)
	{
		// A bucket the CPU did not expect: never drawn (the counters say so).
		if (IsCounting())
			InterlockedAdd(u_Counters[ST_COUNTER_OVERFLOW], 1u);
		return;
	}
	uint key = dense * u_Cull.LODCapacity + lodRecord;
	if (IsCounting())
	{
		InterlockedAdd(u_Counts[key], 1u);
		return;
	}
	uint offset = u_Offsets[key];
	if (offset == ST_CULL_NONE)
		return; // No room in the draw list (counted by CullBuild)
	uint st_Atomic1; InterlockedAdd(u_Counts[key], 0xFFFFFFFFu, st_Atomic1);
	uint slot = st_Atomic1 - 1u;
	u_CullDrawList[offset + slot] = instance;
}

// Feedback for a mesh or material slot: its use bits, its largest sizes, and its place in the touched lists the first time
// a frame touches it. Read before the atomics: thousands of proxies of one mesh mostly find their use recorded already.
void MarkMeshFeedback(uint slot, uint use, float pixels)
{
	uint base = slot * 2u;
	if ((u_MeshFeedback[base + 1u] & use) != use)
	{
		uint st_Atomic2; InterlockedOr(u_MeshFeedback[base + 1u], use, st_Atomic2);
		uint previous = st_Atomic2;
		if (previous == 0u)
		{
			uint st_Atomic3; InterlockedAdd(u_FeedbackLists[0], 1u, st_Atomic3);
			u_FeedbackLists[ST_FEEDBACK_LIST_HEADER + st_Atomic3] = slot;
		}
	}
	uint bits = asuint(pixels);
	if (pixels > 0.0 && u_MeshFeedback[base] < bits)
		InterlockedMax(u_MeshFeedback[base], bits);
}

void MarkMaterialFeedback(uint slot, uint use, float pixels, float uvPixels)
{
	if (slot >= u_Cull.MaterialSlotCount)
		return;
	uint base = slot * 4u;
	if ((u_MaterialFeedback[base + 2u] & use) != use)
	{
		uint st_Atomic4; InterlockedOr(u_MaterialFeedback[base + 2u], use, st_Atomic4);
		uint previous = st_Atomic4;
		if (previous == 0u)
		{
			uint st_Atomic5; InterlockedAdd(u_FeedbackLists[1], 1u, st_Atomic5);
			u_FeedbackLists[ST_FEEDBACK_LIST_HEADER + u_Cull.MeshSlotCount + st_Atomic5] = slot;
		}
	}
	uint pixelBits = asuint(pixels);
	if (pixels > 0.0 && u_MaterialFeedback[base] < pixelBits)
		InterlockedMax(u_MaterialFeedback[base], pixelBits);
	uint uvBits = asuint(uvPixels);
	if (uvPixels > 0.0 && u_MaterialFeedback[base + 1u] < uvBits)
		InterlockedMax(u_MaterialFeedback[base + 1u], uvBits);
}

// The proxy's materials as feedback: its instances' (once the mesh's layout gave it instances), else its override material.
// Sizes only where the mesh is drawable (`sized`, with its geometry entry), as the CPU's screen uses count drawn instances
// only.
void MarkProxyMaterials(GPUProxy proxy, uint use, bool sized, float pixels, GeometryEntry entry)
{
	if (proxy.InstanceCount == 0u)
	{
		uint overrideSlot = proxy.Flags >> ST_PROXY_OVERRIDE_SHIFT;
		if (overrideSlot != 0u)
			MarkMaterialFeedback(overrideSlot - 1u, use, 0.0, 0.0);
		return;
	}
	// TextureStreamingPolicy::GetViewScale and GetWorldPixels, as GatherDraws works them out: once per proxy.
	bool orthographic = u_Cull.CameraPosition.w > 0.5;
	float unitPixels = orthographic ? u_Cull.ViewParameters.x / u_Cull.LODParameters.y : u_Cull.ViewParameters.x / (2.0 * u_Cull.LODParameters.y);
	float worldPixels = -1.0;
	float3 axisScale = float3(1.0);
	for (uint submesh = 0u; submesh < proxy.InstanceCount; submesh++)
	{
		CullInstance instance = u_CullInstances[proxy.FirstInstance + submesh];
		uint material = instance.MaterialIndex < u_Cull.MaterialSlotCount ? u_MaterialFlags[instance.MaterialIndex] : 0u;
		bool drawn = sized && (material & ST_CULL_MATERIAL_AVAILABLE) != 0u;
		float uvPixels = 0.0;
		if (drawn && (material & ST_CULL_MATERIAL_TEXTURED) != 0u)
		{
			if (worldPixels < 0.0)
			{
				worldPixels = GetWorldPixels(u_Cull.CameraPosition.xyz, orthographic, unitPixels, u_Cull.ViewParameters.y, proxy.WorldMin, proxy.WorldMax);
				CullInstance first = u_CullInstances[proxy.FirstInstance];
				axisScale = sqrt(first.WorldRows[0].xyz * first.WorldRows[0].xyz + first.WorldRows[1].xyz * first.WorldRows[1].xyz
					+ first.WorldRows[2].xyz * first.WorldRows[2].xyz);
			}
			uvPixels = GetUVPixels(worldPixels, u_GeometrySubmeshes[entry.FirstSubmesh + submesh].UVDensity.xyz, axisScale);
		}
		MarkMaterialFeedback(instance.MaterialIndex, use, drawn ? pixels : 0.0, uvPixels);
	}
}

// Whether everything in the screen rectangle of the (camera-relative) box lies in front of it in the first phase's depth (the depth pyramid's
// farthest depth there is nearer than the box's nearest point, reversed-Z). Boxes that reach the camera's plane are never.
bool IsOccluded(float3 boxMin, float3 boxMax)
{
	float2 size = u_Cull.OcclusionParameters.xy;
	float2 minPixel = float2(3.0e38);
	float2 maxPixel = float2(-3.0e38);
	float nearest = 0.0;
	for (uint corner = 0u; corner < 8u; corner++)
	{
		float3 position = float3((corner & 1u) != 0u ? boxMax.x : boxMin.x, (corner & 2u) != 0u ? boxMax.y : boxMin.y, (corner & 4u) != 0u ? boxMax.z : boxMin.z);
		float4 clip = mul(u_Cull.ViewProjection, float4(position, 1.0));
		if (clip.w <= 1.0e-6)
			return false;
		float3 ndc = clip.xyz / clip.w;
		if (ndc.z >= 1.0)
			return false;
		float2 pixel = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * size;
		minPixel = min(minPixel, pixel);
		maxPixel = max(maxPixel, pixel);
		nearest = max(nearest, ndc.z);
	}
	int2 a;
	int2 b;
	int level = GetPyramidFootprint(minPixel, maxPixel, size, int(u_Cull.OcclusionParameters.z), 1, a, b);
	float farthest = min(min(u_DepthPyramid.Load(int3(a, level)).r, u_DepthPyramid.Load(int3(int2(b.x, a.y), level)).r),
		min(u_DepthPyramid.Load(int3(int2(a.x, b.y), level)).r, u_DepthPyramid.Load(int3(b, level)).r));
	return nearest < farthest;
}

// The view's draws of a proxy's instances (opaque and alpha-masked: transparent ones are the CPU's) into the buckets of
// a phase (`phaseBit`: 0, or ST_CULL_SECOND_PHASE_BUCKET).
void EmitViewInstances(GPUProxy proxy, GeometryEntry entry, float fraction, uint geometry, uint mirrored, uint phaseBit)
{
	for (uint submesh = 0u; submesh < proxy.InstanceCount; submesh++)
	{
		uint instance = proxy.FirstInstance + submesh;
		uint materialSlot = u_CullInstances[instance].MaterialIndex;
		uint material = materialSlot < u_Cull.MaterialSlotCount ? u_MaterialFlags[materialSlot] : 0u;
		if ((material & ST_CULL_MATERIAL_AVAILABLE) == 0u || (material & ST_CULL_MATERIAL_TRANSPARENT) != 0u)
			continue;
		GeometrySubmesh geometrySubmesh = u_GeometrySubmeshes[entry.FirstSubmesh + submesh];
		uint lod = ChooseLOD(geometrySubmesh, fraction);
		uint doubleSided = (material & ST_CULL_MATERIAL_DOUBLE_SIDED) != 0u ? 2u : 0u;
		uint bucket = mirrored | doubleSided | (geometry << 2) | phaseBit;
		// A changed level cross-fades: both levels are drawn, with complementary masks. The writing pass keeps the state (an
		// instance is drawn in one phase only, so each frame reads the previous frame's).
		// Submeshes with one level have nothing to fade (nor any state), as on the CPU path.
		bool fades = u_Cull.LODFade != 0u && geometrySubmesh.LODCount > 1u;
		uint fadeCode = 0u;
		uint fadeFrom = lod;
		if (fades)
		{
			uint2 state = UpdateLODFade(u_LODFades[instance], lod, geometrySubmesh.LODCount, u_CullInstances[instance].EntityID, u_Cull.LODFadeFrame, true,
				fadeCode, fadeFrom);
			if (!IsCounting())
				u_LODFades[instance] = state;
		}
		Emit(instance | (fadeCode << ST_CULL_DRAW_ENTRY_INSTANCE_BITS), bucket, geometrySubmesh.FirstLOD + lod);
		Count(ST_COUNTER_VIEW_INSTANCES);
		if (fadeCode != 0u)
		{
			Emit(instance | ((fadeCode + ST_LOD_FADE_OUTGOING) << ST_CULL_DRAW_ENTRY_INSTANCE_BITS), bucket, geometrySubmesh.FirstLOD + fadeFrom);
			Count(ST_COUNTER_VIEW_INSTANCES);
		}
		Count(ST_COUNTER_INSTANCES_PER_LOD + int(min(lod, uint(ST_LOD_STAT_COUNT - 1))));
		if (geometry == ST_CULL_GEOMETRY_SKINNED)
			Count(ST_COUNTER_SKINNED_INSTANCES);
	}
}

#ifdef ST_CULL_SHADOW
// The cascades a caster is drawn into (their boxes, less those it spans less than a texel of, counted with `countSubTexel`), and
// its light view bounds relative to the camera, as the cascades' boxes are (AABB::Transformed of the light view times the
// world transform).
uint GetCasterCascades(GPUProxy proxy, bool countSubTexel, out float3 lightMin, out float3 lightMax)
{
	CullInstance first = u_CullInstances[proxy.FirstInstance];
	float4x4 world = transpose(transpose(float4x4(first.WorldRows[0], first.WorldRows[1], first.WorldRows[2], float4(0.0, 0.0, 0.0, 1.0))));
	world[0][3] -= u_Cull.CameraPosition.x;
	world[1][3] -= u_Cull.CameraPosition.y;
	world[2][3] -= u_Cull.CameraPosition.z;
	float4x4 lightWorld = mul(u_Cull.LightView, world);
	float3 localCenter = (proxy.LocalMin + proxy.LocalMax) * 0.5;
	float3 localExtents = (proxy.LocalMax - proxy.LocalMin) * 0.5;
	float3 lightCenter = (mul(lightWorld, float4(localCenter, 1.0))).xyz;
	float3x3 basis = ((float3x3)(lightWorld));
	float3 lightExtents = float3(abs(basis[0][0]) * localExtents.x + abs(basis[0][1]) * localExtents.y + abs(basis[0][2]) * localExtents.z,
		abs(basis[1][0]) * localExtents.x + abs(basis[1][1]) * localExtents.y + abs(basis[1][2]) * localExtents.z,
		abs(basis[2][0]) * localExtents.x + abs(basis[2][1]) * localExtents.y + abs(basis[2][2]) * localExtents.z);
	lightMin = lightCenter - lightExtents;
	lightMax = lightCenter + lightExtents;
	float extent = max(lightMax.x - lightMin.x, lightMax.y - lightMin.y);
	uint cascadeMask = 0u;
	for (uint cascade = 0u; cascade < u_Cull.CascadeCount; cascade++)
	{
		float4 box = u_Cull.CascadeBoxes[cascade];
		float4 parameters = u_Cull.CascadeParameters[cascade];
		if (lightMax.x < box.x || lightMin.x > box.z || lightMax.y < box.y || lightMin.y > box.w || lightMax.z < parameters.x)
			continue;
		if (extent < parameters.y)
		{
			if (countSubTexel)
				Count(ST_COUNTER_SUB_TEXEL_CASTERS);
			continue;
		}
		cascadeMask |= 1u << cascade;
	}
	return cascadeMask;
}

// The cascades whose visibility bits of a parity are set in a proxy's visibility word.
uint GetCascadeVisibility(uint state, uint parity)
{
	return (state >> (ST_VISIBILITY_CASCADE_SHIFT + ST_CULL_MAX_CASCADES * parity)) & ((1u << ST_CULL_MAX_CASCADES) - 1u);
}

// Records the cascades a proxy is visible in this frame (the bits of the parity after u_Cull.VisibilityParity).
void SetCascadeVisibility(uint proxyIndex, uint cascadeMask)
{
	uint shift = ST_VISIBILITY_CASCADE_SHIFT + ST_CULL_MAX_CASCADES * (u_Cull.VisibilityParity ^ 1u);
	uint currentBits = ((1u << ST_CULL_MAX_CASCADES) - 1u) << shift;
	u_Visibility[proxyIndex] = (u_Visibility[proxyIndex] & ~currentBits) | (cascadeMask << shift);
}

// Whether a caster's light view box lies, everywhere it covers the cascade's shadow map, behind the depth of the cascade's first
// phase (its pyramid's farthest depth there is nearer to the light than the box's nearest point, reversed-Z). The cascade's
// projection is orthographic: depth runs linearly from its far side (0) to its near plane (1), and casters reaching past the
// near plane, which ShadowMesh.vert flattens onto it, are never hidden.
bool IsOccludedInCascade(uint cascade, float3 lightMin, float3 lightMax)
{
	float farZ = u_Cull.CascadeParameters[cascade].x;
	float nearZ = u_CullCascades[cascade].Parameters.w;
	if (!(lightMax.z < nearZ) || !(nearZ > farZ))
		return false;
	float nearest = (lightMax.z - farZ) / (nearZ - farZ);
	// Texels as the rasterizer places them (top-left origin, light view +y up).
	float4 box = u_Cull.CascadeBoxes[cascade];
	float size = float(u_Cull.ShadowMapSize);
	float2 scale = float2(size) / max(box.zw - box.xy, float2(1.0e-20));
	float2 minPixel = float2(lightMin.x - box.x, box.w - lightMax.y) * scale;
	float2 maxPixel = float2(lightMax.x - box.x, box.w - lightMin.y) * scale;
	int2 a;
	int2 b;
	int level = GetPyramidFootprint(minPixel, maxPixel, float2(size), int(u_Cull.ShadowPyramidLevels), 2, a, b);
	int layer = int(cascade);
	float farthest = min(min(u_ShadowPyramid.Load(int4(int3(a, layer), level)).r, u_ShadowPyramid.Load(int4(int3(b.x, a.y, layer), level)).r),
		min(u_ShadowPyramid.Load(int4(int3(a.x, b.y, layer), level)).r, u_ShadowPyramid.Load(int4(int3(b, layer), level)).r));
	return nearest < farthest;
}

// A caster's draws into the cascades of `cascadeMask` (opaque and alpha-masked instances: transparent surfaces cast no
// shadows), in the buckets of a phase (`phaseBit`: 0, or ST_CULL_SHADOW_SECOND_PHASE_BUCKET).
void EmitShadowInstances(GPUProxy proxy, GeometryEntry entry, float fraction, uint geometry, uint mirrored, uint cascadeMask, uint phaseBit)
{
	if (cascadeMask == 0u)
		return;
	for (uint submesh = 0u; submesh < proxy.InstanceCount; submesh++)
	{
		uint instance = proxy.FirstInstance + submesh;
		uint materialSlot = u_CullInstances[instance].MaterialIndex;
		uint material = materialSlot < u_Cull.MaterialSlotCount ? u_MaterialFlags[materialSlot] : 0u;
		if ((material & ST_CULL_MATERIAL_AVAILABLE) == 0u || (material & ST_CULL_MATERIAL_TRANSPARENT) != 0u)
			continue;
		GeometrySubmesh geometrySubmesh = u_GeometrySubmeshes[entry.FirstSubmesh + submesh];
		uint lod = ChooseLOD(geometrySubmesh, fraction);
		uint bucket = mirrored | ((material & ST_CULL_MATERIAL_DOUBLE_SIDED) != 0u ? 2u : 0u) | (geometry << 2)
			| ((material & ST_CULL_MATERIAL_ALPHA_MASK) != 0u ? ST_CULL_SHADOW_ALPHA_MASK_BUCKET : 0u) | phaseBit;
		for (uint cascade = 0u; cascade < u_Cull.CascadeCount; cascade++)
		{
			if ((cascadeMask & (1u << cascade)) == 0u)
				continue;
			Emit(instance, ST_CULL_VIEW_BUCKETS + cascade * ST_CULL_SHADOW_BUCKETS + bucket, geometrySubmesh.FirstLOD + lod);
			Count(ST_COUNTER_SHADOW_INSTANCES);
		}
	}
}

// A caster's draws into the local shadow view `view` of the batch, its levels chosen as for a viewport of the view's tile
// (`errorThreshold`).
void EmitLocalShadowInstances(GPUProxy proxy, GeometryEntry entry, float fraction, float errorThreshold, uint geometry, uint mirrored, uint view)
{
	for (uint submesh = 0u; submesh < proxy.InstanceCount; submesh++)
	{
		uint instance = proxy.FirstInstance + submesh;
		uint materialSlot = u_CullInstances[instance].MaterialIndex;
		uint material = materialSlot < u_Cull.MaterialSlotCount ? u_MaterialFlags[materialSlot] : 0u;
		if ((material & ST_CULL_MATERIAL_AVAILABLE) == 0u || (material & ST_CULL_MATERIAL_TRANSPARENT) != 0u)
			continue;
		GeometrySubmesh geometrySubmesh = u_GeometrySubmeshes[entry.FirstSubmesh + submesh];
		uint lod = ChooseLODFor(geometrySubmesh, fraction, errorThreshold);
		uint bucket = mirrored | ((material & ST_CULL_MATERIAL_DOUBLE_SIDED) != 0u ? 2u : 0u) | (geometry << 2)
			| ((material & ST_CULL_MATERIAL_ALPHA_MASK) != 0u ? ST_CULL_SHADOW_ALPHA_MASK_BUCKET : 0u);
		Emit(instance, ST_CULL_FIRST_LOCAL_BUCKET + view * ST_CULL_LOCAL_BUCKETS + bucket, geometrySubmesh.FirstLOD + lod);
		Count(ST_COUNTER_LOCAL_SHADOW_INSTANCES);
	}
}

// The local shadow views of the pass's batch a caster is drawn into, as SceneRenderer::FindLocalShadowCasters decides on the CPU
// path: its box reaches the light's range and lies in the view's frustum (camera-relative), and spans at least a texel of the
// view's tile (casters marked for feedback like the cascades', whether their meshes are loaded or not).
void CullLocalShadowViews(GPUProxy proxy, bool meshDrawable, GeometryEntry entry, uint geometry, uint mirrored, bool feedback)
{
	uint first = (u_Parameters.Mode >> 8) & 0xFFu;
	uint count = (u_Parameters.Mode >> 16) & 0xFFu;
	if (count == 0u)
		return;
	float3 relativeMin = proxy.WorldMin - u_Cull.CameraPosition.xyz;
	float3 relativeMax = proxy.WorldMax - u_Cull.CameraPosition.xyz;
	float3 center = (proxy.WorldMin + proxy.WorldMax) * 0.5;
	float radius = length((proxy.WorldMax - proxy.WorldMin) * 0.5);
	bool marked = false;
	for (uint index = 0u; index < count; index++)
	{
		CullLocalShadowView view = u_LocalShadowViews[first + index];
		float3 offset = clamp(view.RelativeLight.xyz, relativeMin, relativeMax) - view.RelativeLight.xyz;
		if (dot(offset, offset) > view.Light.w * view.Light.w || !IsBoxInPlanes(relativeMin, relativeMax, view.Planes))
			continue;
		float fraction = radius / (max(length(center - view.Light.xyz), view.RelativeLight.w) * view.Parameters.x);
		if (fraction * view.Parameters.y < 1.0)
		{
			Count(ST_COUNTER_SUB_TEXEL_CASTERS);
			continue;
		}
		if (feedback && !marked)
		{
			MarkMeshFeedback(proxy.MeshSlot, ST_FEEDBACK_SHADOW, 0.0);
			MarkProxyMaterials(proxy, ST_FEEDBACK_SHADOW, false, 0.0, entry);
			marked = true;
		}
		if (meshDrawable)
			EmitLocalShadowInstances(proxy, entry, fraction, view.Parameters.z, geometry, mirrored, index);
	}
}

// The cascades' part of a caster's culling (the first phase, or every cascade without occlusion culling).
void CullCascades(uint proxyIndex, GPUProxy proxy, bool meshDrawable, GeometryEntry entry, float fraction, uint geometry, uint mirrored, bool feedback)
{
	float3 lightMin;
	float3 lightMax;
	uint cascadeMask = GetCasterCascades(proxy, true, lightMin, lightMax);
	if (cascadeMask == 0u)
		return;
	if (feedback)
	{
		// Casters are kept resident, not requested (as FindShadowCasters marks them).
		MarkMeshFeedback(proxy.MeshSlot, ST_FEEDBACK_SHADOW, 0.0);
		MarkProxyMaterials(proxy, ST_FEEDBACK_SHADOW, false, 0.0, entry);
	}
	if (!meshDrawable)
		return;
	if (IsCounting())
	{
		// Every drawn caster extends the depth range of its cascades toward the light (Scene/CascadeFit.comp), in both phases'
		// cascades: the projections are known before the first phase draws.
		uint nearZ = EncodeOrderedFloat(lightMax.z);
		for (uint cascade = 0u; cascade < u_Cull.CascadeCount; cascade++)
		{
			if ((cascadeMask & (1u << cascade)) != 0u && u_CasterDepths[cascade] < nearZ)
				InterlockedMax(u_CasterDepths[cascade], nearZ);
		}
	}
	// Occlusion culled: the first phase draws into each cascade what it held in the previous frame; the second the other
	// casters it finds visible against the first phase's depth (CullShadowSecondPhase). Frames that draw the cascades whole
	// record what they drew as the next frame's history (the culling pauses while it does not pay). Cascades that keep their
	// layer from the previous frame get no draws.
	if (u_Cull.ShadowOcclusion != 0u)
		cascadeMask &= GetCascadeVisibility(u_Visibility[proxyIndex], u_Cull.VisibilityParity);
	else if (IsCounting())
		SetCascadeVisibility(proxyIndex, cascadeMask);
	EmitShadowInstances(proxy, entry, fraction, geometry, mirrored, cascadeMask & ~u_Cull.CachedCascades, 0u);
}
#endif

// Whether the mesh table holds a drawable mesh for the proxy: loaded, with the layout its instances were made for.
bool IsDrawable(GPUProxy proxy, out uint mesh, out GeometryEntry entry)
{
	mesh = u_MeshTable[proxy.MeshSlot];
	entry = GeometryEntry(0u, 0u, 0u, 0u);
	if (mesh == 0u || proxy.InstanceCount == 0u)
		return false; // Not drawn this frame: not loaded, or not uploaded yet
	entry = u_GeometryEntries[mesh & ~ST_CULL_MESH_SKINNED_ARENA];
	return entry.SubmeshCount == proxy.InstanceCount; // Else instances of another layout of the mesh
}

void CullProxy(uint proxyIndex)
{
	GPUProxy proxy = u_Proxies[proxyIndex];
	const uint drawable = ST_PROXY_ACTIVE | ST_PROXY_INDEXED;
	if ((proxy.Flags & drawable) != drawable || proxy.MeshSlot >= u_Cull.MeshSlotCount)
		return;
	bool feedback = IsCounting() && u_Cull.Feedback != 0u;
	uint mesh;
	GeometryEntry entry;
	bool meshDrawable = IsDrawable(proxy, mesh, entry);
	bool skinnedArena = (mesh & ST_CULL_MESH_SKINNED_ARENA) != 0u;
	uint geometry = skinnedArena ? ((proxy.Flags & ST_PROXY_SKINNED) != 0u ? ST_CULL_GEOMETRY_SKINNED : ST_CULL_GEOMETRY_SKINNED_ARENA) : ST_CULL_GEOMETRY_STATIC;
	uint mirrored = (proxy.Flags & ST_PROXY_MIRRORED) != 0u ? 1u : 0u;
	float3 center = (proxy.WorldMin + proxy.WorldMax) * 0.5;
	float3 extents = (proxy.WorldMax - proxy.WorldMin) * 0.5;
	float fraction = GetScreenFraction(center, length(extents));

#ifndef ST_CULL_SHADOW
	// Within the prefetch distance (SpatialIndex::QuerySphere's test): requested at low priority, in view or not.
	float prefetch = u_Cull.ViewParameters.z;
	if (feedback && prefetch > 0.0)
	{
		float3 outside = max(max(proxy.WorldMin - u_Cull.CameraPosition.xyz, u_Cull.CameraPosition.xyz - proxy.WorldMax), float3(0.0));
		if (dot(outside, outside) <= prefetch * prefetch)
		{
			MarkMeshFeedback(proxy.MeshSlot, ST_FEEDBACK_PREFETCH, 0.0);
			MarkProxyMaterials(proxy, ST_FEEDBACK_PREFETCH, false, 0.0, entry);
		}
	}
	// Camera-relative: the frustum's planes and the box, so precision does not depend on the distance from the origin.
	float3 relativeMin = proxy.WorldMin - u_Cull.CameraPosition.xyz;
	float3 relativeMax = proxy.WorldMax - u_Cull.CameraPosition.xyz;
	if (!IsBoxInFrustum(relativeMin, relativeMax))
		return;
	float pixels = fraction * u_Cull.ViewParameters.x;
	if (u_Cull.LODParameters.w > 0.0 && pixels < u_Cull.LODParameters.w)
	{
		// Counted as the CPU path counts them: among the proxies it could draw.
		if (meshDrawable)
			Count(ST_COUNTER_SIZE_CULLED);
		return;
	}
	if (feedback)
	{
		uint use = ST_FEEDBACK_VIEW | (fraction > u_Cull.ViewParameters.w ? ST_FEEDBACK_HIGH : 0u);
		MarkMeshFeedback(proxy.MeshSlot, use, pixels);
		MarkProxyMaterials(proxy, use, meshDrawable, pixels, entry);
	}
	if (!meshDrawable)
		return;
	Count(ST_COUNTER_VISIBLE_PROXIES);
	// Occlusion culled: the first phase draws what the view saw in the previous frame; the second the rest it finds
	// visible against the first phase's depth (CullSecondPhase).
	if (u_Cull.OcclusionParameters.w > 0.5 && (u_Visibility[proxyIndex] & (1u << u_Cull.VisibilityParity)) == 0u)
		return;
	EmitViewInstances(proxy, entry, fraction, geometry, mirrored, 0u);
#else
	if ((proxy.Flags & ST_PROXY_CAST_SHADOWS) == 0u || (proxy.Flags & ST_PROXY_MESH_BOUNDS) == 0u || proxy.InstanceCount == 0u)
		return;
	if ((u_Parameters.Mode & ST_CULL_MODE_LOCAL) == 0u)
		CullCascades(proxyIndex, proxy, meshDrawable, entry, fraction, geometry, mirrored, feedback);
	CullLocalShadowViews(proxy, meshDrawable, entry, geometry, mirrored, feedback);
#endif
}

#ifndef ST_CULL_SHADOW
// The view's second phase: every proxy's visibility this frame (the frustum, size and occlusion tests; written while
// counting), and the draws of those visible now that the first phase did not draw.
void CullSecondPhase(uint proxyIndex)
{
	GPUProxy proxy = u_Proxies[proxyIndex];
	uint previousBit = 1u << u_Cull.VisibilityParity;
	uint currentBit = previousBit ^ 3u;
	bool visible = false;
	uint mesh = 0u;
	GeometryEntry entry = GeometryEntry(0u, 0u, 0u, 0u);
	const uint drawable = ST_PROXY_ACTIVE | ST_PROXY_INDEXED;
	bool tested = (proxy.Flags & drawable) == drawable && proxy.MeshSlot < u_Cull.MeshSlotCount && IsDrawable(proxy, mesh, entry);
	float3 center = (proxy.WorldMin + proxy.WorldMax) * 0.5;
	float3 extents = (proxy.WorldMax - proxy.WorldMin) * 0.5;
	float fraction = GetScreenFraction(center, length(extents));
	float3 relativeMin = proxy.WorldMin - u_Cull.CameraPosition.xyz;
	float3 relativeMax = proxy.WorldMax - u_Cull.CameraPosition.xyz;
	if (tested && IsBoxInFrustum(relativeMin, relativeMax)
		&& !(u_Cull.LODParameters.w > 0.0 && fraction * u_Cull.ViewParameters.x < u_Cull.LODParameters.w))
	{
		visible = !IsOccluded(relativeMin, relativeMax);
		if (!visible)
			Count(ST_COUNTER_OCCLUDED);
	}
	uint state = u_Visibility[proxyIndex];
	if (IsCounting())
		u_Visibility[proxyIndex] = (state & ~currentBit) | (visible ? currentBit : 0u);
	if (!visible || (state & previousBit) != 0u)
		return;
	bool skinnedArena = (mesh & ST_CULL_MESH_SKINNED_ARENA) != 0u;
	uint geometry = skinnedArena ? ((proxy.Flags & ST_PROXY_SKINNED) != 0u ? ST_CULL_GEOMETRY_SKINNED : ST_CULL_GEOMETRY_SKINNED_ARENA) : ST_CULL_GEOMETRY_STATIC;
	uint mirrored = (proxy.Flags & ST_PROXY_MIRRORED) != 0u ? 1u : 0u;
	EmitViewInstances(proxy, entry, fraction, geometry, mirrored, ST_CULL_SECOND_PHASE_BUCKET);
}
#else
// The cascades' second phase: per cascade whether the proxy casts into it visibly this frame (its box and sub-texel tests and
// the occlusion test against the cascade's pyramid; written while counting), and the draws into the cascades where it is
// visible now that the first phase did not draw it into.
void CullShadowSecondPhase(uint proxyIndex)
{
	GPUProxy proxy = u_Proxies[proxyIndex];
	uint visibleMask = 0u;
	uint mesh = 0u;
	GeometryEntry entry = GeometryEntry(0u, 0u, 0u, 0u);
	const uint drawable = ST_PROXY_ACTIVE | ST_PROXY_INDEXED | ST_PROXY_CAST_SHADOWS | ST_PROXY_MESH_BOUNDS;
	bool tested = (proxy.Flags & drawable) == drawable && proxy.MeshSlot < u_Cull.MeshSlotCount && IsDrawable(proxy, mesh, entry);
	if (tested)
	{
		float3 lightMin;
		float3 lightMax;
		uint cascadeMask = GetCasterCascades(proxy, false, lightMin, lightMax);
		for (uint cascade = 0u; cascade < u_Cull.CascadeCount; cascade++)
		{
			if ((cascadeMask & (1u << cascade)) == 0u)
				continue;
			if (IsOccludedInCascade(cascade, lightMin, lightMax))
				Count(ST_COUNTER_OCCLUDED_CASTERS);
			else
				visibleMask |= 1u << cascade;
		}
	}
	uint previousMask = GetCascadeVisibility(u_Visibility[proxyIndex], u_Cull.VisibilityParity);
	if (IsCounting())
		SetCascadeVisibility(proxyIndex, visibleMask);
	uint drawMask = visibleMask & ~previousMask & ~u_Cull.CachedCascades;
	if (drawMask == 0u)
		return;
	float3 center = (proxy.WorldMin + proxy.WorldMax) * 0.5;
	float3 extents = (proxy.WorldMax - proxy.WorldMin) * 0.5;
	float fraction = GetScreenFraction(center, length(extents));
	bool skinnedArena = (mesh & ST_CULL_MESH_SKINNED_ARENA) != 0u;
	uint geometry = skinnedArena ? ((proxy.Flags & ST_PROXY_SKINNED) != 0u ? ST_CULL_GEOMETRY_SKINNED : ST_CULL_GEOMETRY_SKINNED_ARENA) : ST_CULL_GEOMETRY_STATIC;
	uint mirrored = (proxy.Flags & ST_PROXY_MIRRORED) != 0u ? 1u : 0u;
	EmitShadowInstances(proxy, entry, fraction, geometry, mirrored, drawMask, ST_CULL_SHADOW_SECOND_PHASE_BUCKET);
}
#endif

void st_Main()
{
	uint lane = st_LocalInvocationIndex;
	if (lane < uint(ST_SHARED_COUNTERS))
		s_Counters[lane] = 0u;
	GroupMemoryBarrierWithGroupSync();

	uint proxyIndex = st_GlobalInvocationID.x;
	if (proxyIndex < u_Cull.ProxyCount)
	{
		if ((u_Parameters.Mode & ST_CULL_MODE_SECOND_PHASE) != 0u)
		{
#ifndef ST_CULL_SHADOW
			CullSecondPhase(proxyIndex);
#else
			CullShadowSecondPhase(proxyIndex);
#endif
		}
		else
			CullProxy(proxyIndex);
	}

	GroupMemoryBarrierWithGroupSync();
	if (IsCounting() && lane < uint(ST_SHARED_COUNTERS) && s_Counters[lane] != 0u)
		InterlockedAdd(u_Counters[lane], s_Counters[lane]);
}

// --- Entry point ---
[numthreads(64, 1, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
