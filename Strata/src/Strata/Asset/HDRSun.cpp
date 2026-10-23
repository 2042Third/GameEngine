#include "stpch.h"
#include "Strata/Asset/HDRSun.h"

#include "Strata/Core/Profiling.h"
#include "Strata/Core/StringUtils.h"

#include <spdlog/fmt/fmt.h>
#include <stb_image.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <deque>
#include <memory>
#include <numbers>
#include <unordered_set>
#include <utility>

namespace Strata::HDRSun
{

	namespace
	{

		// Decoding larger images would need more memory than any map is worth (as TextureImporter::Decode).
		constexpr uint64_t c_MaxDecodedPixels = 1ull << 28;
		// Fewer sky texels than this around the sun (a map of a few texels) measure the sky over the whole map instead.
		constexpr size_t c_MinBackgroundTexels = 8;

		float Channel(float value)
		{
			return std::isfinite(value) && value > 0.0f ? value : 0.0f;
		}

		glm::vec3 TexelColor(std::span<const float> pixels, size_t texel)
		{
			return glm::vec3(Channel(pixels[texel * 4]), Channel(pixels[texel * 4 + 1]), Channel(pixels[texel * 4 + 2]));
		}

		float Luminance(const glm::vec3& color)
		{
			return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
		}

		glm::dvec3 TexelDirectionDouble(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
		{
			const double azimuth = ((static_cast<double>(x) + 0.5) / static_cast<double>(width) - 0.5) * 2.0 * std::numbers::pi;
			const double polar = (static_cast<double>(y) + 0.5) / static_cast<double>(height) * std::numbers::pi;
			return glm::dvec3(std::sin(polar) * std::cos(azimuth), std::cos(polar), std::sin(polar) * std::sin(azimuth));
		}

		// The angle between two unit vectors, accurate for small angles (the chord's, not acos's).
		double AngleBetween(const glm::dvec3& a, const glm::dvec3& b)
		{
			return 2.0 * std::asin(std::min(glm::length(a - b) * 0.5, 1.0));
		}

		// The texels sharing an edge with one: left and right wrap around at the seam; above the top row and below the bottom
		// one lies the texel half way around the same row, over the pole.
		template<typename Visit>
		void ForEachNeighbor(uint32_t texel, uint32_t width, uint32_t height, Visit&& visit)
		{
			const uint32_t x = texel % width;
			const uint32_t y = texel / width;
			visit(y * width + (x + width - 1) % width);
			visit(y * width + (x + 1) % width);
			visit(y > 0 ? (y - 1) * width + x : y * width + (x + width / 2) % width);
			visit(y + 1 < height ? (y + 1) * width + x : y * width + (x + width / 2) % width);
		}

		// The rows within `degrees` of a polar angle (radians), clamped to the image.
		std::pair<uint32_t, uint32_t> RowsAround(double polar, double degrees, uint32_t height)
		{
			const double radians = degrees * std::numbers::pi / 180.0;
			const double rowsPerRadian = static_cast<double>(height) / std::numbers::pi;
			const double first = std::floor((polar - radians) * rowsPerRadian);
			const double last = std::ceil((polar + radians) * rowsPerRadian);
			return { static_cast<uint32_t>(std::clamp(first, 0.0, static_cast<double>(height - 1))),
				static_cast<uint32_t>(std::clamp(last, 0.0, static_cast<double>(height - 1))) };
		}

		float Median(std::vector<float>& values)
		{
			const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
			std::nth_element(values.begin(), middle, values.end());
			return *middle;
		}

	}

	void Image::Deleter::operator()(float* pixels) const
	{
		stbi_image_free(pixels);
	}

	glm::vec3 TexelDirection(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
	{
		return glm::vec3(TexelDirectionDouble(x, y, width, height));
	}

	double TexelSolidAngle(uint32_t y, uint32_t width, uint32_t height)
	{
		const double top = static_cast<double>(y) / static_cast<double>(height) * std::numbers::pi;
		const double bottom = static_cast<double>(y + 1) / static_cast<double>(height) * std::numbers::pi;
		return 2.0 * std::numbers::pi / static_cast<double>(width) * (std::cos(top) - std::cos(bottom));
	}

	std::optional<Sun> Find(std::span<const float> pixels, uint32_t width, uint32_t height, std::string* outReason)
	{
		ST_PROFILE_FUNCTION();
		auto fail = [outReason](std::string reason) -> std::optional<Sun>
		{
			if (outReason)
				*outReason = std::move(reason);
			return std::nullopt;
		};
		const size_t texels = static_cast<size_t>(width) * height;
		if (width == 0 || height == 0 || pixels.size() < texels * 4)
			return fail("the image is empty");

		// The brightest texel (the first of equals).
		size_t peakTexel = 0;
		float peak = 0.0f;
		for (size_t texel = 0; texel < texels; texel++)
		{
			const float luminance = Luminance(TexelColor(pixels, texel));
			if (luminance > peak)
			{
				peak = luminance;
				peakTexel = texel;
			}
		}
		if (!(peak > 0.0f) || !std::isfinite(peak))
			return fail("the image has no bright texel");
		const uint32_t peakX = static_cast<uint32_t>(peakTexel % width);
		const uint32_t peakY = static_cast<uint32_t>(peakTexel / width);
		const glm::dvec3 peakDirection = TexelDirectionDouble(peakX, peakY, width, height);
		const double peakPolar = (static_cast<double>(peakY) + 0.5) / static_cast<double>(height) * std::numbers::pi;

		// The sky around it: the median of a ring of texels, far enough out to leave the sun's glow aside.
		std::vector<float> ring;
		const double inner = c_BackgroundInner * std::numbers::pi / 180.0;
		const double outer = c_BackgroundOuter * std::numbers::pi / 180.0;
		const auto [ringFirst, ringLast] = RowsAround(peakPolar, c_BackgroundOuter, height);
		for (uint32_t y = ringFirst; y <= ringLast; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				const double angle = AngleBetween(TexelDirectionDouble(x, y, width, height), peakDirection);
				if (angle >= inner && angle <= outer)
					ring.push_back(Luminance(TexelColor(pixels, static_cast<size_t>(y) * width + x)));
			}
		}
		if (ring.size() < c_MinBackgroundTexels)
		{
			ring.clear();
			ring.reserve(texels);
			for (size_t texel = 0; texel < texels; texel++)
				ring.push_back(Luminance(TexelColor(pixels, texel)));
		}
		const float background = Median(ring);
		if (peak < c_MinContrast * background)
			return fail(fmt::format("its brightest texel is only {:.1f} times as bright as the sky around it (a sun is at least {:g} times)",
				peak / background, c_MinContrast));

		Sun sun;
		sun.Peak = peak;
		sun.Background = background;
		sun.Threshold = c_RegionContrast * background;

		// The texels above the threshold connected to the peak, within the largest diameter of it; then those within half of
		// it of their center (a flat peak's first texel may lie at its edge).
		const double maxRadius = c_MaxDiameter * 0.5 * std::numbers::pi / 180.0;
		std::vector<uint32_t> connected;
		std::unordered_set<uint32_t> visited;
		std::deque<uint32_t> queue;
		queue.push_back(static_cast<uint32_t>(peakTexel));
		visited.insert(static_cast<uint32_t>(peakTexel));
		glm::dvec3 weightedDirection(0.0);
		while (!queue.empty())
		{
			const uint32_t texel = queue.front();
			queue.pop_front();
			connected.push_back(texel);
			const uint32_t x = texel % width;
			const uint32_t y = texel / width;
			weightedDirection += TexelDirectionDouble(x, y, width, height)
				* (static_cast<double>(Luminance(TexelColor(pixels, texel))) * TexelSolidAngle(y, width, height));
			ForEachNeighbor(texel, width, height, [&](uint32_t neighbor)
			{
				if (!visited.insert(neighbor).second)
					return;
				if (Luminance(TexelColor(pixels, neighbor)) <= sun.Threshold)
					return;
				if (AngleBetween(TexelDirectionDouble(neighbor % width, neighbor / width, width, height), peakDirection) > 2.0 * maxRadius)
					return;
				queue.push_back(neighbor);
			});
		}
		const glm::dvec3 center = glm::length(weightedDirection) > 0.0 ? glm::normalize(weightedDirection) : peakDirection;

		glm::dvec3 illuminance(0.0);
		glm::dvec3 direction(0.0);
		double largestAngle = 0.0;
		uint32_t flat = 0;
		for (const uint32_t texel : connected)
		{
			const uint32_t x = texel % width;
			const uint32_t y = texel / width;
			const glm::dvec3 texelDirection = TexelDirectionDouble(x, y, width, height);
			const double angle = AngleBetween(texelDirection, center);
			if (angle > maxRadius && texel != peakTexel)
				continue;
			sun.Region.push_back(texel);
			const glm::vec3 color = TexelColor(pixels, texel);
			const double solidAngle = TexelSolidAngle(y, width, height);
			illuminance += glm::dvec3(color) * solidAngle;
			direction += texelDirection * (static_cast<double>(Luminance(color)) * solidAngle);
			largestAngle = std::max(largestAngle, angle);
			if (Luminance(color) >= (1.0f - c_FlatTolerance) * peak)
				flat++;
		}
		sun.Direction = glm::vec3(glm::length(direction) > 0.0 ? glm::normalize(direction) : peakDirection);
		sun.Illuminance = glm::vec3(illuminance);
		sun.Luminance = Luminance(sun.Illuminance);
		const float largest = std::max({ sun.Illuminance.r, sun.Illuminance.g, sun.Illuminance.b });
		sun.Color = largest > 0.0f ? sun.Illuminance / largest : glm::vec3(1.0f);
		sun.AngularDiameter = static_cast<float>(2.0 * largestAngle * 180.0 / std::numbers::pi + 180.0 / static_cast<double>(height));
		sun.Clipped = flat >= c_MinFlatTexels && static_cast<size_t>(flat) * 4 >= sun.Region.size();
		return sun;
	}

	void Remove(std::span<float> pixels, uint32_t width, uint32_t height, const Sun& sun)
	{
		ST_PROFILE_FUNCTION();
		const size_t texels = static_cast<size_t>(width) * height;
		if (width == 0 || height == 0 || pixels.size() < texels * 4 || sun.Region.empty())
			return;
		const std::unordered_set<uint32_t> region(sun.Region.begin(), sun.Region.end());

		// The sky that stays around the sun: the texels next to the region that are not part of it and at most as bright as
		// its threshold (a region cut off at its largest diameter may have brighter ones beside it).
		std::vector<uint32_t> border;
		std::unordered_set<uint32_t> seen;
		for (const uint32_t texel : sun.Region)
		{
			ForEachNeighbor(texel, width, height, [&](uint32_t neighbor)
			{
				if (region.contains(neighbor) || !seen.insert(neighbor).second)
					return;
				if (Luminance(TexelColor(pixels, neighbor)) <= sun.Threshold)
					border.push_back(neighbor);
			});
		}
		std::vector<glm::dvec3> borderDirections;
		std::vector<glm::dvec3> borderColors;
		for (const uint32_t texel : border)
		{
			borderDirections.push_back(TexelDirectionDouble(texel % width, texel / width, width, height));
			borderColors.push_back(glm::dvec3(TexelColor(pixels, texel)));
		}

		std::vector<glm::vec3> fills;
		fills.reserve(sun.Region.size());
		for (const uint32_t texel : sun.Region)
		{
			const glm::vec3 color = TexelColor(pixels, texel);
			if (border.empty())
			{
				// Nothing around it (a map of a texel or two): the texel keeps its color at the threshold's brightness.
				const float luminance = Luminance(color);
				fills.push_back(luminance > sun.Threshold ? color * (sun.Threshold / luminance) : color);
				continue;
			}
			const glm::dvec3 direction = TexelDirectionDouble(texel % width, texel / width, width, height);
			glm::dvec3 sum(0.0);
			double weights = 0.0;
			for (size_t index = 0; index < border.size(); index++)
			{
				const glm::dvec3 offset = borderDirections[index] - direction;
				const double weight = 1.0 / std::max(glm::dot(offset, offset), 1.0e-20);
				sum += borderColors[index] * weight;
				weights += weight;
			}
			fills.push_back(glm::vec3(sum / weights));
		}
		// Written once every fill is known: each one reads only the border, never another filled texel.
		for (size_t index = 0; index < sun.Region.size(); index++)
		{
			const size_t texel = sun.Region[index];
			pixels[texel * 4] = fills[index].r;
			pixels[texel * 4 + 1] = fills[index].g;
			pixels[texel * 4 + 2] = fills[index].b;
		}
	}

	std::optional<Image> Decode(std::span<const uint8_t> encoded, std::string* outError)
	{
		auto fail = [outError](std::string message) -> std::optional<Image>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};
		if (encoded.empty() || encoded.size() > static_cast<size_t>(INT_MAX))
			return fail("the image is empty or too large");
		const stbi_uc* data = encoded.data();
		const int length = static_cast<int>(encoded.size());
		int width = 0;
		int height = 0;
		int channels = 0;
		if (!stbi_info_from_memory(data, length, &width, &height, &channels))
		{
			const char* reason = stbi_failure_reason();
			return fail(fmt::format("unsupported or corrupt image ({})", reason ? StringUtils::MakeValidUTF8(reason) : std::string("unknown error")));
		}
		if (width <= 0 || height <= 0 || static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > c_MaxDecodedPixels)
			return fail(fmt::format("image size {}x{} is not supported", width, height));
		int decodedWidth = 0;
		int decodedHeight = 0;
		std::unique_ptr<float[], Image::Deleter> decoded(stbi_loadf_from_memory(data, length, &decodedWidth, &decodedHeight, &channels, 4));
		if (!decoded)
		{
			const char* reason = stbi_failure_reason();
			return fail(fmt::format("decoding failed ({})", reason ? StringUtils::MakeValidUTF8(reason) : std::string("unknown error")));
		}
		if (decodedWidth != width || decodedHeight != height)
			return fail("decoded size differs from the image header");
		Image image;
		image.Width = static_cast<uint32_t>(width);
		image.Height = static_cast<uint32_t>(height);
		image.Data = std::move(decoded);
		return image;
	}

}
