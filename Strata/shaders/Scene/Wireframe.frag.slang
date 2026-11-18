#include "Include/Common.slang"

#include "Include/Scene.slang"

// The wireframe debug view (SceneRenderOptions::ShowWireframe): the scene pipelines of Pass::Wireframe rasterize every
// triangle the frame drew as its three edges (the mesh vertex shaders, skinned or not), which this shader colors with the push
// constant's color, blended over the finished image where no surface lies in front of them. A level of detail that fades shows
// its edges on the pixels its surface covers (the prepass's test of the fade's mask); alpha masks and textures are not read,
// so an alpha-masked surface shows all its triangles.

struct WireframeParameters
{
	uint Color; // RGBA8, sRGB-encoded, packed (red in the low byte); alpha the lines' opacity
};
[[vk::push_constant]] ConstantBuffer<WireframeParameters> u_Wireframe;

static uint v_LODFade;
static float4 o_Color;

void st_Main()
{
	if (!IsLODFadeDrawn(v_LODFade, st_FragCoord.xy))
		discard;
	o_Color = UnpackUnorm4x8(u_Wireframe.Color);
}

// --- Entry point ---
struct ST_Input
{
	[[vk::location(0)]] float3 v_RelativePosition : TEXCOORD0;
	[[vk::location(1)]] float3 v_Normal : TEXCOORD1;
	[[vk::location(2)]] float4 v_Tangent : TEXCOORD2;
	[[vk::location(3)]] float2 v_TexCoord : TEXCOORD3;
	[[vk::location(4)]] nointerpolation uint v_InstanceIndex : TEXCOORD4;
	[[vk::location(5)]] nointerpolation uint v_LODFade : TEXCOORD5;
};
struct ST_Output
{
	[[vk::location(0)]] float4 o_Color : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	v_LODFade = input.v_LODFade;
	st_Main();
	ST_Output output;
	output.o_Color = o_Color;
	return output;
}
