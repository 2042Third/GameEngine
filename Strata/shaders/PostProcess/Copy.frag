#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/Common.glsl"

// Copies an image of the target's size texel for texel (format conversion only).

layout(set = 0, binding = ST_SRV(0)) uniform texture2D u_Source;

layout(location = 0) out vec4 o_Color;

void main()
{
	o_Color = texelFetch(u_Source, ivec2(gl_FragCoord.xy), 0);
}
