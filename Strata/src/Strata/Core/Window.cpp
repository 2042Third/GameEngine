#include "stpch.h"
#include "Strata/Core/Window.h"

#include <cmath>

namespace Strata
{

	WindowPixelsMatch MatchWindowPixels(const glm::uvec2& requested, const glm::uvec2& framebuffer, const glm::uvec2& windowSize)
	{
		if (framebuffer == requested)
			return WindowPixelsMatch::Exact;
		if (windowSize.x == 0 || windowSize.y == 0)
			return WindowPixelsMatch::Other;
		for (glm::length_t axis = 0; axis < 2; axis++)
		{
			// Pixels to a screen coordinate: a window can only be a whole number of coordinates, so where this is more than 1
			// the framebuffer comes in steps of it, and one less than a step away from the request is the nearest it makes.
			const double scale = static_cast<double>(framebuffer[axis]) / static_cast<double>(windowSize[axis]);
			const double difference = std::abs(static_cast<double>(framebuffer[axis]) - static_cast<double>(requested[axis]));
			if (difference > 0.0 && (scale <= 1.0 || difference >= scale))
				return WindowPixelsMatch::Other;
		}
		return WindowPixelsMatch::NearestOfScale;
	}

}
