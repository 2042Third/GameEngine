#ifndef STRATA_AREA_LIGHTS_SLANG
#define STRATA_AREA_LIGHTS_SLANG

// Rect lights shaded per pixel (Strata/Renderer/AreaLights.h is the reference these functions follow operation for
// operation, in float): diffuse light as the face's form factor, the highlight as the share of a GGX lobe the face covers,
// fitted by linearly transformed cosines (Heitz, Dupuy, Hill and Neubelt, "Real-Time Polygonal-Light Shading with Linearly
// Transformed Cosines", 2016). No sampling and no history: the light is a closed-form function of the pixel's own data.
//
// The edge integral's fit and the horizon term's use are ported from webgl/shaders/ltc/ltc_quad.fs (IntegrateEdgeVec,
// LTC_Evaluate) of https://github.com/selfshadow/ltc_code at commit 31e5e96b54f98f33098f8503003119ba2231a1c6:
//
//   Copyright (c) 2017, Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt. All rights reserved.
//   The BSD 2-Clause terms with a third condition, a reference to the paper; the whole text is in ThirdPartyNotices.md.
//
//   Real-Time Polygonal-Light Shading with Linearly Transformed Cosines.
//   Eric Heitz, Jonathan Dupuy, Stephen Hill and David Neubelt.
//   ACM Transactions on Graphics (Proceedings of ACM SIGGRAPH 2016) 35(4), 2016.
//
// Outside ST_AREA_LIGHT_SHADING only plain values are used (no table, no scene resource): the exact clipped form factor, the
// nearest point and the range window, which passes without the scene set (the GI's light injection) can include. The forward
// pass defines ST_AREA_LIGHT_SHADING for the table (scene SRV 56), the shapes (SRV 57) and the shading.

#include "Include/Common.slang"

// RenderData::LightShape: the face of u_Lights' light of the same index (zero for point and spot lights).
struct LightShape
{
	float4 AxisX;      // xyz: the face's unit X axis, w: its half width (meters)
	float4 AxisY;      // xyz: its unit Y axis (cross(AxisX, the normal)), w: its half height
	float4 Parameters; // x: 1 when two-sided, y: the face's area in m^2, zw: 0
};

// A rect light as the shading takes it (AreaLights::AreaLight): its face is Center +- AxisX +- AxisY (half axes, the sizes in
// them) and shines toward Normal (unit). AxisY points along cross(AxisX, Normal), so the corners Center - AxisX - AxisY,
// + AxisX - AxisY, + AxisX + AxisY, - AxisX + AxisY run clockwise seen from the front: the order the edge sums count as positive.
struct AreaLight
{
	float3 Center;
	float3 Normal;
	float3 AxisX;
	float3 AxisY;
	float3 Radiance; // The face's luminance per channel: the color times nits
	float InverseRangeSquared;
	bool TwoSided;   // Both faces shine, each with Radiance
};

// Below these an edge's ends or its angle's sine count as zero: far under anything a scene's sizes give, and their inverses
// stay finite.
static const float c_AreaMinLengthProduct = 1.0e-30;
static const float c_AreaMinSineSquared = 1.0e-30;

// HLSL's saturate: a NaN gives 0 (written out: SPIR-V's clamp leaves a NaN's result undefined).
float SaturateArea(float value)
{
	return value > 0.0 ? min(value, 1.0) : 0.0;
}

// The fit of theta / (2 pi sin(theta)) for an angle of at most a right angle, from its cosine (in [0, 1]).
float GetAreaEdgeFit(float cosine)
{
	float numerator = 0.8543985 + (0.4965155 + 0.0145206 * cosine) * cosine;
	float denominator = 3.4175940 + (4.1616724 + cosine) * cosine;
	return numerator / denominator;
}

// An edge's share of the vector form factor: (the angle it is seen under) / (2 pi) along cross(start, end). `crossProduct` is
// cross(start, end) from the caller, who knows the edge itself: for a small, distant light the two ends are almost parallel
// and their own cross product would be mostly rounding.
float3 IntegrateAreaEdge(float3 start, float3 end, float3 crossProduct)
{
	float inverse = 1.0 / max(length(start) * length(end), c_AreaMinLengthProduct);
	float cosine = dot(start, end) * inverse;
	float3 sine = crossProduct * inverse; // Its length is the angle's sine
	float fit = GetAreaEdgeFit(abs(cosine));
	// Past a right angle the angle is pi minus the one the fit covers; the sine comes from the cross product, which stays
	// exact close to a straight angle where 1 - cosine^2 would not.
	float weight = cosine > 0.0 ? fit : 0.5 / sqrt(max(dot(sine, sine), c_AreaMinSineSquared)) - fit;
	return sine * weight;
}

// 1 in front of the light, -1 behind a two-sided one (where the corners' order is reversed and with it the sign of every edge
// sum) and 0 where it lights nothing: behind a one-sided light and on its plane.
float GetAreaLightSide(float3 position, AreaLight light)
{
	float distance = dot(light.Normal, position - light.Center);
	if (distance > 0.0)
		return 1.0;
	return light.TwoSided && distance < 0.0 ? -1.0 : 0.0;
}

// The vector form factor of the face's part above the receiver's horizon, seen from the origin: its corners, the exact edge that
// leaves each one and the corners' heights over the horizon (AreaLights.cpp's IntegratePolygonAbove). The edges above the horizon,
// cut where they cross it, and an edge along the horizon that closes each stretch of the outline under it; with every corner above
// the horizon it is the four edges' sum, with none 0. The heights may be the corners' before a linear transform took them where
// they are: the result is then the transformed image's sum of the part above the untransformed horizon.
float3 IntegrateAreaPolygonAbove(float3 corners[4], float3 edges[4], float heights[4])
{
	float3 sum = float3(0.0);
	float3 exitPoint = float3(0.0);  // Where the outline last went under, while no entry has closed it
	float3 firstEntry = float3(0.0); // An entry met before any exit: it follows the last exit, around the outline's start
	bool exitOpen = false;
	[unroll]
	for (uint index = 0u; index < 4u; index++)
	{
		uint next = (index + 1u) % 4u;
		float3 start = corners[index];
		float3 end = corners[next];
		float startHeight = heights[index];
		float endHeight = heights[next];
		float3 crossProduct = cross(start, edges[index]);
		if (startHeight > 0.0 && endHeight > 0.0)
		{
			sum += IntegrateAreaEdge(start, end, crossProduct);
		}
		else if (startHeight > 0.0)
		{
			exitPoint = end * startHeight - start * endHeight;
			exitOpen = true;
			sum += IntegrateAreaEdge(start, exitPoint, crossProduct * startHeight);
		}
		else if (endHeight > 0.0)
		{
			float3 entry = start * endHeight - end * startHeight;
			sum += IntegrateAreaEdge(entry, end, crossProduct * endHeight);
			if (exitOpen)
			{
				sum += IntegrateAreaEdge(exitPoint, entry, cross(exitPoint, entry));
				exitOpen = false;
			}
			else
			{
				firstEntry = entry;
			}
		}
	}
	if (exitOpen)
		sum += IntegrateAreaEdge(exitPoint, firstEntry, cross(exitPoint, firstEntry));
	return sum;
}

// The face's corners from a point at the origin, through its center and half axes, in AreaLight's order, and the exact edge that
// leaves each one.
void MakeAreaPolygon(float3 center, float3 axisX, float3 axisY, out float3 corners[4], out float3 edges[4])
{
	corners[0] = center - axisX - axisY;
	corners[1] = center + axisX - axisY;
	corners[2] = center + axisX + axisY;
	corners[3] = center - axisX + axisY;
	edges[0] = axisX * 2.0;
	edges[1] = axisY * 2.0;
	edges[2] = axisX * -2.0;
	edges[3] = axisY * -2.0;
}

// The form factor of the light seen from a surface point with its face clipped by the receiver's horizon exactly (no table):
// what AreaLights::FormFactorClipped computes, and the shading's diffuse light.
float AreaLightFormFactorClipped(float3 position, float3 normal, AreaLight light)
{
	float side = GetAreaLightSide(position, light);
	if (side == 0.0)
		return 0.0;
	float3 corners[4];
	float3 edges[4];
	MakeAreaPolygon(light.Center - position, light.AxisX, light.AxisY, corners, edges);
	float heights[4];
	[unroll]
	for (uint corner = 0u; corner < 4u; corner++)
		heights[corner] = dot(corners[corner], normal);
	return clamp(dot(IntegrateAreaPolygonAbove(corners, edges, heights), normal) * side, 0.0, 1.0);
}

// The point of the light's face nearest to `position` (the axes are perpendicular, so the nearest along each).
float3 ClosestPointOnAreaLight(float3 position, AreaLight light)
{
	float3 offset = position - light.Center;
	float x = dot(offset, light.AxisX) / max(dot(light.AxisX, light.AxisX), c_AreaMinLengthProduct);
	float y = dot(offset, light.AxisY) / max(dot(light.AxisY, light.AxisY), c_AreaMinLengthProduct);
	return light.Center + light.AxisX * clamp(x, -1.0, 1.0) + light.AxisY * clamp(y, -1.0, 1.0);
}

// The light's reach: 1 at the face, 0 from its range on, (1 - (d^2 / range^2)^2)^2 at the distance d from the face (local
// lights' window without the inverse square: the form factor holds the distance already).
float GetAreaLightRangeWindow(float3 position, AreaLight light)
{
	float3 offset = position - ClosestPointOnAreaLight(position, light);
	float ratio = dot(offset, offset) * light.InverseRangeSquared;
	float window = clamp(1.0 - ratio * ratio, 0.0, 1.0);
	return window * window;
}

#ifdef ST_AREA_LIGHT_SHADING

#include "Include/Scene.slang"
#include "Include/PBR.slang"

// The LTC tables (Renderer::GetAreaLightTable, Strata/Renderer/LTCTable.inl): two layers of 64 x 64 RGBA32F texels, read with
// Load and bilinear weights computed here (32-bit float filtering is optional on devices, and the sampler's 8-bit weights
// would be too coarse). Layer 0 texel (x, y, z, w) is the lobe's inverse transform of rows (x, 0, z), (0, 1, 0), (y, 0, w) in
// the frame (T1, T2, N); layer 1's w is the horizon term (x and y, the lobe's magnitude and Fresnel term, come from u_BRDFLut).
Texture2DArray<float4> u_AreaLightTable : register(t56);
// The lights' faces, parallel to u_Lights by index (RenderData::LightShape).
StructuredBuffer<LightShape> u_LightShapes : register(t57);

#define ST_AREA_TABLE_SIZE 64u
#define ST_AREA_TABLE_TRANSFORM 0u
#define ST_AREA_TABLE_TERMS 1u

// Under this squared length the view's direction along the surface is too short to orient the lobe's frame (the view is
// within 1e-4 radians of the normal, where the lobe is isotropic anyway).
static const float c_AreaMinTangentLengthSquared = 1.0e-8;

// A layer between its texels: `uv` 0 is the first texel's center and 1 the last one's, values outside are clamped (a NaN reads
// 0). Four loads weighted bilinearly (AreaLights::SampleTable).
float4 SampleAreaLightTable(uint layer, float2 uv)
{
	float last = float(ST_AREA_TABLE_SIZE - 1u);
	float2 texel = float2(SaturateArea(uv.x), SaturateArea(uv.y)) * last;
	float2 base = floor(texel);
	float2 weight = texel - base;
	int x0 = int(base.x);
	int y0 = int(base.y);
	int x1 = min(x0 + 1, int(ST_AREA_TABLE_SIZE - 1u));
	int y1 = min(y0 + 1, int(ST_AREA_TABLE_SIZE - 1u));
	float4 t00 = u_AreaLightTable.Load(int4(x0, y0, int(layer), 0));
	float4 t10 = u_AreaLightTable.Load(int4(x1, y0, int(layer), 0));
	float4 t01 = u_AreaLightTable.Load(int4(x0, y1, int(layer), 0));
	float4 t11 = u_AreaLightTable.Load(int4(x1, y1, int(layer), 0));
	float4 upper = t00 + (t10 - t00) * weight.x;
	float4 lower = t01 + (t11 - t01) * weight.x;
	return upper + (lower - upper) * weight.y;
}

// The form factor a vector form factor comes to once the horizon has cut it: the table's term for a sphere cap of the same
// vector form factor (AreaLights::ClipToHorizon). `alongNormal` is the vector's part along the receiver's normal. The shading
// takes it for the transformed horizon of the specular lobe only: at the receiver's own horizon it clips exactly.
float ClipAreaToHorizon(float3 vectorFormFactor, float alongNormal)
{
	float vectorLength = length(vectorFormFactor);
	if (!(vectorLength > 0.0))
		return 0.0;
	float cosine = alongNormal / vectorLength;
	float share = SampleAreaLightTable(ST_AREA_TABLE_TERMS, float2(cosine * 0.5 + 0.5, vectorLength)).w;
	return min(vectorLength * share, 1.0);
}

// A tangent of the normal that does not depend on the view (Include/IBL.slang's TangentBasis).
float3 GetAreaFixedTangent(float3 normal)
{
	float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
	return normalize(cross(up, normal));
}

// A vector in the lobe's frame (tangent, bitangent, normal) through the inverse transform `transform` (a texel of layer 0).
float3 TransformAreaVector(float4 transform, float3 tangent, float3 bitangent, float3 normal, float3 vector)
{
	float3 local = float3(dot(tangent, vector), dot(bitangent, vector), dot(normal, vector));
	return float3(transform.x * local.x + transform.z * local.z, local.y, transform.y * local.x + transform.w * local.z);
}

// The share of the clamped cosine lobe the light covers, for the lobe that `transform` turns into the cosine around `normal`, over
// the face's part above the receiver's horizon; `view` orients the transform's frame (AreaLights::GetLobeCoverage).
float GetAreaLobeCoverage(float4 transform, float3 position, float3 normal, float3 view, AreaLight light)
{
	float side = GetAreaLightSide(position, light);
	if (side == 0.0)
		return 0.0;
	float3 tangent = view - normal * dot(view, normal);
	float tangentLengthSquared = dot(tangent, tangent);
	if (tangentLengthSquared < c_AreaMinTangentLengthSquared)
		tangent = GetAreaFixedTangent(normal);
	else
		tangent *= 1.0 / sqrt(tangentLengthSquared);
	float3 bitangent = cross(normal, tangent);
	// GGX reflects nothing from under the receiver's horizon, but the fitted lobe keeps a part of itself there on the viewer's
	// side: the face is cut at the receiver's horizon (the corners' heights before the transform) before the transformed
	// horizon's term, so a face under the horizon gives nothing and one sinking under it fades out.
	float3 corners[4];
	float3 edges[4];
	MakeAreaPolygon(light.Center - position, light.AxisX, light.AxisY, corners, edges);
	float heights[4];
	[unroll]
	for (uint corner = 0u; corner < 4u; corner++)
		heights[corner] = dot(corners[corner], normal);
	// The transform is linear, so the face stays a parallelogram: its center and its two half axes are transformed.
	float3 center = TransformAreaVector(transform, tangent, bitangent, normal, light.Center - position);
	float3 axisX = TransformAreaVector(transform, tangent, bitangent, normal, light.AxisX);
	float3 axisY = TransformAreaVector(transform, tangent, bitangent, normal, light.AxisY);
	MakeAreaPolygon(center, axisX, axisY, corners, edges);
	float3 vectorFormFactor = IntegrateAreaPolygonAbove(corners, edges, heights) * side;
	return ClipAreaToHorizon(vectorFormFactor, vectorFormFactor.z);
}

// The share of the specular lobe of perceptual `roughness` the light covers (AreaLights::SpecularFraction).
float AreaSpecularFraction(float3 position, float3 normal, float3 view, float roughness, AreaLight light)
{
	float cosine = SaturateArea(dot(normal, view));
	float2 uv = float2(max(roughness, c_MinRoughness), sqrt(max(1.0 - cosine, 0.0)));
	return GetAreaLobeCoverage(SampleAreaLightTable(ST_AREA_TABLE_TRANSFORM, uv), position, normal, view, light);
}

// The light buffer's rect light with its face (ST_LIGHT_RECT): the half sizes go into the axes.
AreaLight LoadAreaLight(LightData light, LightShape shape)
{
	AreaLight area;
	area.Center = light.PositionRange.xyz;
	area.Normal = light.DirectionType.xyz;
	area.AxisX = shape.AxisX.xyz * shape.AxisX.w;
	area.AxisY = shape.AxisY.xyz * shape.AxisY.w;
	area.Radiance = light.Color.rgb;
	area.InverseRangeSquared = light.Color.w;
	area.TwoSided = shape.Parameters.x > 0.5;
	return area;
}

// The radiance a surface reflects toward the viewer from the light, unshadowed, faded by the light's range (AreaLights::Shade):
// 0 behind a one-sided light, on its plane and out of its range. Diffuse gets what the specular lobe reflects not (as image
// light's), the highlight is the lobe's covered share times its reflectance and compensation, and a clear coat covers both by
// what it reflects and adds its own lobe's share. A `specularWeight` of 0 (a reflection probe's capture) evaluates no lobe.
float3 ShadeAreaLight(SurfaceData surface, ClearcoatData coat, float2 coatDFG, float2 dfg, float diffuseWeight, AreaLight light, float specularWeight)
{
	if (GetAreaLightSide(surface.Position, light) == 0.0)
		return float3(0.0);
	float window = GetAreaLightRangeWindow(surface.Position, light);
	if (!(window > 0.0))
		return float3(0.0);

	float3 reflectance = surface.F0 * dfg.x + dfg.y;
	float formFactor = AreaLightFormFactorClipped(surface.Position, surface.Normal, light);
	float3 result = surface.Albedo * ((1.0 - surface.Metallic) * formFactor * diffuseWeight) * (float3(1.0) - reflectance);
	if (specularWeight > 0.0)
	{
		float fraction = AreaSpecularFraction(surface.Position, surface.Normal, surface.View, surface.Roughness, light);
		result += reflectance * surface.SpecularEnergy * (fraction * specularWeight);
	}
	if (coat.Strength > 0.0)
	{
		float coatReflectance = coat.Strength * (c_ClearcoatF0 * coatDFG.x + coatDFG.y) * coat.SpecularEnergy;
		result *= 1.0 - coatReflectance;
		if (specularWeight > 0.0)
			result += float3(AreaSpecularFraction(surface.Position, coat.Normal, surface.View, coat.Roughness, light) * coatReflectance * specularWeight);
	}
	return light.Radiance * (result * window);
}

#endif

#endif
