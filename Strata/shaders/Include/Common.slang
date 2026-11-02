#ifndef STRATA_COMMON_SLANG
#define STRATA_COMMON_SLANG

// Bindings are HLSL registers: register(tN) sampled textures and read-only buffers, sN samplers, bN constant buffers, uN
// storage images and read-write buffers; spaceS is descriptor set S, the binding set's position in the pipeline state. The
// build shifts each class to NVRHI's Vulkan ranges, t +0, s +128, b +256, u +384 (VulkanBindingOffsets,
// CMake/StrataShaders.cmake).

static const float PI = 3.14159265358979323846;
static const float TWO_PI = 6.28318530717958647692;
static const float INV_PI = 0.31830988618379067154;

// Fullscreen-pass UV from NDC. NVRHI flips the Vulkan viewport to the D3D convention: NDC +Y is up and
// texture/framebuffer row 0 is at the top.
float2 NDCToUV(float2 ndc)
{
	return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float2 UVToNDC(float2 uv)
{
	return float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
}

// sRGB transfer functions (IEC 61966-2-1).
float3 LinearToSRGB(float3 color)
{
	color = max(color, float3(0.0));
	float3 low = color * 12.92;
	float3 high = 1.055 * pow(color, float3(1.0 / 2.4)) - 0.055;
	return select((color <= float3(0.0031308)), low, high);
}

float3 SRGBToLinear(float3 color)
{
	float3 low = color / 12.92;
	float3 high = pow((color + 0.055) / 1.055, float3(2.4));
	return select((color <= float3(0.04045)), low, high);
}

// Per-pixel noise in [0, 1) whose neighbors differ evenly (interleaved gradient noise): screen-space reflections' start
// offsets, turned each frame by TemporalParams.z.
float InterleavedGradientNoise(float2 pixel)
{
	return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Largest value written to half-float color targets (the format's maximum is 65504; anything above becomes Inf).
static const float c_MaxHDRValue = 65000.0;

// Makes a linear color safe to store and process: NaN becomes 0, negative values 0 and overflowing ones (Inf
// included) c_MaxHDRValue. Inf or NaN in an HDR target would turn black or spread through the post-processing.
float3 SanitizeHDR(float3 color)
{
	color = select(isnan(color), float3(0.0), color);
	return clamp(color, float3(0.0), float3(c_MaxHDRValue));
}

// --- What the conversion from GLSL (wave 8) needs ---

// GLSL's stage variables as private globals, which each converted entry point fills before calling its st_Main. New shaders
// take these as entry-point parameters (Docs/DeveloperGuide.md, "Shaders").
static float4 st_FragCoord;
static bool st_FrontFacing;
static float st_FragDepth;
static float4 st_Position;
static int st_VertexIndex;
static int st_InstanceIndex;
static uint3 st_GlobalInvocationID;
static uint3 st_LocalInvocationID;
static uint3 st_WorkGroupID;
static uint st_LocalInvocationIndex;

// GLSL's expression forms that HLSL has only as methods with out parameters, and what HLSL lacks.
int2 TextureSize(Texture2D<float4> texture, int level)
{
	uint width, height, levels;
	texture.GetDimensions(level, width, height, levels);
	return int2(width, height);
}

int2 TextureSize(Texture2D<uint4> texture, int level)
{
	uint width, height, levels;
	texture.GetDimensions(level, width, height, levels);
	return int2(width, height);
}

int3 TextureSize(Texture2DArray<float4> texture, int level)
{
	uint width, height, elements, levels;
	texture.GetDimensions(level, width, height, elements, levels);
	return int3(width, height, elements);
}

int3 TextureSize(Texture2DArray<uint4> texture, int level)
{
	uint width, height, elements, levels;
	texture.GetDimensions(level, width, height, elements, levels);
	return int3(width, height, elements);
}

int3 TextureSize(Texture3D<float4> texture, int level)
{
	uint width, height, depth, levels;
	texture.GetDimensions(level, width, height, depth, levels);
	return int3(width, height, depth);
}

int2 TextureSize(TextureCube<float4> texture, int level)
{
	uint width, height, levels;
	texture.GetDimensions(level, width, height, levels);
	return int2(width, height);
}

int TextureLevels(Texture2D<float4> texture)
{
	uint width, height, levels;
	texture.GetDimensions(0, width, height, levels);
	return int(levels);
}

int TextureLevels(TextureCube<float4> texture)
{
	uint width, height, levels;
	texture.GetDimensions(0, width, height, levels);
	return int(levels);
}

int TextureLevels(Texture2DArray<float4> texture)
{
	uint width, height, elements, levels;
	texture.GetDimensions(0, width, height, elements, levels);
	return int(levels);
}

int2 ImageSize(RWTexture2D<float4> image)
{
	uint width, height;
	image.GetDimensions(width, height);
	return int2(width, height);
}

int3 ImageSize(RWTexture2DArray<float4> image)
{
	uint width, height, elements;
	image.GetDimensions(width, height, elements);
	return int3(width, height, elements);
}

// Sources declared with two types under #ifdef (the same name, a texture or an array): one form for both.
float4 TexelFetch(Texture2D<float4> texture, int2 position, int level)
{
	return texture.Load(int3(position, level));
}

float4 TexelFetch(Texture2DArray<float4> texture, int3 position, int level)
{
	return texture.Load(int4(position, level));
}

uint4 TexelFetch(Texture2D<uint4> texture, int2 position, int level)
{
	return texture.Load(int3(position, level));
}

uint4 TexelFetch(Texture2DArray<uint4> texture, int3 position, int level)
{
	return texture.Load(int4(position, level));
}

float4 UnpackUnorm4x8(uint value)
{
	return float4(float(value & 255u), float((value >> 8) & 255u), float((value >> 16) & 255u), float(value >> 24)) / 255.0;
}

// GLSL's umulExtended: HLSL has no 32 x 32 to 64 multiplication without 64-bit integers.
void UnsignedMultiplyExtended(uint a, uint b, out uint high, out uint low)
{
	uint a0 = a & 65535u;
	uint a1 = a >> 16;
	uint b0 = b & 65535u;
	uint b1 = b >> 16;
	uint p00 = a0 * b0;
	uint p01 = a0 * b1;
	uint p10 = a1 * b0;
	uint p11 = a1 * b1;
	uint middle = (p00 >> 16) + (p01 & 65535u) + (p10 & 65535u);
	low = (p00 & 65535u) | (middle << 16);
	high = p11 + (p01 >> 16) + (p10 >> 16) + (middle >> 16);
}

// Slang lowers f32tof16 and f16tof32 to 16-bit floats on SPIR-V (Float16, which the engine's device does not enable).
uint PackHalf2x16(float2 value)
{
	__target_switch
	{
	case spirv:
		return spirv_asm { result:$$uint = OpExtInst glsl450 PackHalf2x16 $value };
	default:
		return f32tof16(value.x) | (f32tof16(value.y) << 16);
	}
}

float2 UnpackHalf2x16(uint value)
{
	__target_switch
	{
	case spirv:
		return spirv_asm { result:$$float2 = OpExtInst glsl450 UnpackHalf2x16 $value };
	default:
		return float2(f16tof32(value & 65535u), f16tof32(value >> 16));
	}
}

#endif
