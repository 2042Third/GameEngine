#ifndef STRATA_IBL_GLSL
#define STRATA_IBL_GLSL

// Helpers for image-based lighting precomputation.

#include "Include/Common.glsl"

// World direction through the center of texel `texel` of cube face `face` (+X, -X, +Y, -Y, +Z, -Z; Vulkan/D3D
// cube conventions with texel row 0 at the top of each face).
vec3 CubeTexelDirection(uvec2 texel, uint face, uint size)
{
	vec2 st = (vec2(texel) + 0.5) / float(size) * 2.0 - 1.0;
	vec3 direction;
	switch (face)
	{
		case 0u: direction = vec3(1.0, -st.y, -st.x); break;
		case 1u: direction = vec3(-1.0, -st.y, st.x); break;
		case 2u: direction = vec3(st.x, 1.0, st.y); break;
		case 3u: direction = vec3(st.x, -1.0, -st.y); break;
		case 4u: direction = vec3(st.x, -st.y, 1.0); break;
		default: direction = vec3(-st.x, -st.y, -1.0); break;
	}
	return normalize(direction);
}

// Equirectangular (latitude-longitude) coordinates of a direction: u around +Y starting at -X, v from +Y (top row)
// to -Y (bottom row).
vec2 DirectionToEquirect(vec3 direction)
{
	float u = atan(direction.z, direction.x) / TWO_PI + 0.5;
	float v = acos(clamp(direction.y, -1.0, 1.0)) / PI;
	return vec2(u, v);
}

vec2 Hammersley(uint index, uint count)
{
	return vec2(float(index) / float(count), float(bitfieldReverse(index)) * 2.3283064365386963e-10);
}

// Tangent frame around a normal.
mat3 TangentBasis(vec3 normal)
{
	vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, normal));
	vec3 bitangent = cross(normal, tangent);
	return mat3(tangent, bitangent, normal);
}

// GGX-distributed half vector around the normal (alpha = roughness^2).
vec3 ImportanceSampleGGX(vec2 xi, vec3 normal, float alpha)
{
	float phi = TWO_PI * xi.x;
	float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
	return TangentBasis(normal) * vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

vec3 SampleCosineHemisphere(vec2 xi, vec3 normal)
{
	float phi = TWO_PI * xi.x;
	float cosTheta = sqrt(1.0 - xi.y);
	float sinTheta = sqrt(xi.y);
	return TangentBasis(normal) * vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

#endif
