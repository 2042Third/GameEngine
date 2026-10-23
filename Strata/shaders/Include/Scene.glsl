#ifndef STRATA_SCENE_GLSL
#define STRATA_SCENE_GLSL

// Resources shared by the scene passes. Layouts mirror the structs in Strata/Renderer/SceneRenderData.h; keep them
// in sync (std140 for the constant buffer, std430 for storage buffers, 16-byte aligned members).

#extension GL_EXT_nonuniform_qualifier : require

#include "Include/Common.glsl"

#define ST_MATERIAL_ALPHA_MASK   1u
#define ST_MATERIAL_ALPHA_BLEND  2u
#define ST_MATERIAL_UNLIT        4u
#define ST_MATERIAL_DOUBLE_SIDED 8u

#define ST_INSTANCE_MIRRORED 1u // Negative determinant: winding and tangent handedness are flipped
#define ST_INSTANCE_SKINNED  2u // Posed: SkinPalette is the first joint matrix of its palette (the skinned pipelines)

#define ST_LIGHT_POINT 0u
#define ST_LIGHT_SPOT  1u

#define ST_SAMPLER_COUNT 6

// Texture table entries (u_TextureTable): bindless slot in bits 0-23, sampler index in bits 24-30, and this bit for
// one-channel textures that stand for gray (sampled as (r, r, r, 1)).
#define ST_TEXTURE_SLOT_GRAYSCALE 0x80000000u

// Scene shaders work in camera-relative world space: world space with the camera at the origin (TransformPositionRelative),
// so what is near the camera keeps full precision however far from the world's origin it is. View, ViewProjection and their
// inverses are rotation-only (the view's translation is zero); CameraPosition is the camera's world position.
struct FrameConstants
{
	mat4 View;
	mat4 Projection;
	mat4 ViewProjection;
	mat4 InverseView;
	mat4 InverseProjection;
	mat4 InverseViewProjection;
	// What motion vectors measure positions with: this frame's view-projection without a temporal jitter, and the renderer's
	// previous frame's, which takes positions relative to this frame's camera to that frame's clip space.
	mat4 UnjitteredViewProjection;
	mat4 PreviousViewProjection;
	vec4 CameraPosition;            // xyz: world position, w: near clip distance
	vec4 CameraForward;             // xyz: world view direction, w: 1 for orthographic projections
	vec4 ViewportSize;             // xy: size in pixels, zw: 1 / size
	vec4 DirectionalLightDirection; // xyz: direction the light travels (normalized), w: 1 when the light exists
	vec4 DirectionalLightColor;     // rgb: color * intensity
	vec4 AmbientColor;              // rgb: ambient radiance used without an environment map
	vec4 EnvironmentParams;         // x: intensity, y: rotation (radians), z: specular mip count, w: 1 when enabled
	uvec4 LightCounts;              // x: punctual lights in the light buffer
	uvec4 ClusterGrid;              // xyz: light clusters along screen x, screen y and depth, w: lights per cluster
	vec4 ClusterDepth;              // Depth slice = log(view depth) * x + y; z, w: near and far view depth
	vec4 TimeParams;                // x: scene time in seconds
	vec4 SkyParams;                 // x: background mip (blur), y: 1 when the environment is the background
	vec4 CascadeSplits;             // View distance where each cascade ends (their projections: u_Cascades)
	// x: 2 tan(light angular radius), y: 1 / shadow map size, z: the fraction of each cascade's depth range over which it blends
	// into the next (DirectionalLightComponent::CascadeBlend), w: cascade count (0 = none)
	vec4 ShadowParams;
	vec4 ShadowBias;                // x: depth bias (texels), y: normal offset (texels)
	vec4 AOParams;                  // x: 1 when screen-space ambient occlusion is available
	vec4 MaterialParams;            // x: 1 when raised texture levels fade in (SceneRendererSpecification::FadeTextureLevels)
	// x: 1 when u_PreviousInstances and u_PreviousJoints hold the renderer's previous frame; y: the pre-exposure, a power of two
	// every writer of the HDR target multiplies its luminance by (SceneRenderer: values near 100,000 lux stay within half
	// floats); z: the frame's rotation of per-pixel noise (PCSS, ambient occlusion) in turns, 0 without temporal anti-aliasing
	vec4 TemporalParams;
	// Temporal anti-aliasing's offset of the frame's samples in clip units (in Projection, ViewProjection and their inverses; 0
	// without it): xy this frame's, zw the renderer's previous frame's
	vec4 JitterParams;
	// Height fog (HeightFogComponent, Include/Fog.glsl; FogParams.x is 0 without one):
	vec4 FogDensity; // x: density at the base height (per meter), y: height falloff, z: the camera's height above the base, w: start distance
	vec4 FogColor;   // rgb: the scattering's albedo, w: maximum opacity
	// rgb: light the fog scatters besides the sun's (the procedural sky's horizon, the ambient color, or 1 when ColorFromSky
	// is off), w: 1 when an environment map's irradiance from above lights it instead
	vec4 FogLight;
	vec4 FogParams;  // x: 1 when the scene has fog, y: the asymmetry of its scattering of the directional light
};

layout(set = 0, binding = ST_CBV(0), std140) uniform FrameBuffer
{
	FrameConstants u_Frame;
};

struct InstanceData
{
	vec4 WorldRows[3]; // The upper three rows of the affine world matrix (the fourth is 0, 0, 0, 1)
	uint MaterialIndex;
	uint EntityID;     // Entity identifier + 1 (0 = no entity)
	uint Flags;        // ST_INSTANCE_*
	uint SkinPalette;  // ST_INSTANCE_SKINNED: the first joint matrix of the instance's palette in u_Joints
};

// Every instance of the render scene; it persists between frames.
layout(set = 0, binding = ST_SRV(0), std430) readonly buffer InstanceBuffer
{
	InstanceData u_Instances[];
};

// The frame's draw list: an entry per drawn instance, in batch order (gl_InstanceIndex indexes it): the instance in the low
// ST_DRAW_ENTRY_INSTANCE_BITS bits, its level-of-detail cross-fade code above them (RenderData::MakeDrawEntry).
layout(set = 0, binding = ST_SRV(14), std430) readonly buffer DrawListBuffer
{
	uint u_DrawList[];
};

#ifdef ST_VELOCITY
// The motion history (SceneRenderOptions::MotionVectors): every instance as the renderer's previous frame drew it, valid when
// TemporalParams.x is 1.
layout(set = 0, binding = ST_SRV(20), std430) readonly buffer PreviousInstanceBuffer
{
	InstanceData u_PreviousInstances[];
};
#endif

// The motion from a previous clip position to the current one, in UV units (current minus previous).
vec2 GetClipMotion(vec4 currentClip, vec4 previousClip)
{
	return NDCToUV(currentClip.xy / currentClip.w) - NDCToUV(previousClip.xy / previousClip.w);
}

#define ST_DRAW_ENTRY_INSTANCE_BITS 27u          // RenderData::c_DrawEntryInstanceBits
#define ST_DRAW_ENTRY_INSTANCE_MASK 0x07FFFFFFu
#define ST_LOD_FADE_OUTGOING_CODE   16u          // RenderData::c_LODFadeOutgoing

// Whether a draw with this cross-fade code covers the pixel (LODSelection::IsLODFadeDrawn): the level fading in (codes 1-15)
// where the 4x4 ordered dither threshold is below the code, the level fading out (the code plus 16) on the other pixels.
// The depth prepass and the forward pass test the same, so the EQUAL depth test holds.
bool IsLODFadeDrawn(uint code, vec2 fragCoord)
{
	if (code == 0u)
		return true;
	const uint bayer[16] = uint[16](0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u);
	uvec2 pixel = uvec2(fragCoord) & 3u;
	uint threshold = bayer[pixel.y * 4u + pixel.x];
	return code < ST_LOD_FADE_OUTGOING_CODE ? threshold < code : threshold >= code - ST_LOD_FADE_OUTGOING_CODE;
}

#ifdef ST_SKINNED
// The palettes of the posed instances (RenderScene::GetPalette): per joint of a skin the upper three rows of its affine
// skinning matrix, which takes a bind-pose vertex into the instance's local space.
struct JointMatrix
{
	vec4 Rows[3];
};

layout(set = 0, binding = ST_SRV(15), std430) readonly buffer JointBuffer
{
	JointMatrix u_Joints[];
};

// The skinning matrix of a vertex: its influences' palette entries blended by weight (linear blend skinning).
JointMatrix BlendJoints(InstanceData instance, uvec4 joints, vec4 weights)
{
	JointMatrix blended;
	blended.Rows[0] = vec4(0.0);
	blended.Rows[1] = vec4(0.0);
	blended.Rows[2] = vec4(0.0);
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
layout(set = 0, binding = ST_SRV(21), std430) readonly buffer PreviousJointBuffer
{
	JointMatrix u_PreviousJoints[];
};

JointMatrix BlendPreviousJoints(InstanceData instance, uvec4 joints, vec4 weights)
{
	JointMatrix blended;
	blended.Rows[0] = vec4(0.0);
	blended.Rows[1] = vec4(0.0);
	blended.Rows[2] = vec4(0.0);
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

vec3 SkinPosition(JointMatrix skin, vec3 position)
{
	vec4 point = vec4(position, 1.0);
	return vec3(dot(skin.Rows[0], point), dot(skin.Rows[1], point), dot(skin.Rows[2], point));
}

vec3 SkinDirection(JointMatrix skin, vec3 direction)
{
	return vec3(dot(skin.Rows[0].xyz, direction), dot(skin.Rows[1].xyz, direction), dot(skin.Rows[2].xyz, direction));
}

// A normal through the skinning matrix: its cofactor matrix, like TransformNormal (callers normalize).
vec3 SkinNormal(JointMatrix skin, vec3 normal)
{
	vec3 column0 = vec3(skin.Rows[0].x, skin.Rows[1].x, skin.Rows[2].x);
	vec3 column1 = vec3(skin.Rows[0].y, skin.Rows[1].y, skin.Rows[2].y);
	vec3 column2 = vec3(skin.Rows[0].z, skin.Rows[1].z, skin.Rows[2].z);
	vec3 transformed = cross(column1, column2) * normal.x + cross(column2, column0) * normal.y + cross(column0, column1) * normal.z;
	return dot(column0, cross(column1, column2)) < 0.0 ? -transformed : transformed;
}
#endif

// A point of the instance in camera-relative world space: the instance's translation minus the camera's position is taken
// before anything is added to it, so vertices near the camera keep full precision far from the world's origin.
vec3 TransformPositionRelative(InstanceData instance, vec3 position)
{
	vec3 translation = vec3(instance.WorldRows[0].w, instance.WorldRows[1].w, instance.WorldRows[2].w) - u_Frame.CameraPosition.xyz;
	return vec3(dot(instance.WorldRows[0].xyz, position), dot(instance.WorldRows[1].xyz, position), dot(instance.WorldRows[2].xyz, position)) + translation;
}

vec3 TransformDirection(InstanceData instance, vec3 direction)
{
	return vec3(dot(instance.WorldRows[0].xyz, direction), dot(instance.WorldRows[1].xyz, direction), dot(instance.WorldRows[2].xyz, direction));
}

// A normal transformed by the inverse transpose of the world matrix, up to scale: the cofactor matrix of the upper 3x3
// (its columns are the cross products of the matrix's columns), negated for mirroring transforms, whose determinant is
// negative. Callers normalize.
vec3 TransformNormal(InstanceData instance, vec3 normal)
{
	vec3 column0 = vec3(instance.WorldRows[0].x, instance.WorldRows[1].x, instance.WorldRows[2].x);
	vec3 column1 = vec3(instance.WorldRows[0].y, instance.WorldRows[1].y, instance.WorldRows[2].y);
	vec3 column2 = vec3(instance.WorldRows[0].z, instance.WorldRows[1].z, instance.WorldRows[2].z);
	vec3 transformed = cross(column1, column2) * normal.x + cross(column2, column0) * normal.y + cross(column0, column1) * normal.z;
	return (instance.Flags & ST_INSTANCE_MIRRORED) != 0u ? -transformed : transformed;
}

struct MaterialData
{
	vec4 BaseColor;
	vec4 Emissive;           // rgb: color * intensity
	float Metallic;
	float Roughness;
	float NormalScale;
	float OcclusionStrength;
	float AlphaCutoff;
	uint Flags;
	uint BaseColorMap;       // Texture table indices (u_TextureTable) from here on
	uint MetallicRoughnessMap;
	vec4 UVTransform;        // xy: tiling, zw: offset
	uint NormalMap;
	uint OcclusionMap;
	uint EmissiveMap;
	uint Padding;
};

layout(set = 0, binding = ST_SRV(1), std430) readonly buffer MaterialBuffer
{
	MaterialData u_Materials[];
};

struct LightData
{
	vec4 PositionRange;  // xyz: camera-relative world position, w: range
	vec4 Color;          // rgb: color * intensity, w: 1 / range^2
	vec4 DirectionType;  // xyz: direction the light travels, w: type (ST_LIGHT_*)
	vec4 SpotAngles;     // x: cos(inner cone), y: cos(outer cone), z: its shadow slot plus one (0: no shadow; Include/LocalShadows.glsl)
};

layout(set = 0, binding = ST_SRV(2), std430) readonly buffer LightBuffer
{
	LightData u_Lights[];
};

// Samplers indexed by TextureFilter * 3 + TextureWrap (Linear/Nearest x Repeat/Clamp/Mirror).
layout(set = 0, binding = ST_SAMPLER(0)) uniform sampler u_Samplers[ST_SAMPLER_COUNT];

// Image-based lighting (valid when EnvironmentParams.w > 0).
layout(set = 0, binding = ST_SRV(3)) uniform textureCube u_IrradianceMap;
layout(set = 0, binding = ST_SRV(4)) uniform textureCube u_PrefilteredMap;
layout(set = 0, binding = ST_SRV(5)) uniform texture2D u_BRDFLut;
layout(set = 0, binding = ST_SRV(6)) uniform textureCube u_EnvironmentMap;
layout(set = 0, binding = ST_SAMPLER(8)) uniform sampler u_LinearClampSampler;

// Screen-space ambient occlusion of the current frame (valid when AOParams.x > 0).
layout(set = 0, binding = ST_SRV(11)) uniform texture2D u_AmbientOcclusion;

// Direction from a surface point toward the viewer. Orthographic projections view every pixel along the same
// direction, perspective ones from the camera position.
vec3 GetViewVector(vec3 relativePosition)
{
	if (u_Frame.CameraForward.w > 0.5)
		return -u_Frame.CameraForward.xyz;
	return normalize(-relativePosition);
}

// Environment rotation around +Y (EnvironmentParams.y radians).
vec3 RotateEnvironmentDirection(vec3 direction)
{
	float s = sin(u_Frame.EnvironmentParams.y);
	float c = cos(u_Frame.EnvironmentParams.y);
	return vec3(c * direction.x + s * direction.z, direction.y, -s * direction.x + c * direction.z);
}

// Bindless texture table (descriptor set 1, see BindlessTextureTable).
layout(set = 1, binding = 0) uniform texture2D u_Textures[];

// The texture table (Strata/Renderer/TextureTable.h, GPUEntry): per texture index of material data, x its bindless slot
// (bits 0-23), sampler index (bits 24-30) and ST_TEXTURE_SLOT_GRAYSCALE, y the float bits of the finest level of detail it
// is sampled with (levels fading in; 0 otherwise). Textures keep their index while their levels stream. Fragment shaders
// sample it through SampleMaterialTexture (Include/MaterialTextures.glsl).
layout(set = 0, binding = ST_SRV(16), std430) readonly buffer TextureTableBuffer
{
	uvec2 u_TextureTable[];
};

// The directional shadow cascades (RenderData::CascadeData), fitted on the GPU (Scene/CascadeFit.comp).
struct CascadeData
{
	mat4 ViewProjection; // Light clip space, reversed-Z
	vec4 Parameters;     // x: world units per shadow texel, y: depth range in world units, z: penumbra search distance,
	                     // w: the near plane's light view z (camera-relative)
};

layout(set = 0, binding = ST_SRV(17), std430) readonly buffer CascadeBuffer
{
	CascadeData u_Cascades[];
};

vec2 TransformUV(MaterialData material, vec2 uv)
{
	return uv * material.UVTransform.xy + material.UVTransform.zw;
}

// Octahedral encoding of unit vectors into [-1, 1]^2 (normal G-buffer).
vec2 OctahedralEncode(vec3 n)
{
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	vec2 encoded = n.z >= 0.0 ? n.xy : (1.0 - abs(n.yx)) * vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return encoded;
}

vec3 OctahedralDecode(vec2 encoded)
{
	vec3 n = vec3(encoded, 1.0 - abs(encoded.x) - abs(encoded.y));
	float t = max(-n.z, 0.0);
	n.x += n.x >= 0.0 ? -t : t;
	n.y += n.y >= 0.0 ? -t : t;
	return normalize(n);
}

#endif
