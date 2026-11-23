// SRV 35 holds the probes' ambient cubes here (Include/GIField.slang's ReadProbeIrradiance).
#define ST_GI_FIELD_AMBIENT_CUBE
#include "Include/Common.slang"

#include "Include/GIField.slang"
#include "Include/Scene.slang"

// The probes debug view (SceneLightingView::Probes): global illumination's cascade-0 probes over the lit image. Per pixel, the
// camera's ray walks the field's cells (a 3D DDA, at most c_MaxCells) and tests a sphere of 0.12 spacings at each cell's probe
// where it moved, nearer than the scene's depth: a live probe is shaded with its ambient cube at the sphere's normal through
// the frame's exposure and the display curve x / (1 + x); a dead one (inside a solid) dark red. Pixels without a probe keep the
// lit image.

Texture2D<float4> u_SceneDepth : register(t41);

static const uint c_MaxCells = 96u;
static const float c_SphereRadius = 0.12; // Probe spacings
static const float3 c_DeadColor = float3(0.6, 0.05, 0.05);

float4 main(float4 fragCoord : SV_Position) : SV_Target0
{
	if (u_GIField.FieldOrigin.w <= 0.0)
		discard; // No field this frame
	float2 uv = fragCoord.xy * u_Frame.ViewportSize.zw;
	float2 ndc = UVToNDC(uv);
	float4 nearPoint = mul(u_Frame.InverseViewProjection, float4(ndc, 1.0, 1.0));
	float4 farPoint = mul(u_Frame.InverseViewProjection, float4(ndc, 0.5, 1.0));
	float3 origin = nearPoint.xyz / nearPoint.w;
	float3 direction = normalize(farPoint.xyz / farPoint.w - origin);
	// The scene's surface along the ray (reversed-Z: 0 is the far plane, nothing there).
	float depth = u_SceneDepth.Load(int3(int2(fragCoord.xy), 0)).r;
	float sceneDistance = 1e30;
	if (depth > 0.0)
	{
		float4 surface = mul(u_Frame.InverseViewProjection, float4(ndc, depth, 1.0));
		sceneDistance = length(surface.xyz / surface.w - origin);
	}

	float spacing = u_GIField.FieldOrigin.w;
	float3 grid = u_GIField.FieldSize.xyz;
	float3 boxMinimum = u_GIField.FieldOrigin.xyz;
	float3 boxMaximum = boxMinimum + grid * spacing;
	// Where the ray enters the field's box.
	float3 inverse = 1.0 / select(abs(direction) > 1e-8, direction, float3(1e-8, 1e-8, 1e-8));
	float3 t0 = (boxMinimum - origin) * inverse;
	float3 t1 = (boxMaximum - origin) * inverse;
	float3 tNear = min(t0, t1);
	float3 tFar = max(t0, t1);
	float enter = max(max(tNear.x, tNear.y), max(tNear.z, 0.0));
	float exit = min(min(tFar.x, tFar.y), tFar.z);
	if (enter >= exit || enter >= sceneDistance)
		discard;

	float3 start = (origin + direction * (enter + 1e-4) - boxMinimum) / spacing;
	int3 cell = clamp(int3(floor(start)), int3(0, 0, 0), int3(grid) - 1);
	int3 stepSign = int3(sign(direction));
	float3 next = (float3(cell) + max(float3(stepSign), float3(0.0, 0.0, 0.0)) - start) * spacing * inverse + enter;
	float3 delta = abs(spacing * inverse);
	float radius = c_SphereRadius * spacing;
	float3 color = float3(0.0, 0.0, 0.0);
	bool found = false;
	[loop]
	for (uint visited = 0u; visited < c_MaxCells; visited++)
	{
		if (any(cell < int3(0, 0, 0)) || any(cell >= int3(grid)))
			break;
		float4 validity = u_GIFieldValidity.Load(int4(cell, 0));
		// Where the probe moved off a surface it sat on (ProbeValidity.comp): a ray tests it in its own cell only.
		float3 center = boxMinimum + (float3(cell) + 0.5) * spacing + DecodeProbeMove(validity, spacing);
		float3 toCenter = center - origin;
		float along = dot(toCenter, direction);
		float3 closest = toCenter - direction * along;
		float missSquared = dot(closest, closest);
		if (missSquared < radius * radius)
		{
			float hit = along - sqrt(radius * radius - missSquared);
			if (hit > 0.0 && hit < sceneDistance)
			{
				float3 normal = normalize(origin + direction * hit - center);
				float3 radiance = max(ReadAmbientCube(cell, normal).rgb, float3(0.0, 0.0, 0.0)) / ST_GI_RADIANCE_SCALE * u_Frame.TemporalParams.y;
				color = validity.r <= 0.0 ? c_DeadColor : radiance / (1.0 + radiance);
				found = true;
				break;
			}
		}
		float nearest = min(next.x, min(next.y, next.z));
		if (nearest > sceneDistance || nearest > exit)
			break;
		if (next.x <= next.y && next.x <= next.z)
		{
			cell.x += stepSign.x;
			next.x += delta.x;
		}
		else if (next.y <= next.z)
		{
			cell.y += stepSign.y;
			next.y += delta.y;
		}
		else
		{
			cell.z += stepSign.z;
			next.z += delta.z;
		}
	}
	if (!found)
		discard;
	return float4(LinearToSRGB(color), 1.0);
}
