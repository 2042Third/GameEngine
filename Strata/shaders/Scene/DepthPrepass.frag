#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"
#include "Include/MaterialTextures.glsl"

// Depth prepass: writes depth, view-space normals (screen-space effects) and entity ids (picking). Alpha-masked
// materials are tested here; the forward pass then shades only the visible surface. With ST_NO_ENTITY_IDS (registered as
// Scene/DepthPrepassNoEntityIDs.frag) it writes no entity ids, for renderers without an entity-ID buffer.

layout(location = 0) in vec3 v_RelativePosition;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec4 v_Tangent;
layout(location = 3) in vec2 v_TexCoord;
layout(location = 4) flat in uint v_InstanceIndex;
layout(location = 5) flat in uint v_LODFade;

layout(location = 0) out vec2 o_Normal;
#ifndef ST_NO_ENTITY_IDS
layout(location = 1) out uint o_EntityID;
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
	if (!IsLODFadeDrawn(v_LODFade, gl_FragCoord.xy))
		discard;
	if (masked && alpha < material.AlphaCutoff)
		discard;

	vec3 normal = normalize(v_Normal);
	if (!gl_FrontFacing)
		normal = -normal;
	o_Normal = OctahedralEncode(normalize(mat3(u_Frame.View) * normal));
#ifndef ST_NO_ENTITY_IDS
	o_EntityID = instance.EntityID;
#endif
}
