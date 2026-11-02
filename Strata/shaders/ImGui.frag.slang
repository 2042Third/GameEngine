#include "Include/Common.slang"

#include "Include/Common.slang"

Texture2D<float4> u_Texture : register(t0);
SamplerState u_Sampler : register(s0);

// The vertex shader's push constants, then how the draw shows its texture (ImGuiTextureDisplay in ImGuiRenderer.h).
struct PushConstants
{
	float2 Scale;
	float2 Translate;
	uint TextureDisplay;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> u_Push;

static const uint c_DisplayEncodeSRGB = 1u;
static const uint c_DisplayGrayscale = 2u;
static const uint c_DisplayNormalXY = 3u;

static float2 v_TexCoord;
static float4 v_Color;

static float4 o_Color;

void st_Main()
{
	float4 texel = u_Texture.Sample(u_Sampler, v_TexCoord);
	if (u_Push.TextureDisplay == c_DisplayEncodeSRGB)
	{
		// Sampling an sRGB texture decodes it; the target holds display values, as ImGui's colors are.
		texel.rgb = LinearToSRGB(texel.rgb);
	}
	else if (u_Push.TextureDisplay == c_DisplayGrayscale)
	{
		texel = float4(texel.rrr, 1.0);
	}
	else if (u_Push.TextureDisplay == c_DisplayNormalXY)
	{
		// As materials read two-channel normal maps (Forward.frag): X and Y, Z reconstructed; shown as normal maps look.
		const float2 xy = texel.rg * 2.0 - 1.0;
		const float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
		texel = float4(float3(xy, z) * 0.5 + 0.5, 1.0);
	}
	o_Color = v_Color * texel;
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float2 v_TexCoord : TEXCOORD0;
	[[vk::location(1)]] float4 v_Color : TEXCOORD1;
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Color : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_TexCoord = input.v_TexCoord;
	v_Color = input.v_Color;
	st_Main();
	ST_Output output;
	output.o_Color = o_Color;
	return output;
}
