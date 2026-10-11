#version 450
#extension GL_GOOGLE_include_directive : require

#include "Include/Common.glsl"

layout(set = 0, binding = ST_SRV(0)) uniform texture2D u_Texture;
layout(set = 0, binding = ST_SAMPLER(0)) uniform sampler u_Sampler;

// The vertex shader's push constants, then how the draw shows its texture (ImGuiTextureDisplay in ImGuiRenderer.h).
layout(push_constant) uniform PushConstants
{
	vec2 Scale;
	vec2 Translate;
	uint TextureDisplay;
} u_Push;

const uint c_DisplayEncodeSRGB = 1u;
const uint c_DisplayGrayscale = 2u;
const uint c_DisplayNormalXY = 3u;

layout(location = 0) in vec2 v_TexCoord;
layout(location = 1) in vec4 v_Color;

layout(location = 0) out vec4 o_Color;

void main()
{
	vec4 texel = texture(sampler2D(u_Texture, u_Sampler), v_TexCoord);
	if (u_Push.TextureDisplay == c_DisplayEncodeSRGB)
	{
		// Sampling an sRGB texture decodes it; the target holds display values, as ImGui's colors are.
		texel.rgb = LinearToSRGB(texel.rgb);
	}
	else if (u_Push.TextureDisplay == c_DisplayGrayscale)
	{
		texel = vec4(texel.rrr, 1.0);
	}
	else if (u_Push.TextureDisplay == c_DisplayNormalXY)
	{
		// As materials read two-channel normal maps (Forward.frag): X and Y, Z reconstructed; shown as normal maps look.
		const vec2 xy = texel.rg * 2.0 - 1.0;
		const float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
		texel = vec4(vec3(xy, z) * 0.5 + 0.5, 1.0);
	}
	o_Color = v_Color * texel;
}
