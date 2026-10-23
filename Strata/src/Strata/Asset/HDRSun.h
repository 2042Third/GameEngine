#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The sun of an equirectangular HDR sky: where it is, how much light it gives and the texels it covers, so that it can
// become a directional light (lighting.extractSun) and leave the map (TextureImportSettings::RemoveSun). Pure functions
// over the decoded floats, before the importer resizes or clamps them.
//
// The map's texel (x, y) of a width x height image looks along the direction Include/IBL.glsl's DirectionToEquirect maps
// to it: columns go around +Y starting at -X (u = atan(z, x) / 2 pi + 0.5), rows from +Y (the top row) to -Y.
namespace Strata::HDRSun
{

	// The region around the brightest texel is at most this many degrees across (the real sun is 0.53 degrees; its glow
	// and a blurred or low-resolution capture spread it wider).
	constexpr float c_MaxDiameter = 3.0f;
	// The sky the sun is measured against: the median luminance of the texels this far from it (degrees).
	constexpr float c_BackgroundInner = 3.0f;
	constexpr float c_BackgroundOuter = 6.0f;
	// The brightest texel must be at least this many times the sky around it to be a sun.
	constexpr float c_MinContrast = 8.0f;
	// The sun's texels are those this many times brighter than the sky around it: its disk with the edges of its texels and
	// its glow, which would stay a bright ring in the sky if a higher threshold left them out.
	constexpr float c_RegionContrast = 4.0f;
	// A peak is flat (clipped in the file) when a quarter of the region or more, and at least c_MinFlatTexels texels, lies
	// within this fraction of the brightest texel: a sensor that saturated. A real sun's disk darkens toward its edge.
	constexpr float c_FlatTolerance = 0.01f;
	constexpr uint32_t c_MinFlatTexels = 3;

	// Linear RGBA floats, 4 per texel, rows from the top, in the decoder's own buffer: a large sky's floats take hundreds of
	// megabytes, so they are handed over rather than copied.
	struct Image
	{
		struct Deleter
		{
			void operator()(float* pixels) const;
		};

		uint32_t Width = 0;
		uint32_t Height = 0;
		std::unique_ptr<float[], Deleter> Data;

		std::span<const float> GetPixels() const { return { Data.get(), Data ? static_cast<size_t>(Width) * Height * 4 : 0 }; }
	};

	struct Sun
	{
		glm::vec3 Direction = glm::vec3(0.0f, 1.0f, 0.0f); // Toward the sun, in the map's frame, weighted by each texel's light
		// Per channel: the radiance of the region's texels times their solid angles, summed: the illuminance, in lux per nit
		// of the map, of a surface facing the sun.
		glm::vec3 Illuminance = glm::vec3(0.0f);
		glm::vec3 Color = glm::vec3(1.0f); // Illuminance over its largest channel
		float Luminance = 0.0f;            // The illuminance's luminance (Rec. 709)
		float Peak = 0.0f;                 // The brightest texel's luminance
		float Background = 0.0f;           // The sky's luminance around the sun (c_BackgroundInner to c_BackgroundOuter away)
		float Threshold = 0.0f;            // The region: texels above it (c_RegionContrast times Background)
		float AngularDiameter = 0.0f;      // Degrees across the region's texel centers, one texel added
		bool Clipped = false;              // The peak is flat: the file clipped the sun, and Illuminance is a lower bound
		std::vector<uint32_t> Region;      // The texels (y * width + x), at most c_MaxDiameter across
	};

	// The direction a texel's center looks along, and the solid angle (steradians) a texel of row y covers.
	glm::vec3 TexelDirection(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
	double TexelSolidAngle(uint32_t y, uint32_t width, uint32_t height);

	// The brightest texel (by luminance), the region connected to it above the threshold (wrapping at the seam and over the
	// poles) within c_MaxDiameter / 2 of its center, and what the region gives. Nullopt with the reason when the image has no
	// sun: nothing bright, or a peak below c_MinContrast times the sky around it. `pixels` holds width * height * 4 floats;
	// texels that are not finite or negative count as black.
	std::optional<Sun> Find(std::span<const float> pixels, uint32_t width, uint32_t height, std::string* outReason = nullptr);

	// Replaces the region's texels by the sky around them: each one the average of the region's border texels (those next
	// to it, not in it, at most Threshold bright), weighted by the inverse square of their distance in texels. Alpha stays.
	// Afterwards no texel of the region is above Threshold.
	void Remove(std::span<float> pixels, uint32_t width, uint32_t height, const Sun& sun);

	// Decodes an image file (Radiance HDR, or 8-bit images taken to linear values as the importer's HDR usage does) into
	// linear floats. Nullopt with the reason when it does not decode.
	std::optional<Image> Decode(std::span<const uint8_t> encoded, std::string* outError = nullptr);

}
