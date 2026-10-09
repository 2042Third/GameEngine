#ifndef STRATA_COMMON_GLSL
#define STRATA_COMMON_GLSL

// Descriptor binding numbers follow NVRHI's Vulkan register mapping: each resource class has its own
// binding range within a descriptor set (set index = position of the binding set in the pipeline state).
#define ST_SRV(slot)     (slot)        // Sampled textures, read-only storage buffers
#define ST_SAMPLER(slot) (128 + (slot))
#define ST_CBV(slot)     (256 + (slot)) // Uniform (constant) buffers
#define ST_UAV(slot)     (384 + (slot)) // Storage images, read-write storage buffers

const float PI = 3.14159265358979323846;
const float TWO_PI = 6.28318530717958647692;
const float INV_PI = 0.31830988618379067154;

// Fullscreen-pass UV from NDC. NVRHI flips the Vulkan viewport to the D3D convention: NDC +Y is up and
// texture/framebuffer row 0 is at the top.
vec2 NDCToUV(vec2 ndc)
{
	return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

vec2 UVToNDC(vec2 uv)
{
	return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

// sRGB transfer functions (IEC 61966-2-1).
vec3 LinearToSRGB(vec3 color)
{
	color = max(color, vec3(0.0));
	vec3 low = color * 12.92;
	vec3 high = 1.055 * pow(color, vec3(1.0 / 2.4)) - 0.055;
	return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
}

vec3 SRGBToLinear(vec3 color)
{
	vec3 low = color / 12.92;
	vec3 high = pow((color + 0.055) / 1.055, vec3(2.4));
	return mix(high, low, lessThanEqual(color, vec3(0.04045)));
}

#endif
