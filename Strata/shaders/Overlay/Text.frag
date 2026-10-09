#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Common.glsl"

// Signed distance field text (see FontAtlas): the glyph outline is where the distance crosses the atlas's on-edge
// value; coverage ramps over about one screen pixel around it, so text stays sharp and smooth at any size.

layout(set = 0, binding = ST_SRV(15)) uniform texture2DArray u_Atlas; // One layer per atlas page
layout(set = 0, binding = ST_SAMPLER(15)) uniform sampler u_AtlasSampler;

layout(location = 0) in vec3 v_TexCoord; // Atlas texels and page (the same for a whole glyph)
layout(location = 1) in vec4 v_Color;

layout(location = 0) out vec4 o_Color;

const float c_OnEdgeValue = 128.0 / 255.0; // FontAtlas c_OnEdgeValue

void main()
{
	vec2 pageSize = vec2(textureSize(sampler2DArray(u_Atlas, u_AtlasSampler), 0).xy);
	// Array layers are selected by rounding, so interpolation error in the page coordinate does not matter.
	float distance = texture(sampler2DArray(u_Atlas, u_AtlasSampler), vec3(v_TexCoord.xy / pageSize, v_TexCoord.z)).r;
	float width = max(fwidth(distance) * 0.7, 1e-4);
	float coverage = smoothstep(c_OnEdgeValue - width, c_OnEdgeValue + width, distance);
	if (coverage <= 0.0)
		discard;
	o_Color = vec4(v_Color.rgb, v_Color.a * coverage);
}
