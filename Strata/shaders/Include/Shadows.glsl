#ifndef STRATA_SHADOWS_GLSL
#define STRATA_SHADOWS_GLSL

// Directional light shadows: cascaded shadow maps (reversed-Z: larger depth = closer to the light) filtered as the sun's
// disk shades (Docs/DeveloperGuide.md, "Sun shadows"). Each cascade layer has a conservative pyramid of depth ranges
// (u_ShadowRanges, Scene/ShadowRanges.comp): the cells around a receiver classify it as lit or in umbra without a single
// comparison, and otherwise estimate the blockers' distance, whose penumbra is filtered at the physical half-width
// separation * tan(the sun's angular radius): a fixed tent of four bilinear comparisons where the penumbra is narrower than
// about a texel, and a Vogel disk of ShadowBias.z taps turned by a fixed blue-noise tile where it is wider. Nothing turns
// with the frame, so a still view is the same in every frame. Each cascade blends into the next one over a band before its
// split (the last one fades out). Acne is prevented by the rasterizer's slope bias (shadow pass), a constant depth bias and a
// normal offset (ShadowBias), and a receiver plane depth bias that follows the receiver's slope across the filter region.
// Every shader that includes this file enables GL_EXT_samplerless_texture_functions and GL_EXT_texture_shadow_lod.

#include "Include/BlueNoise.glsl"
#include "Include/Scene.glsl"

layout(set = 0, binding = ST_SRV(7)) uniform texture2DArray u_ShadowMap;
// Per cascade layer, three levels of cells of 4, 8 and 16 shadow texels each way: the nearest depth (the largest, toward the
// light) rounded up in the high 16 bits, the farthest rounded down in the low 16 bits.
layout(set = 0, binding = ST_SRV(32)) uniform utexture2DArray u_ShadowRanges;
layout(set = 0, binding = ST_SAMPLER(9)) uniform samplerShadow u_ShadowCompareSampler;

const int c_ShadowRangeLevels = 3;
const float c_MaxShadowSearchTexels = 31.0; // Keeps a search region within 5 x 5 cells of its level
const float c_GoldenAngle = 2.39996323;
// How far inside its square's edges a cascade holds a receiver (ShadowFitTracker::c_FilterReachTexels): the filter's reach,
// so a receiver it holds is filtered from texels of its square alone. The last cascade holds receivers to its edge.
const float c_ShadowCascadeMarginTexels = 33.0;

// Whether a cascade holds a camera-relative position: within its square by `margin` (in uv) and within its depth range.
bool ShadowCascadeHolds(int cascade, vec3 relativePosition, float margin)
{
	vec4 lightClip = u_Cascades[cascade].ViewProjection * vec4(relativePosition, 1.0);
	vec3 lightPosition = lightClip.xyz / lightClip.w;
	vec2 uv = NDCToUV(lightPosition.xy);
	return all(greaterThanEqual(uv, vec2(margin))) && all(lessThanEqual(uv, vec2(1.0 - margin))) && lightPosition.z >= 0.0 && lightPosition.z <= 1.0;
}

bool ShadowCascadeHolds(int cascade, vec3 relativePosition)
{
	bool last = cascade + 1 >= int(u_Frame.ShadowParams.w);
	return ShadowCascadeHolds(cascade, relativePosition, last ? 0.0 : c_ShadowCascadeMarginTexels * u_Frame.ShadowParams.y);
}

// The cascade that shades a camera-relative position at a view depth, by containment: from the first cascade whose split lies
// beyond the depth, the first one that holds the position (ShadowCascadeHolds); -1 when none does. The cascades' squares may be
// fitted to the receivers a readback saw (SceneRenderer::FindShadowCasters, ShadowFitTracker): one the readback missed (a fast
// turn, something that just appeared) falls to a coarser cascade, at last to the last one's sphere, which reaches the shadow
// distance, until the next readback fits it. Every shader that selects a cascade calls this; none selects by depth alone.
int SelectShadowCascade(float viewDepth, vec3 relativePosition)
{
	int cascadeCount = int(u_Frame.ShadowParams.w);
	for (int cascade = 0; cascade < cascadeCount; cascade++)
	{
		if (viewDepth < u_Frame.CascadeSplits[cascade] && ShadowCascadeHolds(cascade, relativePosition))
			return cascade;
	}
	return -1;
}

// How far a pixel is into the band where its cascade hands over to the next one (the last one fades out): 0 before the
// band, 1 at the split. The band is the last ShadowParams.z of the cascade's depth range, from the previous split (the
// first cascade's from the camera, at 0), as SceneRenderer::FindShadowCasters lays it out: the next cascade's slice starts
// at the band, so its square covers it unless it is fitted to receivers elsewhere; then the cascade does not blend into it
// (0), and a position beyond the split falls to the next cascade that holds it.
float CascadeBlendWeight(int cascade, float viewDepth, vec3 relativePosition)
{
	float split = u_Frame.CascadeSplits[cascade];
	float previousSplit = cascade > 0 ? u_Frame.CascadeSplits[cascade - 1] : 0.0;
	float band = u_Frame.ShadowParams.z * (split - previousSplit);
	if (!(band > 0.0))
		return 0.0;
	float weight = clamp((viewDepth - (split - band)) / band, 0.0, 1.0);
	if (weight > 0.0 && cascade + 1 < int(u_Frame.ShadowParams.w) && !ShadowCascadeHolds(cascade + 1, relativePosition))
		return 0.0;
	return weight;
}

// Receiver plane depth bias: the gradient of the receiver's light-space depth over the shadow map's uv, from the plane
// through the receiver with its normal (in the cascade's orthographic light clip space the plane's normal is the world
// normal through the inverse transpose of the projection, the cofactor matrix up to scale). Filter samples are compared
// against the receiver's own depth at their offset rather than at the center, which would let a surface tilted toward
// the light shadow itself wherever the filter reaches past the depth bias (acne at grazing light angles). Screen-space
// derivatives would give the plane too, but they are wrong at silhouettes and depend on how the GPU groups pixels of
// adjacent triangles into quads, which changes with the order of the draws. Zero for planes along the light.
vec2 ReceiverDepthGradient(mat4 cascadeViewProjection, vec3 normal)
{
	vec3 column0 = cascadeViewProjection[0].xyz;
	vec3 column1 = cascadeViewProjection[1].xyz;
	vec3 column2 = cascadeViewProjection[2].xyz;
	vec3 lightNormal = cross(column1, column2) * normal.x + cross(column2, column0) * normal.y + cross(column0, column1) * normal.z;
	if (abs(lightNormal.z) < 1e-6 * length(lightNormal))
		return vec2(0.0);
	// On the plane, depth changes by -(n.x dx + n.y dy) / n.z; uv = (0.5 x + 0.5, 0.5 - 0.5 y), so dx = 2 du and dy = -2 dv.
	return vec2(-2.0 * lightNormal.x, 2.0 * lightNormal.y) / lightNormal.z;
}

// A cell's depth range: x the nearest depth (toward the light), y the farthest.
vec2 UnpackShadowRange(uint packed)
{
	return vec2(float(packed >> 16u), float(packed & 0xFFFFu)) / 65535.0;
}

// The share of a bilinear 2x2 footprint of the cascade's layer that blocks the light at `reference` (1 = in shadow). An
// explicit level: the sun is shaded in non-uniform control flow (the classification returns early). Level 0 by LOD
// (GL_EXT_texture_shadow_lod), not by zero gradients: the same texels and weights (the map has one level, magnified either
// way), and the disk's taps cost less without the gradients each one would send.
float CompareShadowMap(int cascade, vec2 uv, float reference)
{
	return textureLod(sampler2DArrayShadow(u_ShadowMap, u_ShadowCompareSampler), vec4(uv, float(cascade), reference), 0.0);
}

// What the filter found in one cascade, or blended over two (EvaluateDirectionalShadow).
struct ShadowSample
{
	float Visibility;     // 1 = fully lit (also outside the cascade's square)
	float PenumbraTexels; // The disk's radius, half the penumbra's width, in texels of the cascade; 0 where none was found
	float DiskWeight;     // How much of the result came from the disk (0: the fixed tent alone)
	int Cascade;          // The cascade (the first one where two blend), -1 where none covers the position
};

// The sun's visibility at a camera-relative position with its geometric normal (facing the viewer) in one cascade, at a
// screen pixel (which picks the disk's fixed turn).
ShadowSample SampleCascadeShadow(int cascade, vec3 relativePosition, vec3 normal, ivec2 pixel)
{
	ShadowSample result = ShadowSample(1.0, 0.0, 0.0, cascade);
	// Normal offset: push the lookup position off the surface by about a shadow texel to avoid acne.
	vec4 cascadeData = u_Cascades[cascade].Parameters; // x: world units per texel, y: depth range, z: search distance (world units)
	mat4 cascadeViewProjection = u_Cascades[cascade].ViewProjection;
	vec3 offsetPosition = relativePosition + normal * cascadeData.x * u_Frame.ShadowBias.y;
	vec4 lightClip = cascadeViewProjection * vec4(offsetPosition, 1.0);
	vec3 lightPosition = lightClip.xyz / lightClip.w;
	vec2 uv = NDCToUV(lightPosition.xy);
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
		return result;
	float depthRange = max(cascadeData.y, 1e-6);
	float receiverDepth = lightPosition.z + u_Frame.ShadowBias.x * cascadeData.x / depthRange;

	// The receiver plane correction is limited to the search distance, so planes nearly along the light (curved receivers'
	// normals at their silhouettes) cannot move the comparison arbitrarily far.
	float texelSize = u_Frame.ShadowParams.y;
	vec2 texelPosition = uv / texelSize;
	vec2 depthPerTexel = ReceiverDepthGradient(cascadeViewProjection, normal) * texelSize;
	float slope = abs(depthPerTexel.x) + abs(depthPerTexel.y);
	float maxCorrection = cascadeData.z / depthRange;
	float worldPerTexel = max(cascadeData.x, 1e-6);

	// The search region: the half-width of the penumbra of a blocker at the search distance, at least the tent's support, and
	// a texel more for the bilinear footprints. Its cells come from the coarsest level that keeps it within 5 x 5 of them.
	float searchTexels = min(u_Frame.ShadowParams.x * cascadeData.z / worldPerTexel, c_MaxShadowSearchTexels);
	float regionTexels = max(searchTexels, 1.5) + 1.0;
	int level = regionTexels <= 8.0 ? 0 : (regionTexels <= 16.0 ? 1 : 2);
	float cellSize = float(4 << level);
	ivec2 lastLevelCell = textureSize(u_ShadowRanges, level).xy - 1;
	ivec2 firstCell = clamp(ivec2(floor((texelPosition - regionTexels) / cellSize)), ivec2(0), lastLevelCell);
	ivec2 lastCell = clamp(ivec2(floor((texelPosition + regionTexels) / cellSize)), ivec2(0), lastLevelCell);

	// Every comparison below reads texels inside these cells against a reference within receiverDepth +- spread, and the
	// ranges are rounded outward: a region whose nearest depth is at most the lowest reference is lit, one whose farthest is
	// beyond the highest is in umbra, exactly. A caster of any thinness inside raises the nearest depth: never missed.
	float spread = min(slope * regionTexels, maxCorrection);
	float cellSpread = min(slope * cellSize * 0.5, maxCorrection);
	float nearest = 0.0;
	float farthest = 1.0;
	float blockerSum = 0.0;
	float blockerWeight = 0.0;
	for (int cellY = firstCell.y; cellY <= lastCell.y; cellY++)
	{
		for (int cellX = firstCell.x; cellX <= lastCell.x; cellX++)
		{
			vec2 range = UnpackShadowRange(texelFetch(u_ShadowRanges, ivec3(cellX, cellY, cascade), level).r);
			nearest = max(nearest, range.x);
			farthest = min(farthest, range.y);
			// A cell blocks where its nearest depth is nearer than the receiver plane anywhere over it; its weight is its overlap
			// with the search square, which changes continuously as the receiver moves (no jumps as cells enter the region).
			vec2 cellMin = vec2(cellX, cellY) * cellSize;
			float cellReference = receiverDepth + clamp(dot(depthPerTexel, cellMin + 0.5 * cellSize - texelPosition), -maxCorrection, maxCorrection) + cellSpread;
			if (range.x > cellReference)
			{
				vec2 overlap = clamp(min(cellMin + cellSize, texelPosition + searchTexels) - max(cellMin, texelPosition - searchTexels), 0.0, cellSize) / cellSize;
				float weight = overlap.x * overlap.y;
				blockerSum += weight * (range.x - cellReference);
				blockerWeight += weight;
			}
		}
	}
	if (nearest <= receiverDepth - spread)
		return result; // Lit
	if (farthest > receiverDepth + spread)
	{
		result.Visibility = 0.0; // Umbra
		return result;
	}

	// The penumbra's half-width from the blockers' mean distance (biased toward wider by at most a cell's depth spread):
	// a straight edge under a disk of angular radius a falls from lit to shadow over 2 s tan(a), which filtering with a disk
	// of radius s tan(a) reproduces. No blocker weight (a softness of 0, or blockers only beside the search square): none.
	float penumbraTexels = 0.0;
	if (blockerWeight > 0.0)
	{
		float separation = blockerSum / blockerWeight * depthRange;
		penumbraTexels = min(separation * u_Frame.ShadowParams.x / worldPerTexel, searchTexels);
	}

	// The least filter: four bilinear comparisons at (+-0.5, +-0.5) texels, a separable trapezoid of +-1.5 texels (flat over
	// +-0.5), the same at every pixel: edges narrower than a texel are smooth and never dithered.
	float tent = 0.0;
	for (int tap = 0; tap < 4; tap++)
	{
		vec2 offset = vec2((tap & 1) == 0 ? -0.5 : 0.5, (tap & 2) == 0 ? -0.5 : 0.5);
		float reference = receiverDepth + clamp(dot(depthPerTexel, offset), -maxCorrection, maxCorrection);
		tent += 0.25 * CompareShadowMap(cascade, uv + offset * texelSize, reference);
	}
	float shadowed = tent;

	// Wider penumbrae: a Vogel disk (taps at radii sqrt((i + 0.5) / N), turned by the golden angle) turned per pixel by the
	// blue-noise tile, faded in from 1 to 2 texels of radius so the result stays continuous.
	float diskWeight = clamp(penumbraTexels - 1.0, 0.0, 1.0);
	if (diskWeight > 0.0)
	{
		int taps = max(int(u_Frame.ShadowBias.z), 1);
		float turn = TWO_PI * BlueNoise(pixel, ivec2(0));
		vec2 direction = vec2(cos(turn), sin(turn));
		const mat2 golden = mat2(cos(c_GoldenAngle), sin(c_GoldenAngle), -sin(c_GoldenAngle), cos(c_GoldenAngle));
		float disk = 0.0;
		for (int tap = 0; tap < taps; tap++)
		{
			vec2 offset = penumbraTexels * sqrt((float(tap) + 0.5) / float(taps)) * direction;
			float reference = receiverDepth + clamp(dot(depthPerTexel, offset), -maxCorrection, maxCorrection);
			disk += CompareShadowMap(cascade, uv + offset * texelSize, reference);
			direction = golden * direction;
		}
		shadowed = mix(tent, disk / float(taps), diskWeight);
	}
	result.Visibility = 1.0 - shadowed;
	result.PenumbraTexels = penumbraTexels;
	result.DiskWeight = diskWeight;
	return result;
}

// The sun's shadow at a camera-relative position with its geometric normal (facing the viewer) and view depth: the
// cascade's sample, mixed with the next cascade's (the last cascade's with no shadow) in its blend band, so neither the seam
// between cascades nor the end of the shadows at the shadow distance shows. PenumbraTexels and DiskWeight are mixed alike.
ShadowSample EvaluateDirectionalShadow(vec3 relativePosition, vec3 normal, float viewDepth, ivec2 pixel)
{
	int cascade = SelectShadowCascade(viewDepth, relativePosition);
	if (cascade < 0)
		return ShadowSample(1.0, 0.0, 0.0, -1);
	ShadowSample shadow = SampleCascadeShadow(cascade, relativePosition, normal, pixel);
	float blend = CascadeBlendWeight(cascade, viewDepth, relativePosition);
	if (blend > 0.0)
	{
		ShadowSample next = cascade + 1 < int(u_Frame.ShadowParams.w) ? SampleCascadeShadow(cascade + 1, relativePosition, normal, pixel)
			: ShadowSample(1.0, 0.0, 0.0, cascade);
		shadow.Visibility = mix(shadow.Visibility, next.Visibility, blend);
		shadow.PenumbraTexels = mix(shadow.PenumbraTexels, next.PenumbraTexels, blend);
		shadow.DiskWeight = mix(shadow.DiskWeight, next.DiskWeight, blend);
	}
	return shadow;
}

// Fraction of light reaching the surface (1 = fully lit); `normal` is the receiver's geometric normal (facing the viewer).
float SampleDirectionalShadow(vec3 relativePosition, vec3 normal, float viewDepth, vec2 pixel)
{
	return EvaluateDirectionalShadow(relativePosition, normal, viewDepth, ivec2(pixel)).Visibility;
}

#endif
