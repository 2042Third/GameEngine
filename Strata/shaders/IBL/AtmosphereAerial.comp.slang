#include "Include/Common.slang"

#define ST_ATMOSPHERE_CUSTOM_PUSH_CONSTANTS
#include "Include/AtmospherePass.slang"

// Aerial perspective (SkyLightComponent::AerialPerspective): the air between the camera and every point of its view, as a volume
// of 32 x 32 columns across the screen and 32 depth slices along each. Per column, the view ray through the froxels' centers
// is integrated as the sky-view table integrates the whole sky (AccumulateAtmosphereSegment): the light the air scatters toward
// the camera up to each slice, per lux of sun of the sun's color, into slices 0 to 31, and the transmittance to it into slices
// 32 to 63. Slice k ends at D ((k + 0.5) / 32)^2 meters (D: the volume's far distance), crowding near the camera where the
// change is fastest; distances are multiplied by the distance scale before they enter the air, so small worlds can show the
// haze of large ones. Include/AerialPerspective.glsl applies it.

[vk::image_format("rgba16f")] RWTexture3D<float4> o_Volume : register(u0);

// RenderData::AerialPerspectiveParameters: the view rays through the screen's corners (camera-relative world space, not
// normalized: they interpolate linearly across the screen), and what the march needs in their w.
struct AerialPerspectiveParameters
{
	float4 RayTopLeft;     // w: the volume's far distance D (meters)
	float4 RayTopRight;    // w: the distance scale
	float4 RayBottomLeft;  // w: the viewer's distance from the planet's center (km)
	float4 RayBottomRight; // w: unused
};
[[vk::push_constant]] ConstantBuffer<AerialPerspectiveParameters> u_Aerial;

static const uint c_AerialSize = 32u;
static const uint c_AerialSlices = 32u;

void st_Main()
{
	uint2 column = st_GlobalInvocationID.xy;
	if (any((column >= uint2(c_AerialSize))))
		return;
	float2 uv = (float2(column) + 0.5) / float(c_AerialSize);
	float3 ray = lerp(lerp(u_Aerial.RayTopLeft.xyz, u_Aerial.RayTopRight.xyz, uv.x), lerp(u_Aerial.RayBottomLeft.xyz, u_Aerial.RayBottomRight.xyz, uv.x), uv.y);
	float3 direction = normalize(ray);
	float radius = u_Aerial.RayBottomLeft.w;
	float mu = direction.y; // The viewer's zenith is the world's +Y
	float3 toSun = u_Atmosphere.SunDirection.xyz;
	float cosTheta = dot(direction, toSun);
	float rayleighPhase = RayleighPhase(cosTheta);
	float miePhase = MiePhase(cosTheta);
	AtmosphereMedium medium = GetMedium();
	// The slices' distances in kilometers of air.
	float farDistance = u_Aerial.RayTopLeft.w * u_Aerial.RayTopRight.w * 0.001;

	float3 transmittance = float3(1.0);
	float3 light = float3(0.0);
	float previous = 0.0;
	for (uint slice = 0u; slice < c_AerialSlices; slice++)
	{
		float fraction = (float(slice) + 0.5) / float(c_AerialSlices);
		float target = farDistance * fraction * fraction;
		// Two midpoint steps from the previous slice's distance to this one's.
		float step = 0.5 * (target - previous);
		AccumulateAtmosphereSegment(medium, radius, mu, direction, toSun, rayleighPhase, miePhase, previous + 0.5 * step, step, transmittance, light);
		AccumulateAtmosphereSegment(medium, radius, mu, direction, toSun, rayleighPhase, miePhase, previous + 1.5 * step, step, transmittance, light);
		previous = target;
		// Finite and within half floats, like every sky.
		o_Volume[int3(column, slice)] = float4(clamp(light * u_Atmosphere.SunColor.rgb, float3(0.0), float3(65000.0)), 1.0);
		o_Volume[int3(column, slice + c_AerialSlices)] = float4(transmittance, 1.0);
	}
}

// --- Entry point ---
[numthreads(8, 8, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
