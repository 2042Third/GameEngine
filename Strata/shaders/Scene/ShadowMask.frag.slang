#include "Include/Common.slang"

#include "Include/Scene.slang"
#include "Include/MaterialTextures.slang"

// Shadow casters with alpha-masked materials: discard cut-out texels. Opaque casters use no fragment shader.

static float2 v_TexCoord;
static uint v_InstanceIndex;

void st_Main()
{
	MaterialData material = u_Materials[u_Instances[v_InstanceIndex].MaterialIndex];
	float alpha = material.BaseColor.a * SampleMaterialTexture(material.BaseColorMap, TransformUV(material, v_TexCoord)).a;
	if (alpha < material.AlphaCutoff)
		discard;
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float2 v_TexCoord : TEXCOORD0;
	[[vk::location(1)]] nointerpolation uint v_InstanceIndex : TEXCOORD1;
};
struct ST_Output
{
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_TexCoord = input.v_TexCoord;
	v_InstanceIndex = input.v_InstanceIndex;
	st_Main();
	ST_Output output;
	return output;
}
