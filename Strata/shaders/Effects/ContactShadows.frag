#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require

#include "Include/BlueNoise.glsl"
#include "Include/Scene.glsl"

// The sun's contact shadows (SceneRenderer::RenderScreenShadows, ScreenShadowParameters.ContactSteps > 0: High and Ultra),
// a pass of their own between the mask's evaluation (Effects/ScreenShadows.frag, read here as the source) and its blur:
// from each pixel the sun still reaches whose normal and own face both face it, a march toward the sun over the depth buffer
// finds what lies below the cascades' texels. A step that ends a little behind the surface the depth buffer holds there (its
// own plane, from the depths, not the prepass's interpolated normal) occludes. Fixed per-pixel offsets (no temporal noise)
// dither where a hit depends on whether a step lands within the thickness behind a surface (a contact shadow's far edge, a ray
// passing behind an object seen from the camera), which the blur takes with a radius of half a step's length on screen, given
// to every pixel whose march hit or passed behind a surface by more than the thickness; hits fade over the march's last 40%,
// with distance and at the screen's edges. Out: the source's visibility times what the march leaves of
// it (its encoding, the square root, multiplied by the square root), and the larger of the two blur radii. Kept apart from the
// evaluation so neither carries the other's registers.

layout(set = 0, binding = ST_SRV(22)) uniform texture2D u_Depth;
layout(set = 0, binding = ST_SRV(23)) uniform texture2D u_Normals; // The prepass's view-space geometric normal, octahedral
layout(set = 0, binding = ST_SRV(24)) uniform texture2D u_Source;  // The evaluated mask

layout(push_constant) uniform ScreenShadowParameters
{
	float ContactLength;
	uint ContactSteps;
	float ContactMaxPixels;
	float ContactFadeStart;
	float ContactFadeEnd;
	float ContactThickness;
	float ContactThicknessScale;
	uint Reserved;
} u_ScreenShadows;

layout(location = 0) out vec2 o_Mask;

const float c_ScreenShadowMaxBlurPixels = 8.0;
const float c_ContactMinBehind = 0.002; // Meters behind a surface a march step must lie to occlude: depth precision
const uint c_ContactBatch = 4u; // Steps whose depth reads are issued together

// The camera-relative position the depth buffer holds at a pixel.
vec3 PositionAt(ivec2 pixel, float depth)
{
	vec2 uv = (vec2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	vec4 point = u_Frame.InverseViewProjection * vec4(UVToNDC(uv), depth, 1.0);
	return point.xyz / point.w;
}

// The plane of the surface the depth buffer holds at a pixel (not the prepass's interpolated normal, which leans off the
// faces of coarse smooth-shaded meshes): through the pixel's position and, on each axis, the neighbor nearer in depth (the
// other may lie across a silhouette; device depths rank neighbors as view depths do, being monotonic in them), turned
// toward the viewer along the pixel's own view ray (GetViewVector: an orthographic camera's position says nothing about which
// side of a face it sees, e.g. a wall beyond it laterally). Zero where an axis has no drawn neighbor.
vec3 FaceNormal(ivec2 pixel, vec3 position, float depth)
{
	ivec2 last = ivec2(u_Frame.ViewportSize.xy) - 1;
	vec3 axes[2];
	for (int axis = 0; axis < 2; axis++)
	{
		ivec2 direction = axis == 0 ? ivec2(1, 0) : ivec2(0, 1);
		ivec2 before = pixel - direction;
		ivec2 after = pixel + direction;
		bool hasBefore = all(greaterThanEqual(before, ivec2(0)));
		bool hasAfter = all(lessThanEqual(after, last));
		float depthBefore = hasBefore ? texelFetch(u_Depth, before, 0).r : 0.0;
		float depthAfter = hasAfter ? texelFetch(u_Depth, after, 0).r : 0.0;
		bool useAfter = depthAfter > 0.0 && (!(depthBefore > 0.0) || abs(depthAfter - depth) <= abs(depthBefore - depth));
		if (useAfter)
			axes[axis] = PositionAt(after, depthAfter) - position;
		else if (depthBefore > 0.0)
			axes[axis] = position - PositionAt(before, depthBefore);
		else
			axes[axis] = vec3(0.0);
	}
	vec3 normal = cross(axes[0], axes[1]);
	if (dot(normal, normal) < 1e-20)
		return vec3(0.0);
	normal = normalize(normal);
	return dot(normal, GetViewVector(position)) < 0.0 ? -normal : normal;
}

// Contact shadows: a march toward the sun over the depth buffer, for what the cascades are too coarse to show. Returns the
// occlusion (0: none), the march's step on screen in pixels, and 1 where a step lay behind a surface by more than the
// thickness (its neighbors' marches may hit: the blur evens them out), else 0. Points along the march are interpolated in clip space (a
// line's clip coordinates are linear along it); the steps read the depth buffer c_ContactBatch at a time, so their reads
// overlap, and are then judged in order. A step in front of that surface along the view by more than two pixels' footprint
// lies in front of its plane too (but on surfaces seen nearly edge-on, whose contact shadows are given up), so most steps end
// there; the others take the face's own plane (FaceNormal), which decides. A march only counts from a pixel whose own face
// faces the sun, which is asked once something is hit.
vec3 ContactShadow(ivec2 pixel, vec3 position, float depth, vec3 normal, float viewDepth, float pixelWorld)
{
	vec3 toSun = -u_Frame.DirectionalLightDirection.xyz;
	if (u_ScreenShadows.ContactSteps == 0u || viewDepth >= u_ScreenShadows.ContactFadeEnd || dot(normal, toSun) <= 0.0)
		return vec3(0.0);

	// The march's reach, shortened to ContactMaxPixels on screen.
	vec3 start = position + normal * pixelWorld;
	float reach = u_ScreenShadows.ContactLength;
	vec4 startClip = u_Frame.ViewProjection * vec4(start, 1.0);
	vec4 endClip = u_Frame.ViewProjection * vec4(start + toSun * reach, 1.0);
	if (!(startClip.w > 0.0))
		return vec3(0.0);
	vec2 startPixel = NDCToUV(startClip.xy / startClip.w) * u_Frame.ViewportSize.xy;
	float screenLength = endClip.w > 0.0 ? length(NDCToUV(endClip.xy / endClip.w) * u_Frame.ViewportSize.xy - startPixel) : 1e30;
	if (screenLength > u_ScreenShadows.ContactMaxPixels)
	{
		float shortened = u_ScreenShadows.ContactMaxPixels / screenLength;
		reach *= shortened;
		endClip = mix(startClip, endClip, shortened);
		screenLength = u_ScreenShadows.ContactMaxPixels;
	}
	uint steps = u_ScreenShadows.ContactSteps;
	float stepPixels = screenLength / float(steps);
	float offset = BlueNoise(pixel, ivec2(32, 17)); // Fixed per pixel, apart from the disk's turn
	float startViewDepth = -(u_Frame.View * vec4(start, 1.0)).z;
	float sunViewDepth = -(mat3(u_Frame.View) * toSun).z; // View depth gained per meter toward the sun
	bool orthographic = u_Frame.CameraForward.w > 0.5;
	float passedBehind = 0.0;
	ivec2 last = ivec2(u_Frame.ViewportSize.xy) - 1;

	for (uint first = 0u; first < steps; first += c_ContactBatch)
	{
		// The batch's steps: where they fall on screen, and the depth there. A step off the image (or behind the camera) ends
		// the march: the steps after it are off the image too.
		float alongs[c_ContactBatch];
		vec2 uvs[c_ContactBatch];
		ivec2 pixels[c_ContactBatch];
		float depths[c_ContactBatch];
		uint count = 0u;
		bool ended = false;
		for (uint k = 0u; k < c_ContactBatch; k++)
		{
			uint index = first + k;
			float t = (float(index) + offset) / float(steps);
			vec4 clip = mix(startClip, endClip, t);
			vec2 uv = NDCToUV(clip.xy / max(clip.w, 1e-6));
			bool onImage = clip.w > 0.0 && all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
			if (!ended && index < steps && !onImage)
				ended = true;
			bool taken = !ended && index < steps;
			count += taken ? 1u : 0u;
			alongs[k] = reach * t;
			uvs[k] = uv;
			pixels[k] = clamp(ivec2(uv * u_Frame.ViewportSize.xy), ivec2(0), last);
			depths[k] = taken ? texelFetch(u_Depth, pixels[k], 0).r : 0.0;
		}
		for (uint k = 0u; k < count; k++)
		{
			float along = alongs[k];
			if (along < 2.0 * pixelWorld || !(depths[k] > 0.0))
				continue; // The start's own neighborhood (terminators of coarse meshes), or nothing drawn there
			vec3 surface = PositionAt(pixels[k], depths[k]);
			float stepViewDepth = startViewDepth + sunViewDepth * along;
			float surfaceViewDepth = dot(surface, u_Frame.CameraForward.xyz);
			float footprint = orthographic ? pixelWorld : pixelWorld * surfaceViewDepth / viewDepth;
			if (stepViewDepth - surfaceViewDepth < -2.0 * footprint)
				continue;
			vec3 toStep = start + toSun * along - surface;
			float thickness = u_ScreenShadows.ContactThickness + u_ScreenShadows.ContactThicknessScale * stepViewDepth;
			float behind = -dot(FaceNormal(pixels[k], surface, depths[k]), toStep); // How far the step lies behind that surface
			if (behind > c_ContactMinBehind && behind < thickness)
			{
				// A face turned from the sun under a normal turned toward it: its ray entered its own mesh.
				if (dot(FaceNormal(pixel, position, depth), toSun) <= 0.0)
					return vec3(0.0);
				float t = (float(first + k) + offset) / float(steps);
				vec2 edge = clamp(min(uvs[k], 1.0 - uvs[k]) / 0.05, 0.0, 1.0);
				float fade = 1.0 - smoothstep(u_ScreenShadows.ContactFadeStart, u_ScreenShadows.ContactFadeEnd, viewDepth);
				return vec3(fade * edge.x * edge.y * (1.0 - smoothstep(0.6, 1.0, t)), stepPixels, 1.0);
			}
			if (behind >= thickness)
				passedBehind = 1.0;
		}
		if (ended)
			break;
	}
	return vec3(0.0, stepPixels, passedBehind);
}

void main()
{
	ivec2 pixel = ivec2(gl_FragCoord.xy);
	vec2 source = texelFetch(u_Source, pixel, 0).rg;
	float depth = texelFetch(u_Depth, pixel, 0).r;
	o_Mask = source;
	if (!(depth > 0.0) || !(source.r > 0.0))
		return; // Nothing drawn, or no sun left to take
	vec2 uv = (vec2(pixel) + 0.5) * u_Frame.ViewportSize.zw;
	vec4 point = u_Frame.InverseViewProjection * vec4(UVToNDC(uv), depth, 1.0);
	vec3 position = point.xyz / point.w;
	float viewDepth = -(u_Frame.View * vec4(position, 1.0)).z;
	vec3 normal = normalize(mat3(u_Frame.InverseView) * OctahedralDecode(texelFetch(u_Normals, pixel, 0).xy));
	float pixelWorld = 2.0 * (u_Frame.CameraForward.w > 0.5 ? 1.0 : viewDepth) / (u_Frame.Projection[1][1] * u_Frame.ViewportSize.y);
	vec3 contact = ContactShadow(pixel, position, depth, normal, viewDepth, pixelWorld);
	if (contact.z > 0.0)
		o_Mask = vec2(source.r * sqrt(1.0 - contact.x), max(source.g, clamp(0.5 * contact.y, 1.0, 4.0) / c_ScreenShadowMaxBlurPixels));
}
