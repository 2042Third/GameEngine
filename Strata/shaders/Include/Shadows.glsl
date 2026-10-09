#ifndef STRATA_SHADOWS_GLSL
#define STRATA_SHADOWS_GLSL

// Directional light shadows: cascaded shadow maps (reversed-Z: larger depth = closer to the light) filtered with
// percentage-closer soft shadows (PCSS): a blocker search estimates the penumbra from the light's angular size,
// then percentage-closer filtering with that radius softens the edge. Acne is prevented by the rasterizer's slope
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

// Per-pixel rotation of the sampling pattern (interleaved gradient noise) trades banding for fine noise.
mat2 ShadowSampleRotation(vec2 pixel)
{
	float angle = TWO_PI * fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
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

// Receiver plane depth bias: the gradient of the receiver's light-space depth over the shadow map's uv, from the
// screen-space derivatives of its world position. Filter samples are compared against the receiver's own depth at
// their offset rather than at the center, which would let a surface tilted toward the light shadow itself wherever
// the filter reaches past the depth bias (acne at grazing light angles). Zero when the derivatives are degenerate.
vec2 ReceiverDepthGradient(mat4 cascadeViewProjection, vec3 positionDx, vec3 positionDy)
{
	// The cascade projection is orthographic: light-space differences transform without the translation.
	vec3 lightDx = (cascadeViewProjection * vec4(positionDx, 0.0)).xyz;
	vec3 lightDy = (cascadeViewProjection * vec4(positionDy, 0.0)).xyz;
	vec2 uvDx = vec2(0.5, -0.5) * lightDx.xy;
	vec2 uvDy = vec2(0.5, -0.5) * lightDy.xy;
	// Solve [uvDx; uvDy] * gradient = (depth change along x, depth change along y).
	float determinant = uvDx.x * uvDy.y - uvDx.y * uvDy.x;
	if (abs(determinant) < 1e-24)
		return vec2(0.0);
	return vec2(uvDy.y * lightDx.z - uvDx.y * lightDy.z, uvDx.x * lightDy.z - uvDy.x * lightDx.z) / determinant;
}

// Fraction of light reaching the surface (1 = fully lit). positionDx and positionDy are the screen-space derivatives of
// worldPosition (taken in uniform control flow by the caller).
float SampleDirectionalShadow(vec3 worldPosition, vec3 normal, float viewDepth, vec2 pixel, vec3 positionDx, vec3 positionDy)
{
	int cascade = SelectCascade(viewDepth);
	if (cascade < 0)
		return 1.0;

	// Normal offset: push the lookup position off the surface by about a shadow texel to avoid acne.
	vec4 cascadeData = u_Frame.CascadeData[cascade]; // x: world units per texel, y: depth range, z: search distance (world units)
	mat4 cascadeViewProjection = u_Frame.CascadeViewProjection[cascade];
	vec3 offsetPosition = worldPosition + normal * cascadeData.x * u_Frame.ShadowBias.y;
	vec4 lightClip = cascadeViewProjection * vec4(offsetPosition, 1.0);
	vec3 lightPosition = lightClip.xyz / lightClip.w;
	vec2 uv = NDCToUV(lightPosition.xy);
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
		return 1.0;
	float depthRange = max(cascadeData.y, 1e-6);
	float receiverDepth = lightPosition.z + u_Frame.ShadowBias.x * cascadeData.x / depthRange;

	// The receiver plane correction is limited to the search distance, so wrong derivatives (at silhouettes, where a
	// pixel quad spans several surfaces) cannot move the comparison arbitrarily far.
	vec2 depthGradient = ReceiverDepthGradient(cascadeViewProjection, positionDx, positionDy);
	float maxCorrection = cascadeData.z / depthRange;

	float texelSize = u_Frame.ShadowParams.y;
	mat2 rotation = ShadowSampleRotation(pixel);

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

#endif
