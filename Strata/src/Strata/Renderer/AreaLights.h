#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

namespace Strata
{

	// What a rect light's intensity states.
	enum class AreaLightUnits : uint8_t
	{
		Nits = 0, // The luminance of its face (cd/m^2)
		Lumens    // The luminous power the whole light emits
	};

}

namespace Strata::AreaLights
{

	// Rect lights shaded with linearly transformed cosines (Heitz, Dupuy, Hill and Neubelt, "Real-Time Polygonal-Light
	// Shading with Linearly Transformed Cosines", 2016): the published tables and the shading math on the CPU, in float and
	// in an order of operations a shader repeats exactly (the tables are read with four loads and bilinear weights computed
	// here, never through a filtering sampler), so tests can compute what the GPU must show.
	//
	// A GGX lobe is fitted by a clamped cosine under a linear transform of directions, so the fitted lobe's integral over a
	// polygon is the cosine's integral over the transformed polygon, and that one has a closed form: a sum over the
	// polygon's edges. How far the fit is from GGX: SpecularFraction.

	// The tables (LTCTable.inl, which Tools/ConvertLTCTable.py writes from the authors' fit): c_TableLayers layers of
	// c_TableSize x c_TableSize texels of four floats. Texel (a, t) is for the perceptual roughness a / 63 (GGX's alpha is
	// its square) seen from the direction whose cosine to the normal is 1 - (t / 63)^2.
	constexpr uint32_t c_TableSize = 64;
	constexpr uint32_t c_TableLayers = 2;
	constexpr uint32_t c_TableChannels = 4;
	// Layer 0, the lobe's inverse transform over its middle term: a texel (x, y, z, w) is the matrix of rows (x, 0, z),
	// (0, 1, 0), (y, 0, w) for directions in the frame (T1, T2, N), T1 being the view's direction along the surface.
	constexpr uint32_t c_TransformLayer = 0;
	// Layer 1: x the lobe's magnitude (GGX's directional albedo for F0 1), y its Fresnel term, z nothing (0), and w the
	// horizon term: the share of a vector form factor's length the receiver's horizon leaves, read at (the cosine between
	// the vector and the normal * 0.5 + 0.5, the length).
	constexpr uint32_t c_TermsLayer = 1;

	// Include/PBR.slang's constants the shading uses: the roughness under which no lobe is evaluated, and the clear coat's F0.
	constexpr float c_MinRoughness = 0.045f;
	constexpr float c_ClearcoatF0 = 0.04f;

	// A layer's values, texel after texel, row by row (c_TableSize * c_TableSize * c_TableChannels floats: what a texture of
	// the layer is filled with). Empty for a layer the tables do not have.
	std::span<const float> GetTableLayer(uint32_t layer);
	// A texel; indexes past the last texel read the last one.
	glm::vec4 LoadTable(uint32_t layer, uint32_t x, uint32_t y);
	// The layer between its texels: `uv` 0 is the first texel's center and 1 the last one's, values outside are clamped (a
	// NaN reads 0). Four loads weighted bilinearly.
	glm::vec4 SampleTable(uint32_t layer, const glm::vec2& uv);

	// A rect light as the shading takes it. Its face is Center +- AxisX +- AxisY (half axes, the sizes in them) and shines
	// toward Normal (unit); AxisY points along cross(AxisX, Normal), so the corners Center - AxisX - AxisY, + AxisX - AxisY,
	// + AxisX + AxisY, - AxisX + AxisY run clockwise seen from the front: the order the edge sums below count as positive.
	struct AreaLight
	{
		glm::vec3 Center = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f, 0.0f, -1.0f);
		glm::vec3 AxisX = glm::vec3(0.0f);
		glm::vec3 AxisY = glm::vec3(0.0f);
		glm::vec3 Radiance = glm::vec3(0.0f); // The face's luminance per channel: the color times nits
		float InverseRangeSquared = 0.0f;
		bool TwoSided = false;                // Both faces shine, each with Radiance
	};

	// What a rect light's transform and values come to: where its face is, what it emits and the sphere it reaches.
	struct Record
	{
		glm::vec3 Center = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f, 0.0f, -1.0f); // Unit, on the transformed -Z's side of the face
		glm::vec3 AxisX = glm::vec3(1.0f, 0.0f, 0.0f);   // Unit, along the transform's X
		glm::vec3 AxisY = glm::vec3(0.0f, 1.0f, 0.0f);   // Unit: cross(AxisX, Normal), whichever way the transform's Y points
		glm::vec2 HalfExtents = glm::vec2(0.0f);         // Along AxisX and AxisY, in world meters
		glm::vec3 Radiance = glm::vec3(0.0f);
		float Range = 0.0f;                              // How far from the face it lights, in meters
		float BoundsRadius = 0.0f;                       // Around Center: Range plus the face's half diagonal
		bool TwoSided = false;
		bool Drawn = false;
	};

	// A face smaller than this along either axis, after the transform's scale, is not drawn (meters).
	constexpr float c_MinSize = 0.001f;

	// The face's half sizes in the world: `halfExtents` times the lengths of the transform's X and Y axes.
	glm::vec2 GetWorldHalfExtents(const glm::mat4& world, const glm::vec2& halfExtents);
	// The face's luminance in nits for an intensity in `units`. A Lambertian face of area A and luminance L emits pi L A
	// lumens, and a two-sided light emits its power from both faces: L = P / (pi A), half of it when two-sided. Nits pass
	// unchanged whatever the size; lumens on a face of no area give 0. `halfExtents` in world meters (GetWorldHalfExtents).
	float GetLuminance(float intensity, AreaLightUnits units, const glm::vec2& halfExtents, bool twoSided);
	// The record of a light with the world transform `world` whose face spans +-`halfExtents` along its local X and Y and
	// shines along its local -Z. The transform's X axis is kept, the normal is perpendicular to its X and Y axes on its -Z
	// axis's side, and AxisY follows from the two, so a mirroring or shearing transform still gives the frame AreaLight
	// states. A sheared face (a turned light under an unevenly scaled parent: its X and Y axes meet at an angle a that is
	// not a right one) becomes the rectangle with its sides' lengths, X kept and Y turned in the face's plane until it is
	// perpendicular to X: 1 / sin(a) the parallelogram's area, and a light in nits emits that much more. Not drawn (the
	// default record) when it is inactive, a value is not finite (the radiance and the bounds radius it comes to included),
	// `luminance` or `range` is not above 0, the transform is degenerate or the face is smaller than c_MinSize.
	Record MakeRecord(const glm::mat4& world, bool active, const glm::vec3& color, float luminance, const glm::vec2& halfExtents, float range,
		bool twoSided);
	// The record as the shading takes it: the half sizes go into the axes. Shade it only when it is drawn.
	AreaLight LoadAreaLight(const Record& record);

	// The fit of theta / (2 pi sin(theta)) for an angle of at most a right angle, from its cosine (in [0, 1]): what an edge
	// seen under theta adds to the vector form factor, over the sine its cross product already holds.
	float GetEdgeFit(float cosine);
	// The vector form factor of the light's face seen from `position`: the sum over its edges of (the angle the edge is seen
	// under) / (2 pi) along the normal of the plane through the edge and the position. Its dot product with a receiver's
	// normal is the form factor (the share of the receiver's cosine-weighted hemisphere the face fills) while the whole face
	// is above the receiver's horizon. From the front it points toward the face, from behind away from it.
	glm::vec3 GetVectorFormFactor(const glm::vec3& position, const AreaLight& light);

	// The form factor of the light seen from a surface point as the shading computes it: the vector form factor with the
	// table's horizon term, which stands for a sphere of the same vector form factor. It is exact while that sphere's cap
	// is above the horizon, within 2% of the vector form factor's length (the most the face gives any receiver) while the
	// horizon cuts the cap but not the face, and an estimate where it cuts the face: off by 0.037 at most in the tests, by
	// up to 18% of that length for a face seen as a strip standing across the horizon. 0 behind a one-sided light and for
	// a position on the light's plane; within [0, 1].
	float FormFactor(const glm::vec3& position, const glm::vec3& normal, const AreaLight& light);
	// The same form factor with the face clipped by the horizon exactly, without the table.
	float FormFactorClipped(const glm::vec3& position, const glm::vec3& normal, const AreaLight& light);

	// The share of a clamped cosine lobe the light covers, for the lobe that the inverse transform `transform` (a texel of
	// layer 0) turns into the cosine around `normal`; `view` (unit, surface to viewer) orients the transform's frame. The
	// identity (1, 0, 0, 1) gives FormFactor, up to rounding.
	float GetLobeCoverage(const glm::vec4& transform, const glm::vec3& position, const glm::vec3& normal, const glm::vec3& view, const AreaLight& light);
	// The share of the specular lobe the light covers, within [0, 1]: GetLobeCoverage with the table's transform for the
	// perceptual `roughness` (at least Include/PBR.slang's c_MinRoughness) and the view's angle. Times the lobe's
	// reflectance it is the light's specular radiance over its luminance. Toward grazing views the fitted lobe departs from
	// GGX's, most at middling roughness, both ways:
	//   It keeps a part under the surface's horizon, which no light above it covers. A light filling the sky gives
	//   (1 + x / sqrt(x^2 + y^2)) / 2 of the transform's texel (x, y, z, w): 1 seen along the normal and for fully rough
	//   lobes; seen at a view cosine of 0.5, 0.2 and 0.05: 0.9997, 0.998 and 0.971 at roughness 0.1, 0.989, 0.939 and
	//   0.845 at 0.25, 0.940, 0.893 and 0.872 at 0.5, 0.967, 0.962 and 0.957 at 0.75.
	//   Faces centered on the mirror direction get more than GGX gives them: seen at a view cosine of 0.05, 1.1 to 1.4
	//   times as much at roughness 0.25, 1.6 to 2.4 times at 0.5 and 1.2 to 1.6 times at 0.75; at 0.19, 1.1 to 1.35 times
	//   at roughness 0.5; at 0.32, 1.06 to 1.16 times. At roughness 1 they get less: 0.36 to 0.7 times at 0.05.
	float SpecularFraction(const glm::vec3& position, const glm::vec3& normal, const glm::vec3& view, float roughness, const AreaLight& light);

	// The point of the light's face nearest to `position`.
	glm::vec3 ClosestPoint(const glm::vec3& position, const AreaLight& light);
	// The light's reach: 1 at the face, 0 from its range on, (1 - (d^2 / range^2)^2)^2 at the distance d from the face (the
	// window local lights fade with, without the inverse square: the form factor holds the distance already).
	float GetRangeWindow(const glm::vec3& position, const AreaLight& light);

	// What the shading reads of a surface (the shaders' SurfaceData) and of its clear coat (ClearcoatData).
	struct Surface
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f, 0.0f, 1.0f);
		glm::vec3 View = glm::vec3(0.0f, 0.0f, 1.0f);  // Surface to viewer, unit
		glm::vec3 Albedo = glm::vec3(1.0f);
		float Metallic = 0.0f;
		float Roughness = 1.0f;                        // Perceptual
		glm::vec3 F0 = glm::vec3(0.04f);
		glm::vec3 SpecularEnergy = glm::vec3(1.0f);    // GetSpecularEnergy(F0, dfg)
	};
	struct Coat
	{
		glm::vec3 Normal = glm::vec3(0.0f, 0.0f, 1.0f);
		float Strength = 0.0f;                         // 0: no coat
		float Roughness = 0.0f;
		float SpecularEnergy = 1.0f;                   // GetSpecularEnergy(c_ClearcoatF0, coatDFG)
	};

	// The radiance a surface reflects toward the viewer from the light, unshadowed, faded by the light's range: 0 behind a
	// one-sided light, on its plane and out of its range. `dfg` and `coatDFG` are the split-sum terms of the BRDF table at
	// the surface's and the coat's roughness and view angle.
	//   Diffuse: FormFactor * Albedo * (1 - Metallic) * (1 - Es) * diffuseWeight, with Es = F0 dfg.x + dfg.y, what the
	//   specular lobe reflects (the rule the image-based light follows).
	//   Specular: SpecularFraction * Es * SpecularEnergy * specularWeight.
	//   Under a coat the two lose what the coat reflects, Strength * (c_ClearcoatF0 coatDFG.x + coatDFG.y) * its
	//   SpecularEnergy, and the coat adds its own lobe's share times that reflectance and specularWeight.
	// A `specularWeight` of 0 (a reflection probe's capture) evaluates no specular lobe.
	glm::vec3 Shade(const Surface& surface, const Coat& coat, const glm::vec2& coatDFG, const glm::vec2& dfg, float diffuseWeight, const AreaLight& light,
		float specularWeight);

}
