#ifndef STRATA_SCENE_SLANG
#define STRATA_SCENE_SLANG

// Resources shared by the scene passes. Layouts mirror the structs in Strata/Renderer/SceneRenderData.h; keep them
// in sync (std140 for the constant buffer, std430 for storage buffers, 16-byte aligned members).

// The scene layout's slots (SceneRenderer::CreateResources): SRV 0-21, 27, 28, 29 (the aerial perspective volume), 32 and 33,
// samplers 0-5 (materials), 8, 9 and 10, CBV 0.
// SRV 22-26 belong to the compute layouts of temporal anti-aliasing and screen-space reflections, never to this one.
// Assigned in wave 7: SRV 28 the sun's screen-space shadow mask, 32 the cascades' depth ranges and 33 the shadows'
// blue-noise tile; 29 the aerial perspective volume. Assigned for wave 8: 30 and 31 the irradiance volume (radiance-cascades,
// below its cut), 34 volumetric fog (volumetric), 35 the radiance cascades' indirect light (radiance-cascades), 36 fog
// volumes (volumetric, below its cut). A new scene resource takes the next free slot (37), and a pass that does not run
// binds a dummy.

#include "Include/Common.slang"

#define ST_MATERIAL_ALPHA_MASK   1u
#define ST_MATERIAL_ALPHA_BLEND  2u
#define ST_MATERIAL_UNLIT        4u
#define ST_MATERIAL_DOUBLE_SIDED 8u
#define ST_MATERIAL_CLEARCOAT    16u
#define ST_MATERIAL_TRANSMISSIVE 32u

#define ST_INSTANCE_MIRRORED 1u // Negative determinant: winding and tangent handedness are flipped
#define ST_INSTANCE_SKINNED  2u // Posed: SkinPalette is the first joint matrix of its palette (the skinned pipelines)

#define ST_LIGHT_POINT 0u
#define ST_LIGHT_SPOT  1u

// The lighting debug views (SceneLightingView, MaterialParams.y): what the forward pass writes instead of the lit color.
#define ST_LIGHTING_VIEW_NONE              0u
#define ST_LIGHTING_VIEW_LIGHTING          1u // White albedo, no emission
#define ST_LIGHTING_VIEW_AMBIENT_OCCLUSION 2u // The occlusion of indirect light (material x screen), unexposed
#define ST_LIGHTING_VIEW_INDIRECT          3u // Indirect light only
#define ST_LIGHTING_VIEW_LUMINANCE         4u // The lit image (the overlay bands it)
#define ST_LIGHTING_VIEW_ALBEDO            5u // The base color, unexposed

#define ST_SAMPLER_COUNT 6

// Texture table entries (u_TextureTable): bindless slot in bits 0-23, sampler index in bits 24-30, and this bit for
// one-channel textures that stand for gray (sampled as (r, r, r, 1)).
#define ST_TEXTURE_SLOT_GRAYSCALE 0x80000000u

// Scene shaders work in camera-relative world space: world space with the camera at the origin (TransformPositionRelative),
// so what is near the camera keeps full precision however far from the world's origin it is. View, ViewProjection and their
// inverses are rotation-only (the view's translation is zero); CameraPosition is the camera's world position.
struct FrameConstants
{
	float4x4 View;
	float4x4 Projection;
	float4x4 ViewProjection;
	float4x4 InverseView;
	float4x4 InverseProjection;
	float4x4 InverseViewProjection;
	// What motion vectors measure positions with: this frame's view-projection without a temporal jitter, and the renderer's
	// previous frame's, which takes positions relative to this frame's camera to that frame's clip space.
	float4x4 UnjitteredViewProjection;
	float4x4 PreviousViewProjection;
	float4 CameraPosition;            // xyz: world position, w: near clip distance
	float4 CameraForward;             // xyz: world view direction, w: 1 for orthographic projections
	float4 ViewportSize;             // xy: size in pixels, zw: 1 / size
	float4 DirectionalLightDirection; // xyz: direction the light travels (normalized), w: 1 when the light exists
	float4 DirectionalLightColor;     // rgb: color * intensity
	float4 AmbientColor;              // rgb: ambient radiance used without an environment map
	float4 EnvironmentParams;         // x: intensity, y: rotation (radians), z: specular mip count, w: 1 when enabled
	uint4 LightCounts;              // x: punctual lights in the light buffer
	uint4 ClusterGrid;              // xyz: light clusters along screen x, screen y and depth, w: lights per cluster
	float4 ClusterDepth;              // Depth slice = log(view depth) * x + y; z, w: near and far view depth
	float4 TimeParams;                // x: scene time in seconds
	float4 SkyParams;                 // x: background mip (blur), y: 1 when the environment is the background
	float4 CascadeSplits;             // View distance where each cascade ends (their projections: u_Cascades)
	// x: tan(light angular radius), the penumbra's half-width per unit of separation, y: 1 / shadow map size, z: the fraction of
	// each cascade's depth range over which it blends into the next (DirectionalLightComponent::CascadeBlend), w: cascade count
	// (0 = none)
	float4 ShadowParams;
	// x: depth bias (texels), y: normal offset (texels), z: the wide penumbrae's taps (RendererQualitySettings::
	// ShadowFilterQuality), w: unused (0)
	float4 ShadowBias;
	// x: 1 when screen-space ambient occlusion is available; y: 1 when occlusion's multi-bounce applies (AOMultiBounce)
	float4 AOParams;
	// x: 1 when raised texture levels fade in (SceneRendererSpecification::FadeTextureLevels); y: the lighting debug view
	// (ST_LIGHTING_VIEW_*, 0: none); z: specular anti-aliasing's strength (WidenRoughness; 1 with
	// SceneRendererSpecification::SpecularAntiAliasing, else 0); w: 1 in a reflection probe's capture (the lights' specular
	// left out, SceneRenderOptions::ProbeCapture), else 0
	float4 MaterialParams;
	// x: 1 when u_PreviousInstances and u_PreviousJoints hold the renderer's previous frame; y: the pre-exposure, a power of two
	// every writer of the HDR target multiplies its luminance by (SceneRenderer: values near 100,000 lux stay within half
	// floats); z: the frame's rotation of per-pixel noise (local shadows' PCF, the checkerboard screen-space reflections) in
	// turns, 0 without temporal anti-aliasing
	float4 TemporalParams;
	// Temporal anti-aliasing's offset of the frame's samples in clip units (in Projection, ViewProjection and their inverses; 0
	// without it): xy this frame's, zw the renderer's previous frame's
	float4 JitterParams;
	// Height fog (HeightFogComponent, Include/Fog.slang; FogParams.x is 0 without one):
	float4 FogDensity; // x: density at the base height (per meter), y: height falloff, z: the camera's height above the base, w: start distance
	float4 FogColor;   // rgb: the scattering's albedo, w: maximum opacity
	// rgb: light the fog scatters besides the sun's (the procedural sky's horizon, the ambient color, or 1 when ColorFromSky
	// is off), w: 1 when an environment map's irradiance from above lights it instead, 2 when the atmosphere's sky at the
	// horizon does (toward the ray's azimuth)
	float4 FogLight;
	// x: 1 when the scene has fog, y: the asymmetry of its scattering of the directional light; z: aerial perspective's far
	// distance in meters (0: none; Include/AerialPerspective.slang), w: its distance scale (light-and-materials claims z and w)
	float4 FogParams;
	// The atmosphere's sun disk: xyz toward the sun, w its angular radius (0: none); rgb its illuminance at the ground (lux)
	float4 SunDiskDirection;
	float4 SunDiskColor;
	// x: 1 when the frame traced screen-space reflections (Forward.frag's u_ScreenReflections, SRV 18); y: 1 when the frame's
	// indirect light comes from the radiance cascades (SRV 35), z: their intensity (radiance-cascades); w: the alpha^2 the
	// directional light's disk adds to its highlight's lobe (RenderData::GetSunLobeAlpha2; Include/PBR.slang).
	float4 ReflectionParams;
};

ConstantBuffer<FrameConstants> u_Frame : register(b0);

struct InstanceData
{
	float4 WorldRows[3]; // The upper three rows of the affine world matrix (the fourth is 0, 0, 0, 1)
	uint MaterialIndex;
	uint EntityID;     // Entity identifier + 1 (0 = no entity)
	uint Flags;        // ST_INSTANCE_*
	uint SkinPalette;  // ST_INSTANCE_SKINNED: the first joint matrix of the instance's palette in u_Joints
};

// Every instance of the render scene; it persists between frames.
StructuredBuffer<InstanceData> u_Instances : register(t0);

// The frame's draw list: an entry per drawn instance, in batch order (st_InstanceIndex, SV_VulkanInstanceID with the base instance, indexes it): the instance in the low
// ST_DRAW_ENTRY_INSTANCE_BITS bits, its level-of-detail cross-fade code above them (RenderData::MakeDrawEntry).
StructuredBuffer<uint> u_DrawList : register(t14);

#ifdef ST_VELOCITY
// The motion history (SceneRenderOptions::MotionVectors): every instance as the renderer's previous frame drew it, valid when
// TemporalParams.x is 1.
StructuredBuffer<InstanceData> u_PreviousInstances : register(t20);
#endif

// The motion from a previous clip position to the current one, in UV units (current minus previous).
float2 GetClipMotion(float4 currentClip, float4 previousClip)
{
	return NDCToUV(currentClip.xy / currentClip.w) - NDCToUV(previousClip.xy / previousClip.w);
}

#define ST_DRAW_ENTRY_INSTANCE_BITS 27u          // RenderData::c_DrawEntryInstanceBits
#define ST_DRAW_ENTRY_INSTANCE_MASK 0x07FFFFFFu
#define ST_LOD_FADE_OUTGOING_CODE   16u          // RenderData::c_LODFadeOutgoing

// Whether a draw with this cross-fade code covers the pixel (LODSelection::IsLODFadeDrawn): the level fading in (codes 1-15)
// where the 4x4 ordered dither threshold is below the code, the level fading out (the code plus 16) on the other pixels.
// The depth prepass and the forward pass test the same, so the EQUAL depth test holds.
bool IsLODFadeDrawn(uint code, float2 fragCoord)
{
	if (code == 0u)
		return true;
	const uint bayer[16] = { 0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u };
	uint2 pixel = uint2(fragCoord) & 3u;
	uint threshold = bayer[pixel.y * 4u + pixel.x];
	return code < ST_LOD_FADE_OUTGOING_CODE ? threshold < code : threshold >= code - ST_LOD_FADE_OUTGOING_CODE;
}

#ifdef ST_SKINNED
// The palettes of the posed instances (RenderScene::GetPalette): per joint of a skin the upper three rows of its affine
// skinning matrix, which takes a bind-pose vertex into the instance's local space.
struct JointMatrix
{
	float4 Rows[3];
};

StructuredBuffer<JointMatrix> u_Joints : register(t15);

// The skinning matrix of a vertex: its influences' palette entries blended by weight (linear blend skinning).
JointMatrix BlendJoints(InstanceData instance, uint4 joints, float4 weights)
{
	JointMatrix blended;
	blended.Rows[0] = float4(0.0);
	blended.Rows[1] = float4(0.0);
	blended.Rows[2] = float4(0.0);
	for (int influence = 0; influence < 4; influence++)
	{
		JointMatrix joint = u_Joints[instance.SkinPalette + joints[influence]];
		blended.Rows[0] += joint.Rows[0] * weights[influence];
		blended.Rows[1] += joint.Rows[1] * weights[influence];
		blended.Rows[2] += joint.Rows[2] * weights[influence];
	}
	return blended;
}

#ifdef ST_VELOCITY
// The palettes as the renderer's previous frame drew them (the motion history, like u_PreviousInstances).
StructuredBuffer<JointMatrix> u_PreviousJoints : register(t21);

JointMatrix BlendPreviousJoints(InstanceData instance, uint4 joints, float4 weights)
{
	JointMatrix blended;
	blended.Rows[0] = float4(0.0);
	blended.Rows[1] = float4(0.0);
	blended.Rows[2] = float4(0.0);
	for (int influence = 0; influence < 4; influence++)
	{
		JointMatrix joint = u_PreviousJoints[instance.SkinPalette + joints[influence]];
		blended.Rows[0] += joint.Rows[0] * weights[influence];
		blended.Rows[1] += joint.Rows[1] * weights[influence];
		blended.Rows[2] += joint.Rows[2] * weights[influence];
	}
	return blended;
}
#endif

float3 SkinPosition(JointMatrix skin, float3 position)
{
	float4 point = float4(position, 1.0);
	return float3(dot(skin.Rows[0], point), dot(skin.Rows[1], point), dot(skin.Rows[2], point));
}

float3 SkinDirection(JointMatrix skin, float3 direction)
{
	return float3(dot(skin.Rows[0].xyz, direction), dot(skin.Rows[1].xyz, direction), dot(skin.Rows[2].xyz, direction));
}

// A normal through the skinning matrix: its cofactor matrix, like TransformNormal (callers normalize).
float3 SkinNormal(JointMatrix skin, float3 normal)
{
	float3 column0 = float3(skin.Rows[0].x, skin.Rows[1].x, skin.Rows[2].x);
	float3 column1 = float3(skin.Rows[0].y, skin.Rows[1].y, skin.Rows[2].y);
	float3 column2 = float3(skin.Rows[0].z, skin.Rows[1].z, skin.Rows[2].z);
	float3 transformed = cross(column1, column2) * normal.x + cross(column2, column0) * normal.y + cross(column0, column1) * normal.z;
	return dot(column0, cross(column1, column2)) < 0.0 ? -transformed : transformed;
}
#endif

// A point of the instance in camera-relative world space: the instance's translation minus the camera's position is taken
// before anything is added to it, so vertices near the camera keep full precision far from the world's origin.
float3 TransformPositionRelative(InstanceData instance, float3 position)
{
	float3 translation = float3(instance.WorldRows[0].w, instance.WorldRows[1].w, instance.WorldRows[2].w) - u_Frame.CameraPosition.xyz;
	return float3(dot(instance.WorldRows[0].xyz, position), dot(instance.WorldRows[1].xyz, position), dot(instance.WorldRows[2].xyz, position)) + translation;
}

float3 TransformDirection(InstanceData instance, float3 direction)
{
	return float3(dot(instance.WorldRows[0].xyz, direction), dot(instance.WorldRows[1].xyz, direction), dot(instance.WorldRows[2].xyz, direction));
}

// A normal transformed by the inverse transpose of the world matrix, up to scale: the cofactor matrix of the upper 3x3
// (its columns are the cross products of the matrix's columns), negated for mirroring transforms, whose determinant is
// negative. Callers normalize.
float3 TransformNormal(InstanceData instance, float3 normal)
{
	float3 column0 = float3(instance.WorldRows[0].x, instance.WorldRows[1].x, instance.WorldRows[2].x);
	float3 column1 = float3(instance.WorldRows[0].y, instance.WorldRows[1].y, instance.WorldRows[2].y);
	float3 column2 = float3(instance.WorldRows[0].z, instance.WorldRows[1].z, instance.WorldRows[2].z);
	float3 transformed = cross(column1, column2) * normal.x + cross(column2, column0) * normal.y + cross(column0, column1) * normal.z;
	return (instance.Flags & ST_INSTANCE_MIRRORED) != 0u ? -transformed : transformed;
}

struct MaterialData
{
	float4 BaseColor;
	float4 Emissive;           // rgb: color * intensity
	float Metallic;
	float Roughness;
	float NormalScale;
	float OcclusionStrength;
	float AlphaCutoff;
	uint Flags;
	uint BaseColorMap;       // Texture table indices (u_TextureTable) from here on
	uint MetallicRoughnessMap;
	float4 UVTransform;        // xy: tiling, zw: offset
	uint NormalMap;
	uint OcclusionMap;
	uint EmissiveMap;
	uint ClearcoatMap;
	float Clearcoat;         // 0 without a coat (ST_MATERIAL_CLEARCOAT)
	float ClearcoatRoughness;
	float Transmission;      // 0 when not transmissive (ST_MATERIAL_TRANSMISSIVE)
	float IOR;
	float Thickness;         // In the mesh's units
	uint ClearcoatRoughnessMap;
	uint ClearcoatNormalMap;
	uint TransmissionMap;
	float4 Absorption;         // rgb: absorbed per world unit inside a transmissive volume (Beer's law), w: unused
};

StructuredBuffer<MaterialData> u_Materials : register(t1);

struct LightData
{
	float4 PositionRange;  // xyz: camera-relative world position, w: range
	float4 Color;          // rgb: color * intensity, w: 1 / range^2
	float4 DirectionType;  // xyz: direction the light travels, w: type (ST_LIGHT_*)
	float4 SpotAngles;     // x: cos(inner cone), y: cos(outer cone), z: its shadow slot plus one (0: no shadow; Include/LocalShadows.slang)
};

StructuredBuffer<LightData> u_Lights : register(t2);

// Samplers indexed by TextureFilter * 3 + TextureWrap (Linear/Nearest x Repeat/Clamp/Mirror).
SamplerState u_Samplers[ST_SAMPLER_COUNT] : register(s0);

// Image-based lighting (valid when EnvironmentParams.w > 0).
TextureCube<float4> u_IrradianceMap : register(t3);
TextureCube<float4> u_PrefilteredMap : register(t4);
Texture2D<float4> u_BRDFLut : register(t5);
TextureCube<float4> u_EnvironmentMap : register(t6);
SamplerState u_LinearClampSampler : register(s8);
// The atmosphere's sky seen from the viewer, per lux of sun (SkyLightSource::Atmosphere; Include/Atmosphere.slang maps it;
// valid while the sky uses the atmosphere, black otherwise). Sampled with u_Samplers[0] (linear, repeating around).
Texture2D<float4> u_SkyViewTable : register(t27);

// Screen-space ambient occlusion of the current frame (valid when AOParams.x > 0).
Texture2D<float4> u_AmbientOcclusion : register(t11);

// Direction from a surface point toward the viewer. Orthographic projections view every pixel along the same
// direction, perspective ones from the camera position.
float3 GetViewVector(float3 relativePosition)
{
	if (u_Frame.CameraForward.w > 0.5)
		return -u_Frame.CameraForward.xyz;
	return normalize(-relativePosition);
}

// Environment rotation around +Y (EnvironmentParams.y radians).
float3 RotateEnvironmentDirection(float3 direction)
{
	float s = sin(u_Frame.EnvironmentParams.y);
	float c = cos(u_Frame.EnvironmentParams.y);
	return float3(c * direction.x + s * direction.z, direction.y, -s * direction.x + c * direction.z);
}

// Bindless texture table (descriptor set 1, see BindlessTextureTable).
Texture2D<float4> u_Textures[] : register(t0, space1);

// The texture table (Strata/Renderer/TextureTable.h, GPUEntry): per texture index of material data, x its bindless slot
// (bits 0-23), sampler index (bits 24-30) and ST_TEXTURE_SLOT_GRAYSCALE, y the float bits of the finest level of detail it
// is sampled with (levels fading in; 0 otherwise). Textures keep their index while their levels stream. Fragment shaders
// sample it through SampleMaterialTexture (Include/MaterialTextures.slang).
StructuredBuffer<uint2> u_TextureTable : register(t16);

// The directional shadow cascades (RenderData::CascadeData), fitted on the GPU (Scene/CascadeFit.comp).
struct CascadeData
{
	float4x4 ViewProjection; // Light clip space, reversed-Z
	float4 Parameters;     // x: world units per shadow texel, y: depth range in world units, z: penumbra search distance,
	                     // w: the near plane's light view z (camera-relative)
};

StructuredBuffer<CascadeData> u_Cascades : register(t17);

float2 TransformUV(MaterialData material, float2 uv)
{
	return uv * material.UVTransform.xy + material.UVTransform.zw;
}

// Octahedral encoding of unit vectors into [-1, 1]^2 (normal G-buffer).
float2 OctahedralEncode(float3 n)
{
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	float2 encoded = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return encoded;
}

float3 OctahedralDecode(float2 encoded)
{
	float3 n = float3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
	float t = max(-n.z, 0.0);
	n.x += n.x >= 0.0 ? -t : t;
	n.y += n.y >= 0.0 ? -t : t;
	return normalize(n);
}

#endif
