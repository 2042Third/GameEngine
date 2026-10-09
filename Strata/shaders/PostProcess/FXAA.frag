#version 460
#extension GL_GOOGLE_include_directive : require

#include "Include/Common.glsl"

// Fast approximate anti-aliasing (after FXAA 3.11 quality by Timothy Lottes) on the display-encoded image: finds
// luma edges, walks along each edge to its ends and blends across it by the pixel's position along the edge.

layout(set = 0, binding = ST_SRV(0)) uniform texture2D u_Color;
layout(set = 0, binding = ST_SAMPLER(0)) uniform sampler u_LinearClamp;

layout(location = 0) in vec2 v_UV;
layout(location = 0) out vec4 o_Color;

const float c_EdgeThreshold = 0.125;     // Minimum local contrast, relative to the brightest neighbor
const float c_EdgeThresholdMin = 0.0312; // Minimum absolute contrast (keeps dark noise untouched)
const float c_Subpixel = 0.75;           // Amount of sub-pixel aliasing removal
const int c_SearchSteps = 12;
const float c_StepSizes[c_SearchSteps] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float Luma(vec3 color)
{
	return dot(color, vec3(0.299, 0.587, 0.114)); // Of display-encoded values, as FXAA expects
}

vec3 Fetch(vec2 uv)
{
	return textureLod(sampler2D(u_Color, u_LinearClamp), uv, 0.0).rgb;
}

void main()
{
	vec2 texel = 1.0 / vec2(textureSize(sampler2D(u_Color, u_LinearClamp), 0));
	vec2 uv = v_UV;
	vec3 center = Fetch(uv);
	float lumaM = Luma(center);
	float lumaN = Luma(Fetch(uv + vec2(0.0, -texel.y)));
	float lumaS = Luma(Fetch(uv + vec2(0.0, texel.y)));
	float lumaW = Luma(Fetch(uv + vec2(-texel.x, 0.0)));
	float lumaE = Luma(Fetch(uv + vec2(texel.x, 0.0)));

	float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
	float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
	float range = lumaMax - lumaMin;
	if (range < max(c_EdgeThresholdMin, lumaMax * c_EdgeThreshold))
	{
		o_Color = vec4(center, 1.0);
		return;
	}

	float lumaNW = Luma(Fetch(uv + vec2(-texel.x, -texel.y)));
	float lumaNE = Luma(Fetch(uv + vec2(texel.x, -texel.y)));
	float lumaSW = Luma(Fetch(uv + vec2(-texel.x, texel.y)));
	float lumaSE = Luma(Fetch(uv + vec2(texel.x, texel.y)));

	// Sub-pixel aliasing: how far the center is from the average of its neighborhood.
	float neighborhood = (2.0 * (lumaN + lumaS + lumaW + lumaE) + lumaNW + lumaNE + lumaSW + lumaSE) / 12.0;
	float subpixel = smoothstep(0.0, 1.0, clamp(abs(neighborhood - lumaM) / range, 0.0, 1.0));
	float subpixelOffset = subpixel * subpixel * c_Subpixel;

	// Edge orientation from the second derivatives across the 3x3 block.
	float edgeHorizontal = abs(lumaNW - 2.0 * lumaW + lumaSW) + 2.0 * abs(lumaN - 2.0 * lumaM + lumaS) + abs(lumaNE - 2.0 * lumaE + lumaSE);
	float edgeVertical = abs(lumaNW - 2.0 * lumaN + lumaNE) + 2.0 * abs(lumaW - 2.0 * lumaM + lumaE) + abs(lumaSW - 2.0 * lumaS + lumaSE);
	bool horizontal = edgeHorizontal >= edgeVertical;

	// The side of the edge with the steeper gradient; the search runs along the boundary half a pixel toward it.
	float stepLength = horizontal ? texel.y : texel.x;
	float luma1 = horizontal ? lumaN : lumaW;
	float luma2 = horizontal ? lumaS : lumaE;
	float gradient1 = abs(luma1 - lumaM);
	float gradient2 = abs(luma2 - lumaM);
	bool steeper1 = gradient1 >= gradient2;
	float gradientScaled = 0.25 * max(gradient1, gradient2);
	float lumaLocalAverage = 0.5 * ((steeper1 ? luma1 : luma2) + lumaM);
	if (steeper1)
		stepLength = -stepLength;

	vec2 edgeUV = uv;
	vec2 offset;
	if (horizontal)
	{
		edgeUV.y += 0.5 * stepLength;
		offset = vec2(texel.x, 0.0);
	}
	else
	{
		edgeUV.x += 0.5 * stepLength;
		offset = vec2(0.0, texel.y);
	}

	vec2 uv1 = edgeUV - offset;
	vec2 uv2 = edgeUV + offset;
	float lumaEnd1 = Luma(Fetch(uv1)) - lumaLocalAverage;
	float lumaEnd2 = Luma(Fetch(uv2)) - lumaLocalAverage;
	bool reached1 = abs(lumaEnd1) >= gradientScaled;
	bool reached2 = abs(lumaEnd2) >= gradientScaled;
	for (int step = 1; step < c_SearchSteps && !(reached1 && reached2); step++)
	{
		if (!reached1)
		{
			uv1 -= offset * c_StepSizes[step];
			lumaEnd1 = Luma(Fetch(uv1)) - lumaLocalAverage;
			reached1 = abs(lumaEnd1) >= gradientScaled;
		}
		if (!reached2)
		{
			uv2 += offset * c_StepSizes[step];
			lumaEnd2 = Luma(Fetch(uv2)) - lumaLocalAverage;
			reached2 = abs(lumaEnd2) >= gradientScaled;
		}
	}

	float distance1 = horizontal ? uv.x - uv1.x : uv.y - uv1.y;
	float distance2 = horizontal ? uv2.x - uv.x : uv2.y - uv.y;
	bool closer1 = distance1 < distance2;
	float edgeOffset = 0.5 - min(distance1, distance2) / (distance1 + distance2);

	// Blend only when the luma at the nearer edge end varies the opposite way to the center (a real edge end).
	bool centerSmaller = lumaM < lumaLocalAverage;
	bool correctVariation = ((closer1 ? lumaEnd1 : lumaEnd2) < 0.0) != centerSmaller;
	float finalOffset = max(correctVariation ? edgeOffset : 0.0, subpixelOffset);

	vec2 finalUV = uv;
	if (horizontal)
		finalUV.y += finalOffset * stepLength;
	else
		finalUV.x += finalOffset * stepLength;
	o_Color = vec4(Fetch(finalUV), 1.0);
}
