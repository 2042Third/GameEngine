#include <doctest/doctest.h>

#include "Strata/Core/Window.h"

#include <glm/glm.hpp>

using namespace Strata;

// A window asked for in pixels (WindowSpecification::SizeInPixels: the editor's --window-size) has a framebuffer of whole
// screen coordinates times the display's scale: what MatchWindowPixels says of it decides what the editor tells the person
// when the pixels differ (HasRequestedPixels).
TEST_SUITE("Core.WindowPixels")
{
	TEST_CASE("A framebuffer of the pixels asked for matches at every scale")
	{
		CHECK(MatchWindowPixels({ 1600, 900 }, { 1600, 900 }, { 1600, 900 }) == WindowPixelsMatch::Exact);
		CHECK(MatchWindowPixels({ 1600, 900 }, { 1600, 900 }, { 800, 450 }) == WindowPixelsMatch::Exact);
		CHECK(MatchWindowPixels({ 1500, 900 }, { 1500, 900 }, { 1000, 600 }) == WindowPixelsMatch::Exact);
	}

	TEST_CASE("A size the display's scale cannot make gets the nearest, which is told apart from a shrunk window")
	{
		// A Retina display, 2 pixels to a point, makes only even sizes: 1601 x 901 pixels become 801 x 451 points.
		CHECK(MatchWindowPixels({ 1601, 901 }, { 1602, 902 }, { 801, 451 }) == WindowPixelsMatch::NearestOfScale);
		CHECK(MatchWindowPixels({ 1600, 901 }, { 1600, 902 }, { 800, 451 }) == WindowPixelsMatch::NearestOfScale);
		CHECK(MatchWindowPixels({ 1601, 900 }, { 1600, 900 }, { 800, 450 }) == WindowPixelsMatch::NearestOfScale);
		// A fractional scale (1.5) steps by one or two pixels.
		CHECK(MatchWindowPixels({ 1000, 1000 }, { 1001, 1001 }, { 667, 667 }) == WindowPixelsMatch::NearestOfScale);

		// Where a screen coordinate is a pixel (Windows), every size exists: another one is the system's doing.
		CHECK(MatchWindowPixels({ 1601, 901 }, { 1600, 900 }, { 1600, 900 }) == WindowPixelsMatch::Other);
		CHECK(MatchWindowPixels({ 65535, 65535 }, { 3844, 2130 }, { 3844, 2130 }) == WindowPixelsMatch::Other);
		// Shrunk to fit a Retina display: far more than a step from the request.
		CHECK(MatchWindowPixels({ 8000, 4000 }, { 3024, 1890 }, { 1512, 945 }) == WindowPixelsMatch::Other);
		CHECK(MatchWindowPixels({ 1600, 904 }, { 1600, 900 }, { 800, 450 }) == WindowPixelsMatch::Other);
		// A window of no size (minimized) has no scale to explain anything.
		CHECK(MatchWindowPixels({ 1600, 900 }, { 0, 0 }, { 0, 0 }) == WindowPixelsMatch::Other);
		CHECK(MatchWindowPixels({ 1600, 900 }, { 1600, 902 }, { 800, 0 }) == WindowPixelsMatch::Other);
	}
}
