#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"

// Shadow casters with alpha-masked materials: discard cut-out texels. Opaque casters use no fragment shader.

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) flat in uint v_InstanceIndex;

void main()
{
	MaterialData material = u_Materials[u_Instances[v_InstanceIndex].MaterialIndex];
	float alpha = material.BaseColor.a * SampleMaterialTexture(material.BaseColorMap, TransformUV(material, v_TexCoord)).a;
	if (alpha < material.AlphaCutoff)
		discard;
}
