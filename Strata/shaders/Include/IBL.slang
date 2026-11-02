#ifndef STRATA_IBL_SLANG
#define STRATA_IBL_SLANG

// Helpers for image-based lighting precomputation.

#include "Include/Common.slang"

// World direction through the center of texel `texel` of cube face `face` (+X, -X, +Y, -Y, +Z, -Z; Vulkan/D3D
// cube conventions with texel row 0 at the top of each face).
float3 CubeTexelDirection(uint2 texel, uint face, uint size)
{
	float2 st = (float2(texel) + 0.5) / float(size) * 2.0 - 1.0;
	float3 direction;
	switch (face)
	{
		case 0u: direction = float3(1.0, -st.y, -st.x); break;
		case 1u: direction = float3(-1.0, -st.y, st.x); break;
		case 2u: direction = float3(st.x, 1.0, st.y); break;
		case 3u: direction = float3(st.x, -1.0, -st.y); break;
		case 4u: direction = float3(st.x, -st.y, 1.0); break;
		default: direction = float3(-st.x, -st.y, -1.0); break;
	}
	return normalize(direction);
}

// Equirectangular (latitude-longitude) coordinates of a direction: u around +Y starting at -X, v from +Y (top row)
// to -Y (bottom row).
float2 DirectionToEquirect(float3 direction)
{
	// atan(0, 0) is undefined: straight up or down, where every column is the same direction, the middle one is read.
	float u = (direction.x != 0.0 || direction.z != 0.0 ? atan2(direction.z, direction.x) : 0.0) / TWO_PI + 0.5;
	float v = acos(clamp(direction.y, -1.0, 1.0)) / PI;
	return float2(u, v);
}

float2 Hammersley(uint index, uint count)
{
	return float2(float(index) / float(count), float(reversebits(index)) * 2.3283064365386963e-10);
}

// Tangent frame around a normal.
float3x3 TangentBasis(float3 normal)
{
	float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
	float3 tangent = normalize(cross(up, normal));
	float3 bitangent = cross(normal, tangent);
	return transpose(float3x3(tangent, bitangent, normal));
}

// GGX-distributed half vector around the normal (alpha = roughness^2).
float3 ImportanceSampleGGX(float2 xi, float3 normal, float alpha)
{
	float phi = TWO_PI * xi.x;
	float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
	return mul(TangentBasis(normal), float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta));
}

float3 SampleCosineHemisphere(float2 xi, float3 normal)
{
	float phi = TWO_PI * xi.x;
	float cosTheta = sqrt(1.0 - xi.y);
	float sinTheta = sqrt(xi.y);
	return mul(TangentBasis(normal), float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta));
}

#endif
