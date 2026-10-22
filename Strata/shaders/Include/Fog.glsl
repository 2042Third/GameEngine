#ifndef STRATA_FOG_GLSL
#define STRATA_FOG_GLSL

#include "Include/Scene.glsl"

// Exponential height fog (HeightFogComponent; SceneRenderer::SetHeightFog fills the frame's Fog* constants). Its extinction
// at a height h above the base height is density * exp(-falloff * h); along a view ray it is integrated analytically from
// the start distance on, so it costs a few instructions per pixel and no pass. Scene/Forward.frag fogs every surface it
// shades (opaque and transparent), Scene/Skybox.frag the sky at an infinite distance. Rays that stay level or go down
// never leave the fog: the sky there gets the maximum opacity.

// Stands for the optical depth of a ray that never leaves the fog: exp(-depth) is 0.
const float c_FogEndlessDepth = 1.0e30;

// The fog's optical depth over `distance` along a ray that starts `startHeight` above the base height and rises `rise` over
// it (negative: falls). The density at the ray's lowest point times the distance, times the mean of exp(-y) over [0, y] for
// the height difference y in units of the falloff, which stays in (0, 1]: no term overflows but the lowest point's
// density, whose exponent is bounded (a ray that deep in the fog is opaque anyway).
float GetFogOpticalDepth(float startHeight, float rise, float distance)
{
	float falloff = u_Frame.FogDensity.y;
	float lowest = startHeight + min(rise, 0.0);
	float y = falloff * abs(rise);
	float mean = y > 1.0e-4 ? (1.0 - exp(-y)) / y : 1.0 - 0.5 * y;
	// The density first: a density of 0 stays 0 through the finite factors after it.
	return u_Frame.FogDensity.x * distance * exp(min(-falloff * lowest, 80.0)) * mean;
}

// The light the fog scatters toward a viewer looking along `direction`: the sky's (a constant light, or an environment map's
// irradiance) equally in every direction, the directional light's through the Henyey-Greenstein phase function, both times
// the fog's albedo.
vec3 GetFogInscattering(vec3 direction)
{
	vec3 light = u_Frame.FogLight.rgb;
	if (u_Frame.FogLight.w > 0.0)
	{
		// An environment map's irradiance from above (its radiance where it is uniform), taken for the light from every
		// direction, as if the ground below the fog were as bright as the sky: the light the fog scatters within itself makes
		// up much of what a darker ground does not send.
		light += textureLod(samplerCube(u_IrradianceMap, u_LinearClampSampler), vec3(0.0, 1.0, 0.0), 0.0).rgb * u_Frame.EnvironmentParams.x;
	}
	if (u_Frame.DirectionalLightDirection.w > 0.0)
	{
		float g = u_Frame.FogParams.y;
		float cosTheta = dot(direction, -u_Frame.DirectionalLightDirection.xyz);
		float denominator = 1.0 + g * g - 2.0 * g * cosTheta;
		light += u_Frame.DirectionalLightColor.rgb * ((1.0 - g * g) / (4.0 * PI * denominator * sqrt(denominator)));
	}
	return u_Frame.FogColor.rgb * light;
}

// `color` seen through fog of the given optical depth along `direction`. With a depth of 0 the color comes back unchanged
// to the bit.
vec3 BlendFog(vec3 color, vec3 direction, float opticalDepth)
{
	float opacity = min(1.0 - exp(-opticalDepth), u_Frame.FogColor.w);
	return color * (1.0 - opacity) + GetFogInscattering(direction) * opacity;
}

// Where a view ray toward a camera-relative point starts (camera-relative) and its direction. Perspective rays start at the
// camera; orthographic ones on the camera's plane, along its forward direction.
void GetFogRay(vec3 relativePosition, out vec3 start, out vec3 direction)
{
	if (u_Frame.CameraForward.w > 0.5)
	{
		direction = u_Frame.CameraForward.xyz;
		start = relativePosition - direction * dot(relativePosition, direction);
		return;
	}
	start = vec3(0.0);
	float distance = length(relativePosition);
	direction = distance > 0.0 ? relativePosition / distance : u_Frame.CameraForward.xyz;
}

// A surface's shaded color at a camera-relative position seen through the scene's fog (only when FogParams.x > 0).
vec3 ApplyHeightFog(vec3 color, vec3 relativePosition)
{
	vec3 start;
	vec3 direction;
	GetFogRay(relativePosition, start, direction);
	float startDistance = u_Frame.FogDensity.w;
	float distanceInFog = dot(relativePosition - start, direction) - startDistance;
	if (!(distanceInFog > 0.0))
		return color;
	float startHeight = u_Frame.FogDensity.z + start.y + direction.y * startDistance;
	return BlendFog(color, direction, GetFogOpticalDepth(startHeight, direction.y * distanceInFog, distanceInFog));
}

// The sky's color along a view ray through the scene's fog, at an infinite distance (only when FogParams.x > 0): rays that
// rise leave the fog after a finite depth, the others never do.
vec3 ApplyHeightFogToSky(vec3 color, vec3 start, vec3 direction)
{
	float density = u_Frame.FogDensity.x;
	float falloff = u_Frame.FogDensity.y;
	float startHeight = u_Frame.FogDensity.z + start.y + direction.y * u_Frame.FogDensity.w;
	float rate = falloff * direction.y;
	float opticalDepth = density > 0.0 ? c_FogEndlessDepth : 0.0;
	if (rate > 0.0)
		opticalDepth = min(density * exp(min(-falloff * startHeight, 80.0)) / rate, c_FogEndlessDepth);
	return BlendFog(color, direction, opticalDepth);
}

#endif
