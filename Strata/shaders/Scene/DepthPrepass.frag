#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"
#include "Include/MaterialTextures.glsl"

// Depth prepass: writes depth, view-space normals (screen-space effects) and entity ids (picking). Alpha-masked
// materials are tested here; the forward pass then shades only the visible surface. With ST_NO_ENTITY_IDS (registered as
// Scene/DepthPrepassNoEntityIDs.frag) it writes no entity ids, for renderers without an entity-ID buffer. With ST_VELOCITY
// (Scene/DepthPrepassVelocity.frag, DepthPrepassNoEntityIDsVelocity.frag) it also writes the motion since the renderer's
// previous frame (Scene/MeshVelocity.vert), in UV units, into the attachment after the others. With ST_SELECTION_MASK
// (Scene/SelectionMask.frag) it writes only 1 into a mask where a surface is drawn, with the same tests: the selection mask
// SceneRenderer draws the selected instances into, with the camera's own matrices, for their outline under temporal
// anti-aliasing. With ST_PREPASS_SURFACE (Scene/DepthPrepassSurface.frag, DepthPrepassNoEntityIDsSurface.frag; always with
// ST_VELOCITY) it also writes the surface target screen-space reflections trace from, after the velocity: the normal-mapped
// view-space normal (octahedral), the roughness and the metallic factor, as the forward pass shades them (unlit surfaces:
// roughness 1, nothing to reflect).

layout(location = 0) in vec3 v_RelativePosition;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec4 v_Tangent;
layout(location = 3) in vec2 v_TexCoord;
layout(location = 4) flat in uint v_InstanceIndex;
layout(location = 5) flat in uint v_LODFade;
#ifdef ST_VELOCITY
layout(location = 6) in vec4 v_CurrentClip;
layout(location = 7) in vec4 v_PreviousClip;
#endif

#ifdef ST_SELECTION_MASK
layout(location = 0) out float o_Selected;
#else
layout(location = 0) out vec2 o_Normal;
#ifndef ST_NO_ENTITY_IDS
layout(location = 1) out uint o_EntityID;
#endif
#if defined(ST_VELOCITY) && defined(ST_NO_ENTITY_IDS)
layout(location = 1) out vec2 o_Velocity;
#elif defined(ST_VELOCITY)
layout(location = 2) out vec2 o_Velocity;
#endif
#if defined(ST_PREPASS_SURFACE) && defined(ST_NO_ENTITY_IDS)
layout(location = 2) out vec4 o_Surface;
#elif defined(ST_PREPASS_SURFACE)
layout(location = 3) out vec4 o_Surface;
#endif
#endif

void main()
{
	InstanceData instance = u_Instances[v_InstanceIndex];
	MaterialData material = u_Materials[instance.MaterialIndex];
	// Sampled before the discards: a quad whose other pixels discarded has no implicit derivatives (texture levels of
	// detail) left (Architecture.Shaders).
	const bool masked = (material.Flags & ST_MATERIAL_ALPHA_MASK) != 0u;
	float alpha = 1.0;
	if (masked)
		alpha = material.BaseColor.a * SampleMaterialTexture(material.BaseColorMap, TransformUV(material, v_TexCoord)).a;
#ifdef ST_PREPASS_SURFACE
	vec4 surface = vec4(0.0, 0.0, 1.0, 0.0);
	if ((material.Flags & ST_MATERIAL_UNLIT) == 0u)
	{
		vec2 uv = TransformUV(material, v_TexCoord);
		vec4 metallicRoughness = SampleMaterialTexture(material.MetallicRoughnessMap, uv);
		vec3 shadingNormal = GetShadingNormal(material, uv, v_Normal, v_Tangent, gl_FrontFacing);
		surface = vec4(OctahedralEncode(normalize(mat3(u_Frame.View) * shadingNormal)), clamp(material.Roughness * metallicRoughness.g, 0.0, 1.0),
			clamp(material.Metallic * metallicRoughness.b, 0.0, 1.0));
	}
#endif
	if (!IsLODFadeDrawn(v_LODFade, gl_FragCoord.xy))
		discard;
	if (masked && alpha < material.AlphaCutoff)
		discard;

#ifdef ST_SELECTION_MASK
	o_Selected = 1.0;
#else
	vec3 normal = normalize(v_Normal);
	if (!gl_FrontFacing)
		normal = -normal;
	o_Normal = OctahedralEncode(normalize(mat3(u_Frame.View) * normal));
#ifndef ST_NO_ENTITY_IDS
	o_EntityID = instance.EntityID;
#endif
#ifdef ST_VELOCITY
	o_Velocity = GetClipMotion(v_CurrentClip, v_PreviousClip);
#endif
#ifdef ST_PREPASS_SURFACE
	o_Surface = surface;
#endif
#endif
}
