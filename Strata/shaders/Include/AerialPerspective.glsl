#ifndef STRATA_AERIAL_PERSPECTIVE_GLSL
#define STRATA_AERIAL_PERSPECTIVE_GLSL

#include "Include/Scene.glsl"

// Aerial perspective (IBL/AtmosphereAerial.comp's volume, SceneRenderer::RenderAerialPerspective): the air between the camera
// and a surface dims it and adds the light it scatters toward the camera. FogParams.z is the volume's far distance in meters
// (0: none, a black dummy is bound), FogParams.w its distance scale.

// 32 x 32 columns across the screen, slices 0-31 the in-scattered light per lux of sun (the sky's units, times
// EnvironmentParams.x), slices 32-63 the transmittance.
layout(set = 0, binding = ST_SRV(29)) uniform texture3D u_AerialPerspective;

const float c_AerialSlices = 32.0;

// A surface's color at `relativePosition` (camera-relative, meters) seen through the air (only when FogParams.z > 0).
vec3 ApplyAerialPerspective(vec3 color, vec3 relativePosition, vec2 fragCoord)
{
	vec2 uv = fragCoord * u_Frame.ViewportSize.zw;
	// Slice k's center lies at D ((k + 0.5) / 32)^2: invert it, clamped to the volume (the first slice holds the camera's near
	// air, the last everything beyond D).
	float slice = clamp(sqrt(length(relativePosition) / u_Frame.FogParams.z) * c_AerialSlices - 0.5, 0.0, c_AerialSlices - 1.0);
	vec3 scattering = textureLod(sampler3D(u_AerialPerspective, u_LinearClampSampler), vec3(uv, (slice + 0.5) / (2.0 * c_AerialSlices)), 0.0).rgb;
	vec3 transmittance = textureLod(sampler3D(u_AerialPerspective, u_LinearClampSampler),
		vec3(uv, (slice + c_AerialSlices + 0.5) / (2.0 * c_AerialSlices)), 0.0).rgb;
	return color * transmittance + scattering * u_Frame.EnvironmentParams.x;
}

#endif
