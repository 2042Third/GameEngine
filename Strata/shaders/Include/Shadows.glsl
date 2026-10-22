#ifndef STRATA_SHADOWS_GLSL
#define STRATA_SHADOWS_GLSL

// Directional light shadows: cascaded shadow maps (reversed-Z: larger depth = closer to the light) filtered with
// percentage-closer soft shadows (PCSS): a blocker search estimates the penumbra from the light's angular size,
// then percentage-closer filtering with that radius softens the edge. Each cascade blends into the next one over a band
// before its split (the last one fades out). Acne is prevented by the rasterizer's slope
// bias (shadow pass), a constant depth bias and a normal offset (ShadowBias), and a receiver plane depth bias that
// follows the receiver's slope across the filter region.

#include "Include/Scene.glsl"

layout(set = 0, binding = ST_SRV(7)) uniform texture2DArray u_ShadowMap;
layout(set = 0, binding = ST_SAMPLER(9)) uniform samplerShadow u_ShadowCompareSampler;
layout(set = 0, binding = ST_SAMPLER(10)) uniform sampler u_ShadowPointSampler;

const vec2 c_PoissonDisk[16] = vec2[](
	vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725), vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
	vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464), vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
	vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420), vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
	vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590), vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));

// Per-pixel rotation of the sampling pattern (interleaved gradient noise) trades banding for fine noise. With temporal
// anti-aliasing every frame turns it further (TemporalParams.z), so the history averages the noise away.
mat2 ShadowSampleRotation(vec2 pixel)
{
	float angle = TWO_PI * fract(fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715)))) + u_Frame.TemporalParams.z);
	float s = sin(angle);
	float c = cos(angle);
	return mat2(c, s, -s, c);
}

int SelectCascade(float viewDepth)
{
	int cascadeCount = int(u_Frame.ShadowParams.w);
	for (int cascade = 0; cascade < cascadeCount; cascade++)
	{
		if (viewDepth < u_Frame.CascadeSplits[cascade])
			return cascade;
	}
	return -1;
}

// How far a pixel is into the band where its cascade hands over to the next one (the last one fades out): 0 before the
// band, 1 at the split. The band is the last ShadowParams.z of the cascade's depth range, from the previous split (the
// first cascade's from the camera, at 0), as SceneRenderer::FindShadowCasters lays it out: the next cascade's slice starts
// at the band, so its square covers it.
float CascadeBlendWeight(int cascade, float viewDepth)
{
	float split = u_Frame.CascadeSplits[cascade];
	float previousSplit = cascade > 0 ? u_Frame.CascadeSplits[cascade - 1] : 0.0;
	float band = u_Frame.ShadowParams.z * (split - previousSplit);
	if (!(band > 0.0))
		return 0.0;
	return clamp((viewDepth - (split - band)) / band, 0.0, 1.0);
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

// Fraction of light reaching the surface in one cascade (1 = fully lit, also outside the cascade's square).
float SampleCascadeShadow(int cascade, vec3 relativePosition, vec3 normal, mat2 rotation)
{
	// Normal offset: push the lookup position off the surface by about a shadow texel to avoid acne.
	vec4 cascadeData = u_Cascades[cascade].Parameters; // x: world units per texel, y: depth range, z: search distance (world units)
	mat4 cascadeViewProjection = u_Cascades[cascade].ViewProjection;
	vec3 offsetPosition = relativePosition + normal * cascadeData.x * u_Frame.ShadowBias.y;
	vec4 lightClip = cascadeViewProjection * vec4(offsetPosition, 1.0);
	vec3 lightPosition = lightClip.xyz / lightClip.w;
	vec2 uv = NDCToUV(lightPosition.xy);
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
		return 1.0;
	float depthRange = max(cascadeData.y, 1e-6);
	float receiverDepth = lightPosition.z + u_Frame.ShadowBias.x * cascadeData.x / depthRange;

	// The receiver plane correction is limited to the search distance, so planes nearly along the light (curved receivers'
	// normals at their silhouettes) cannot move the comparison arbitrarily far.
	vec2 depthGradient = ReceiverDepthGradient(cascadeViewProjection, normal);
	float maxCorrection = cascadeData.z / depthRange;

	float texelSize = u_Frame.ShadowParams.y;

	// Blocker search over the region that could shade this receiver: the penumbra of blockers up to the search
	// distance away.
	float lightSizeUV = u_Frame.ShadowParams.x / max(cascadeData.x, 1e-6) * texelSize; // Penumbra per world unit of separation
	float searchRadius = clamp(lightSizeUV * cascadeData.z, texelSize, 32.0 * texelSize);
	float separationSum = 0.0;
	float blockerCount = 0.0;
	for (int index = 0; index < 16; index++)
	{
		vec2 offset = rotation * c_PoissonDisk[index] * searchRadius;
		float reference = receiverDepth + clamp(dot(depthGradient, offset), -maxCorrection, maxCorrection);
		float depth = textureLod(sampler2DArray(u_ShadowMap, u_ShadowPointSampler), vec3(uv + offset, float(cascade)), 0.0).r;
		if (depth > reference)
		{
			separationSum += depth - reference;
			blockerCount += 1.0;
		}
	}
	if (blockerCount == 0.0)
		return 1.0;

	// Penumbra grows with the distance between blocker and receiver (depth is linear in an orthographic projection).
	float separation = separationSum / blockerCount * depthRange;
	float filterRadius = clamp(separation * lightSizeUV, texelSize, 32.0 * texelSize);

	// The comparison sampler tests reference < stored: true where a blocker is closer to the light (reversed-Z).
	float shadowed = 0.0;
	for (int index = 0; index < 16; index++)
	{
		vec2 offset = rotation * c_PoissonDisk[index] * filterRadius;
		float reference = receiverDepth + clamp(dot(depthGradient, offset), -maxCorrection, maxCorrection);
		shadowed += texture(sampler2DArrayShadow(u_ShadowMap, u_ShadowCompareSampler), vec4(uv + offset, float(cascade), reference));
	}
	return 1.0 - shadowed / 16.0;
}

// Fraction of light reaching the surface (1 = fully lit); `normal` is the receiver's geometric normal (facing the viewer).
// Pixels in a cascade's blend band mix it with the next cascade by depth (the last cascade with no shadow), so neither the
// seam between cascades nor the end of the shadows at the shadow distance shows.
float SampleDirectionalShadow(vec3 relativePosition, vec3 normal, float viewDepth, vec2 pixel)
{
	int cascade = SelectCascade(viewDepth);
	if (cascade < 0)
		return 1.0;

	mat2 rotation = ShadowSampleRotation(pixel);
	float shadow = SampleCascadeShadow(cascade, relativePosition, normal, rotation);
	float blend = CascadeBlendWeight(cascade, viewDepth);
	if (blend > 0.0)
	{
		float next = cascade + 1 < int(u_Frame.ShadowParams.w) ? SampleCascadeShadow(cascade + 1, relativePosition, normal, rotation) : 1.0;
		shadow = mix(shadow, next, blend);
	}
	return shadow;
}

#endif
