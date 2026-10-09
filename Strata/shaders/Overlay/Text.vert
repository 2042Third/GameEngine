#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Scene.glsl"

// Glyph quads of TextRenderer: world-space positions, or pixel positions (+Y down) for screen-space text.

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_TexCoord; // Atlas texels and page
layout(location = 2) in vec4 a_Color;    // RGBA8 UNORM, display colors

layout(location = 0) out vec3 v_TexCoord;
layout(location = 1) out vec4 v_Color;

layout(push_constant) uniform TextParameters
{
	vec2 ViewportSize;
	uint ScreenSpace;
	uint Padding;
} u_Text;

void main()
{
	v_TexCoord = a_TexCoord;
	v_Color = a_Color;
	if (u_Text.ScreenSpace != 0u)
		gl_Position = vec4(UVToNDC(a_Position.xy / u_Text.ViewportSize), 0.5, 1.0);
	else
		gl_Position = u_Frame.ViewProjection * vec4(a_Position, 1.0);
}
