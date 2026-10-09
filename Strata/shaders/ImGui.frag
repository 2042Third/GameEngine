#version 450
#extension GL_GOOGLE_include_directive : require

#include "Include/Common.glsl"

layout(set = 0, binding = ST_SRV(0)) uniform texture2D u_Texture;
layout(set = 0, binding = ST_SAMPLER(0)) uniform sampler u_Sampler;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in vec4 v_Color;

layout(location = 0) out vec4 o_Color;

void main()
{
	o_Color = v_Color * texture(sampler2D(u_Texture, u_Sampler), v_TexCoord);
}
