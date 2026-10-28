#include "Strata/Renderer/ColorLookup.h"

#include <doctest/doctest.h>
#include <glm/gtc/packing.hpp>

#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace Strata;

// The color lookup asset: its cooked form, its validation and the trilinear lookup the GPU and the importer share.
namespace
{

	// A lattice whose colors are no simple function of their position: every channel differs from its neighbors.
	std::vector<glm::vec3> MakeGrade(uint32_t size)
	{
		std::vector<glm::vec3> colors = ColorLookup::MakeIdentity(size);
		for (size_t index = 0; index < colors.size(); index++)
		{
			const glm::vec3 c = colors[index];
			colors[index] = glm::vec3(c.r * c.r, 0.25f + 0.5f * c.b, 1.0f - c.g * 0.75f) + 0.01f * static_cast<float>(index % 7);
		}
		return colors;
	}

}

TEST_SUITE("Renderer.ColorLookup")
{
	TEST_CASE("A table round-trips through its cooked form at half precision")
	{
		const std::vector<glm::vec3> colors = MakeGrade(5);
		std::string error;
		const std::vector<uint8_t> cooked = ColorLookup::Serialize(5, colors, &error);
		REQUIRE_MESSAGE(!cooked.empty(), error);
		CHECK(cooked.size() == ColorLookup::c_HeaderBytes + 125 * 8);
		CHECK(std::memcmp(cooked.data(), "STLU", 4) == 0);
		const Ref<ColorLookup> lookup = ColorLookup::Deserialize(cooked, &error);
		REQUIRE_MESSAGE(lookup, error);
		CHECK(lookup->GetType() == AssetType::ColorLookup);
		CHECK(lookup->GetSize() == 5);
		const std::vector<glm::vec3> stored = lookup->GetColors();
		REQUIRE(stored.size() == colors.size());
		for (size_t index = 0; index < colors.size(); index++)
		{
			for (int channel = 0; channel < 3; channel++)
				CHECK(stored[index][channel] == glm::unpackHalf1x16(glm::packHalf1x16(colors[index][channel])));
		}
		// Create keeps what Serialize writes; its memory is the texels until a renderer takes them.
		const Ref<ColorLookup> created = ColorLookup::Create(5, colors, &error);
		REQUIRE(created);
		CHECK(created->GetColors() == stored);
		CHECK(created->GetMemoryUsage().Cpu == 125 * 8);
		CHECK(created->GetMemoryUsage().GpuTextures == 0);
		CHECK(created->GetTexture() == nullptr);
	}

	TEST_CASE("Tables of other sizes, counts or values are refused with the reason")
	{
		std::string error;
		CHECK_FALSE(ColorLookup::Create(1, ColorLookup::MakeIdentity(2), &error));
		CHECK(error.find("not 1") != std::string::npos);
		CHECK_FALSE(ColorLookup::Create(66, std::vector<glm::vec3>(66 * 66 * 66), &error));
		CHECK(error.find("not 66") != std::string::npos);
		CHECK_FALSE(ColorLookup::Create(3, ColorLookup::MakeIdentity(2), &error));
		CHECK(error.find("has 27 colors, not 8") != std::string::npos);
		std::vector<glm::vec3> colors = ColorLookup::MakeIdentity(2);
		colors[3].g = std::nanf("");
		CHECK_FALSE(ColorLookup::Create(2, colors, &error));
		CHECK(error.find("color 3") != std::string::npos);
		colors[3].g = 1.0e6f; // Beyond a half float's range
		CHECK(ColorLookup::Serialize(2, colors, &error).empty());
		CHECK(ColorLookup::Create(65, ColorLookup::MakeIdentity(65), &error));
	}

	TEST_CASE("Truncated, padded, corrupted and foreign bytes are rejected")
	{
		const std::vector<uint8_t> cooked = ColorLookup::Serialize(2, MakeGrade(2));
		REQUIRE(ColorLookup::Deserialize(cooked));
		// Every truncation, and a byte too many.
		for (size_t length = 0; length < cooked.size(); length++)
		{
			std::string error;
			CHECK_FALSE(ColorLookup::Deserialize(std::span<const uint8_t>(cooked.data(), length), &error));
			CHECK_FALSE(error.empty());
		}
		std::vector<uint8_t> padded = cooked;
		padded.push_back(0);
		CHECK_FALSE(ColorLookup::Deserialize(padded));
		// Every bit of the header.
		for (size_t byte = 0; byte < ColorLookup::c_HeaderBytes; byte++)
		{
			for (int bit = 0; bit < 8; bit++)
			{
				std::vector<uint8_t> flipped = cooked;
				flipped[byte] ^= static_cast<uint8_t>(1u << bit);
				CAPTURE(byte);
				CAPTURE(bit);
				CHECK_FALSE(ColorLookup::Deserialize(flipped));
			}
		}
		// A point that is infinite or not a number.
		for (const uint16_t half : { uint16_t(0x7C00), uint16_t(0xFC00), uint16_t(0x7E00) })
		{
			std::vector<uint8_t> broken = cooked;
			std::memcpy(broken.data() + ColorLookup::c_HeaderBytes + 3 * 8 + 2, &half, sizeof(half));
			std::string error;
			CHECK_FALSE(ColorLookup::Deserialize(broken, &error));
			CHECK(error.find("point 3 is not finite") != std::string::npos);
		}
		// A table of the largest size claimed by a small file, and garbage.
		std::vector<uint8_t> claimed = cooked;
		const uint32_t size = ColorLookup::c_MaxSize;
		std::memcpy(claimed.data() + 8, &size, sizeof(size));
		CHECK_FALSE(ColorLookup::Deserialize(claimed));
		std::mt19937 random(0xC0105u);
		for (int attempt = 0; attempt < 200; attempt++)
		{
			std::vector<uint8_t> garbage(random() % 200);
			for (uint8_t& value : garbage)
				value = static_cast<uint8_t>(random());
			CHECK_FALSE(ColorLookup::Deserialize(garbage));
		}
	}

	TEST_CASE("The lookup is the lattice at its points and trilinear between them")
	{
		const uint32_t size = 4;
		const std::vector<glm::vec3> grade = MakeGrade(size);
		auto at = [&](uint32_t red, uint32_t green, uint32_t blue) { return grade[(blue * size + green) * size + red]; };
		for (uint32_t blue = 0; blue < size; blue++)
		{
			for (uint32_t green = 0; green < size; green++)
			{
				for (uint32_t red = 0; red < size; red++)
				{
					const glm::vec3 point = glm::vec3(red, green, blue) / 3.0f;
					const glm::vec3 sampled = ColorLookup::Sample(size, grade, point);
					CHECK(glm::all(glm::lessThan(glm::abs(sampled - at(red, green, blue)), glm::vec3(1e-6f))));
				}
			}
		}
		// Halfway between two points on one axis, and in a cell's middle.
		const glm::vec3 edge = ColorLookup::Sample(size, grade, glm::vec3(0.5f / 3.0f, 0.0f, 1.0f));
		const glm::vec3 edgeExpected = 0.5f * (at(0, 0, 3) + at(1, 0, 3));
		CHECK(glm::all(glm::lessThan(glm::abs(edge - edgeExpected), glm::vec3(1e-6f))));
		const glm::vec3 center = ColorLookup::Sample(size, grade, glm::vec3(1.5f / 3.0f));
		glm::vec3 centerExpected(0.0f);
		for (uint32_t corner = 0; corner < 8; corner++)
			centerExpected += 0.125f * at(1 + (corner & 1u), 1 + ((corner >> 1) & 1u), 1 + ((corner >> 2) & 1u));
		CHECK(glm::all(glm::lessThan(glm::abs(center - centerExpected), glm::vec3(1e-6f))));
		// Outside [0, 1] the edge's points; the identity maps every color to itself.
		CHECK(ColorLookup::Sample(size, grade, glm::vec3(-1.0f, 2.0f, 0.0f)) == at(0, 3, 0));
		const std::vector<glm::vec3> identity = ColorLookup::MakeIdentity(33);
		for (const glm::vec3 color : { glm::vec3(0.1f, 0.5f, 0.9f), glm::vec3(0.0f), glm::vec3(1.0f), glm::vec3(0.123f, 0.777f, 0.5f) })
			CHECK(glm::all(glm::lessThan(glm::abs(ColorLookup::Sample(33, identity, color) - color), glm::vec3(1e-6f))));
	}
}
