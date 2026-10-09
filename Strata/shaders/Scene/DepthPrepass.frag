#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"

// Depth prepass: writes depth, view-space normals (screen-space effects) and entity ids (picking). Alpha-masked
// materials are tested here; the forward pass then shades only the visible surface.

layout(location = 0) in vec3 v_WorldPosition;
layout(location = 1) in vec3 v_Normal;
layout(location = 2) in vec4 v_Tangent;
layout(location = 3) in vec2 v_TexCoord;
layout(location = 4) flat in uint v_InstanceIndex;

layout(location = 0) out vec2 o_Normal;
layout(location = 1) out uint o_EntityID;

void main()
{
	InstanceData instance = u_Instances[v_InstanceIndex];
	MaterialData material = u_Materials[instance.MaterialIndex];
	if ((material.Flags & ST_MATERIAL_ALPHA_MASK) != 0u)
	{
		float alpha = material.BaseColor.a * SampleMaterialTexture(material.BaseColorMap, TransformUV(material, v_TexCoord)).a;
		if (alpha < material.AlphaCutoff)
			discard;
	}

	vec3 normal = normalize(v_Normal);
	if (!gl_FrontFacing)
		normal = -normal;
	o_Normal = OctahedralEncode(normalize(mat3(u_Frame.View) * normal));
	o_EntityID = instance.EntityID;
}
