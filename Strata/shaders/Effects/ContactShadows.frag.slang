#include "Include/Common.slang"

#include "Include/BlueNoise.slang"
#include "Include/Scene.slang"

// The sun's contact shadows (SceneRenderer::RenderScreenShadows, ScreenShadowParameters.ContactSteps > 0: High and Ultra),
// a pass of their own between the mask's evaluation (Effects/ScreenShadows.frag, read here as the source) and its blur:
// from each pixel the sun still reaches whose normal and own face both face it, a march toward the sun over the depth buffer
// finds what lies below the cascades' texels. A step that ends a little behind the surface the depth buffer holds there (its
// own plane, from the depths, not the prepass's interpolated normal) occludes. Fixed per-pixel offsets (no temporal noise)
// dither where a hit depends on whether a step lands within the thickness behind a surface (a contact shadow's far edge, a ray
// passing behind an object seen from the camera), which the blur takes with a radius of half a step's length on screen, given
// to every pixel whose march hit, passed behind a surface by more than the thickness or passed near an occluder; hits fade
// over the march's last 40%, with distance and at the screen's edges. Out: the source's visibility times what the march
// leaves of it (its encoding, the square root, multiplied by the square root), and the larger of the two blur radii. Kept
// apart from the evaluation so neither carries the other's registers.

Texture2D<float4> u_Depth : register(t22);
Texture2D<float4> u_Normals : register(t23); // The prepass's view-space geometric normal, octahedral
Texture2D<float4> u_Source : register(t24);  // The evaluated mask

struct ScreenShadowParameters
{
	float ContactLength;
	uint ContactSteps;
	float ContactMaxPixels;
	float ContactFadeStart;
	float ContactFadeEnd;
	float ContactThickness;
	float ContactThicknessScale;
	uint Reserved;
};
[[vk::push_constant]] ConstantBuffer<ScreenShadowParameters> u_ScreenShadows;

static float2 o_Mask;

static const float c_ScreenShadowMaxBlurPixels = 8.0;
static const float c_ContactMinBehind = 0.002; // Meters behind a surface a march step must lie to occlude: depth precision

// The camera-relative position the depth buffer holds at a pixel.
float3 PositionAt(int2 pixel, float depth)
{
	float2 uv = (float2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	float4 point = mul(u_Frame.InverseViewProjection, float4(UVToNDC(uv), depth, 1.0));
	return point.xyz / point.w;
}

// The plane of the surface the depth buffer holds at a pixel (not the prepass's interpolated normal, which leans off the
// faces of coarse smooth-shaded meshes): through the pixel's position and, on each axis, the neighbor nearer in depth (the
// other may lie across a silhouette; device depths rank neighbors as view depths do, being monotonic in them), turned
// toward the viewer along the pixel's own view ray (GetViewVector: an orthographic camera's position says nothing about which
// side of a face it sees, e.g. a wall beyond it laterally). Zero where an axis has no drawn neighbor.
float3 FaceNormal(int2 pixel, float3 position, float depth)
{
	int2 last = int2(u_Frame.ViewportSize.xy) - 1;
	float3 axes[2];
	for (int axis = 0; axis < 2; axis++)
	{
		int2 direction = axis == 0 ? int2(1, 0) : int2(0, 1);
		int2 before = pixel - direction;
		int2 after = pixel + direction;
		bool hasBefore = all((before >= int2(0)));
		bool hasAfter = all((after <= last));
		float depthBefore = hasBefore ? u_Depth.Load(int3(before, 0)).r : 0.0;
		float depthAfter = hasAfter ? u_Depth.Load(int3(after, 0)).r : 0.0;
		bool useAfter = depthAfter > 0.0 && (!(depthBefore > 0.0) || abs(depthAfter - depth) <= abs(depthBefore - depth));
		if (useAfter)
			axes[axis] = PositionAt(after, depthAfter) - position;
		else if (depthBefore > 0.0)
			axes[axis] = position - PositionAt(before, depthBefore);
		else
			axes[axis] = float3(0.0);
	}
	float3 normal = cross(axes[0], axes[1]);
	if (dot(normal, normal) < 1e-20)
		return float3(0.0);
	normal = normalize(normal);
	return dot(normal, GetViewVector(position)) < 0.0 ? -normal : normal;
}

// Contact shadows: a march toward the sun over the depth buffer, for what the cascades are too coarse to show. Returns the
// occlusion (0: none), the march's step on screen in pixels, and 1 where the march hit or passed near an occluder (a step
// lay behind a surface by more than the thickness, or a surface stood off the start's plane toward the sun by more than it:
// its neighbors' marches may hit where this one does not, and the blur evens them out), else 0. Points along the march are
// interpolated in clip space (a line's clip coordinates are linear along it); the steps are judged in order, one at a time:
// reading the depth of several steps together saves nothing here (four at a time cost 5-10% more, in registers). A step in
// front of that surface along the view by more than two pixels' footprint lies in front of its plane too (but on surfaces
// seen nearly edge-on, whose contact shadows are given up), so most steps end there; the others take the face's own plane
// (FaceNormal), which decides. A march only counts from a pixel whose own face faces the sun, which is asked once something
// is hit.
float3 ContactShadow(int2 pixel, float3 position, float depth, float3 normal, float viewDepth, float pixelWorld)
{
	float3 toSun = -u_Frame.DirectionalLightDirection.xyz;
	if (u_ScreenShadows.ContactSteps == 0u || viewDepth >= u_ScreenShadows.ContactFadeEnd || dot(normal, toSun) <= 0.0)
		return float3(0.0);

	// The march's reach, shortened to ContactMaxPixels on screen. A point r meters toward the sun has the clip coordinates
	// C0 + r D, so its distance on screen from the start is r B / (w0 + r D.w) for B = |D.xy w0 - C0.xy D.w| / w0 in pixels:
	// it reaches ContactMaxPixels at r = M w0 / (B - M D.w), which for a march toward the camera (D.w < 0) lies before the
	// camera's plane, so a march is shortened, never dropped, as it nears the camera.
	float3 start = position + normal * pixelWorld;
	float4 startClip = mul(u_Frame.ViewProjection, float4(start, 1.0));
	float4 clipPerMeter = mul(u_Frame.ViewProjection, float4(toSun, 0.0));
	if (!(startClip.w > 0.0))
		return float3(0.0);
	float2 pixelsPerNDC = float2(0.5, -0.5) * u_Frame.ViewportSize.xy;
	float screenRate = length((clipPerMeter.xy * startClip.w - startClip.xy * clipPerMeter.w) * pixelsPerNDC) / startClip.w;
	float maxPixels = u_ScreenShadows.ContactMaxPixels;
	float reach = u_ScreenShadows.ContactLength;
	if (screenRate > maxPixels * clipPerMeter.w)
		reach = min(reach, maxPixels * startClip.w / (screenRate - maxPixels * clipPerMeter.w));
	float4 endClip = startClip + clipPerMeter * reach;
	float screenLength = reach * screenRate / max(endClip.w, 1e-6);
	uint steps = u_ScreenShadows.ContactSteps;
	float stepPixels = screenLength / float(steps);
	float offset = BlueNoise(pixel, int2(32, 17)); // Fixed per pixel, apart from the disk's turn
	float startViewDepth = -(mul(u_Frame.View, float4(start, 1.0))).z;
	float sunViewDepth = -(mul(((float3x3)(u_Frame.View)), toSun)).z; // View depth gained per meter toward the sun
	bool orthographic = u_Frame.CameraForward.w > 0.5;
	float nearOccluder = 0.0;
	int2 last = int2(u_Frame.ViewportSize.xy) - 1;

	for (uint index = 0u; index < steps; index++)
	{
		// Where the step falls on screen. A step off the image (or behind the camera) ends the march: the steps after it are off
		// the image too.
		float t = (float(index) + offset) / float(steps);
		float4 clip = lerp(startClip, endClip, t);
		float2 uv = NDCToUV(clip.xy / max(clip.w, 1e-6));
		if (!(clip.w > 0.0 && all((uv >= float2(0.0))) && all((uv <= float2(1.0)))))
			break;
		float along = reach * t;
		if (along < 2.0 * pixelWorld)
			continue; // The start's own neighborhood (terminators of coarse meshes)
		int2 stepPixel = clamp(int2(uv * u_Frame.ViewportSize.xy), int2(0), last);
		float stepDepth = u_Depth.Load(int3(stepPixel, 0)).r;
		if (!(stepDepth > 0.0))
			continue; // Nothing drawn there
		float3 surface = PositionAt(stepPixel, stepDepth);
		float stepViewDepth = startViewDepth + sunViewDepth * along;
		float thickness = u_ScreenShadows.ContactThickness + u_ScreenShadows.ContactThicknessScale * stepViewDepth;
		// A surface standing off the start's own plane by more than the thickness, on the side the march rises to: an occluder
		// the march passes near, whose neighbors' marches may hit where this one does not.
		if (dot(normal, surface - position) > thickness)
			nearOccluder = 1.0;
		float surfaceViewDepth = dot(surface, u_Frame.CameraForward.xyz);
		float footprint = orthographic ? pixelWorld : pixelWorld * surfaceViewDepth / viewDepth;
		if (stepViewDepth - surfaceViewDepth < -2.0 * footprint)
			continue;
		float3 toStep = start + toSun * along - surface;
		float behind = -dot(FaceNormal(stepPixel, surface, stepDepth), toStep); // How far the step lies behind that surface
		if (behind > c_ContactMinBehind && behind < thickness)
		{
			// A face turned from the sun under a normal turned toward it: its ray entered its own mesh.
			if (dot(FaceNormal(pixel, position, depth), toSun) <= 0.0)
				return float3(0.0);
			float2 edge = clamp(min(uv, 1.0 - uv) / 0.05, 0.0, 1.0);
			float fade = 1.0 - smoothstep(u_ScreenShadows.ContactFadeStart, u_ScreenShadows.ContactFadeEnd, viewDepth);
			return float3(fade * edge.x * edge.y * (1.0 - smoothstep(0.6, 1.0, t)), stepPixels, 1.0);
		}
		if (behind >= thickness)
			nearOccluder = 1.0;
	}
	return float3(0.0, stepPixels, nearOccluder);
}

void st_Main()
{
	int2 pixel = int2(st_FragCoord.xy);
	float2 source = u_Source.Load(int3(pixel, 0)).rg;
	float depth = u_Depth.Load(int3(pixel, 0)).r;
	o_Mask = source;
	if (!(depth > 0.0) || !(source.r > 0.0))
		return; // Nothing drawn, or no sun left to take
	float2 uv = (float2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	float4 point = mul(u_Frame.InverseViewProjection, float4(UVToNDC(uv), depth, 1.0));
	float3 position = point.xyz / point.w;
	float viewDepth = -(mul(u_Frame.View, float4(position, 1.0))).z;
	float3 normal = normalize(mul(((float3x3)(u_Frame.InverseView)), OctahedralDecode(u_Normals.Load(int3(pixel, 0)).xy)));
	float pixelWorld = 2.0 * (u_Frame.CameraForward.w > 0.5 ? 1.0 : viewDepth) / (u_Frame.Projection[1][1] * u_Frame.ViewportSize.y);
	float3 contact = ContactShadow(pixel, position, depth, normal, viewDepth, pixelWorld);
	if (contact.z > 0.0)
		o_Mask = float2(source.r * sqrt(1.0 - contact.x), max(source.g, clamp(0.5 * contact.y, 1.0, 4.0) / c_ScreenShadowMaxBlurPixels));
}

// --- Entry point ---
struct ST_Input
{
};
struct ST_Output
{
	[[vk::location(0)]] float2 o_Mask : SV_Target0;
};
ST_Output main(ST_Input input, float4 fragCoord : SV_Position, bool frontFacing : SV_IsFrontFace)
{
	st_FragCoord = fragCoord;
	st_FrontFacing = frontFacing;
	st_Main();
	ST_Output output;
	output.o_Mask = o_Mask;
	return output;
}
