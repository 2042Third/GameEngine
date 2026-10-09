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

#define ST_LIGHT_POINT 0u
#define ST_LIGHT_SPOT  1u

#define ST_SAMPLER_COUNT 6

struct FrameConstants
{
	mat4 View;
	mat4 Projection;
	mat4 ViewProjection;
	mat4 InverseView;
	mat4 InverseProjection;
	mat4 InverseViewProjection;
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
	mat4 CascadeViewProjection[4];  // Directional shadow cascades (light clip space, reversed-Z)
	vec4 CascadeSplits;             // View distance where each cascade ends
	vec4 CascadeData[4];            // x: world units per shadow texel, y: depth range in world units, z: penumbra search distance
	vec4 ShadowParams;              // x: 2 tan(light angular radius), y: 1 / shadow map size, w: cascade count (0 = none)
	vec4 ShadowBias;                // x: depth bias (texels), y: normal offset (texels)
	vec4 AOParams;                  // x: 1 when screen-space ambient occlusion is available
};

layout(set = 0, binding = ST_CBV(0), std140) uniform FrameBuffer
{
	FrameConstants u_Frame;
};

struct InstanceData
{
	mat4 World;
	mat4 NormalMatrix; // Inverse transpose of World (not set for shadow casters)
	uint MaterialIndex;
	uint EntityID;     // Entity identifier + 1 (0 = no entity)
	uint Flags;        // ST_INSTANCE_*
	uint Padding;
};

layout(set = 0, binding = ST_SRV(0), std430) readonly buffer InstanceBuffer
{
	InstanceData u_Instances[];
};

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
	uint BaseColorMap;       // Bits 0-23: bindless texture slot, bits 24-31: sampler index
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
	vec4 PositionRange;  // xyz: world position, w: range
	vec4 Color;          // rgb: color * intensity, w: 1 / range^2
	vec4 DirectionType;  // xyz: direction the light travels, w: type (ST_LIGHT_*)
	vec4 SpotAngles;     // x: cos(inner cone), y: cos(outer cone)
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
vec3 GetViewVector(vec3 worldPosition)
{
	if (u_Frame.CameraForward.w > 0.5)
		return -u_Frame.CameraForward.xyz;
	return normalize(u_Frame.CameraPosition.xyz - worldPosition);
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

vec4 SampleMaterialTexture(uint packedSlot, vec2 uv)
{
	uint slot = packedSlot & 0xFFFFFFu;
	uint samplerIndex = min(packedSlot >> 24, uint(ST_SAMPLER_COUNT - 1));
	return texture(sampler2D(u_Textures[nonuniformEXT(slot)], u_Samplers[nonuniformEXT(samplerIndex)]), uv);
}

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
