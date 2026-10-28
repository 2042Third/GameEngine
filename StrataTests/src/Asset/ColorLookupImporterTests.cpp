#include <doctest/doctest.h>

#include "Strata/Asset/ColorLookupImporter.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Renderer/ColorLookup.h"
#include "TestHelpers.h"

#include <atomic>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

// .cube files into color lookups: the parser's reading and refusals, the resampling and the importer's settings.
namespace
{

	std::span<const uint8_t> Bytes(std::string_view text)
	{
		return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
	}

	// A .cube file of `size`^3 values from `color`(red, green, blue), each point's input i / (size - 1), with `header` first.
	template<typename ColorFunction>
	std::string MakeCube(uint32_t size, ColorFunction color, std::string_view header = "")
	{
		std::string text(header);
		text += "LUT_3D_SIZE " + std::to_string(size) + "\n";
		for (uint32_t blue = 0; blue < size; blue++)
		{
			for (uint32_t green = 0; green < size; green++)
			{
				for (uint32_t red = 0; red < size; red++)
				{
					const glm::vec3 value = color(glm::vec3(red, green, blue) / static_cast<float>(size - 1));
					text += fmt::format("{:.6f} {:.6f} {:.6f}\n", value.r, value.g, value.b);
				}
			}
		}
		return text;
	}

	glm::vec3 Warm(glm::vec3 c)
	{
		return glm::vec3(c.r + 0.2f * c.r * (1.0f - c.r), c.g * c.b, 1.0f - 0.5f * c.b);
	}

	std::string ParseError(std::string_view text)
	{
		std::string error;
		CHECK_FALSE(ParseCubeFile(Bytes(text), &error).has_value());
		return error;
	}

}

TEST_SUITE("Asset.ColorLookupImporter")
{
	TEST_CASE("A .cube file's title, domain and values are read whatever its line endings and comments")
	{
		const std::string text = "\xEF\xBB\xBF# Made by a grading tool\r\nTITLE \"Warm # look\"\r\n\r\n  LUT_3D_SIZE 2 # two points each way\r\n"
			"DOMAIN_MIN 0 0 0\r\nDOMAIN_MAX 1.0 1.0 1.0\r\n"
			"0 0 0\r\n1 0 0\r\n0 1 0\r\n1 1 0\r\n0 0 1\r\n1e0 0 1\r\n0 +1 1.\r\n.5 1.0E0 1\r\n";
		std::string error;
		const std::optional<CubeTable> table = ParseCubeFile(Bytes(text), &error);
		REQUIRE_MESSAGE(table, error);
		CHECK(table->Title == "Warm # look");
		CHECK(table->Size == 2);
		CHECK(table->DomainMin == glm::vec3(0.0f));
		CHECK(table->DomainMax == glm::vec3(1.0f));
		REQUIRE(table->Colors.size() == 8);
		CHECK(table->Colors[1] == glm::vec3(1.0f, 0.0f, 0.0f));
		CHECK(table->Colors[5] == glm::vec3(1.0f, 0.0f, 1.0f));
		CHECK(table->Colors[6] == glm::vec3(0.0f, 1.0f, 1.0f));
		CHECK(table->Colors[7] == glm::vec3(0.5f, 1.0f, 1.0f));
		CHECK(table->Warnings.empty());

		// Numbers read exactly as their decimals round, whatever the C locale; unknown keywords are ignored with a warning.
		const std::string precise = MakeCube(2, [](glm::vec3 c) { return c * 0.123456789f; }, "LUT_3D_INPUT_RANGE -0.5 1.5\nLUT_IN_VIDEO_RANGE\n");
		const std::optional<CubeTable> ranged = ParseCubeFile(Bytes(precise), &error);
		REQUIRE_MESSAGE(ranged, error);
		CHECK(ranged->DomainMin == glm::vec3(-0.5f));
		CHECK(ranged->DomainMax == glm::vec3(1.5f));
		CHECK(glm::all(glm::lessThan(glm::abs(ranged->Colors[7] - glm::vec3(0.123457f)), glm::vec3(1e-7f))));
		REQUIRE(ranged->Warnings.size() == 1);
		CHECK(ranged->Warnings[0] == "line 2: the keyword LUT_IN_VIDEO_RANGE is not known and was ignored");
	}

	TEST_CASE("Malformed files are refused with the line and the reason")
	{
		const std::string valid = MakeCube(2, Warm);
		REQUIRE(ParseCubeFile(Bytes(valid), nullptr));
		CHECK(ParseError("LUT_1D_SIZE 1024\n0 0 0\n").starts_with("line 1: a 1D table (LUT_1D_SIZE)"));
		CHECK(ParseError("TITLE \"No size\"\n") == "no LUT_3D_SIZE: not a 3D .cube table");
		CHECK(ParseError("0 0 0\nLUT_3D_SIZE 2\n") == "line 1: the value 0 0 0 comes before LUT_3D_SIZE");
		CHECK(ParseError("LUT_3D_SIZE 1\n") == "line 1: LUT_3D_SIZE 1 is outside 2 to 65");
		CHECK(ParseError("LUT_3D_SIZE 66\n") == "line 1: LUT_3D_SIZE 66 is outside 2 to 65");
		CHECK(ParseError("LUT_3D_SIZE 2.5\n") == "line 1: LUT_3D_SIZE needs a whole number, not 2.5");
		CHECK(ParseError("LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n") == "line 2: LUT_3D_SIZE is given twice");
		CHECK(ParseError("LUT_3D_SIZE 2\n0 0 0\n1 0\n") == "line 3: expected three numbers, found 1 0");
		CHECK(ParseError("LUT_3D_SIZE 2\n0 0 0 0\n") == "line 2: expected three numbers, found 0 0 0 0");
		CHECK(ParseError("LUT_3D_SIZE 2\n0 0 0x1\n") == "line 2: expected three numbers, found 0 0 0x1");
		CHECK(ParseError("LUT_3D_SIZE 2\n0,5 0 0\n") == "line 2: expected three numbers, found 0,5 0 0");
		CHECK(ParseError("LUT_3D_SIZE 2\n1e999 0 0\n") == "line 2: expected three numbers, found 1e999 0 0");
		CHECK(ParseError("LUT_3D_SIZE 2\n0 0 0\nDOMAIN_MIN 0 0 0\n") == "line 3: the keyword DOMAIN_MIN comes after the table's values");
		CHECK(ParseError("DOMAIN_MAX 1 1\nLUT_3D_SIZE 2\n") == "line 1: DOMAIN_MAX needs three numbers, not 1 1");
		CHECK(ParseError("LUT_3D_INPUT_RANGE 0 1\nDOMAIN_MAX 1 1 1\n") == "line 2: DOMAIN_MAX and LUT_3D_INPUT_RANGE both give the domain");
		CHECK(ParseError(valid + "0 0 0\n") == "line 10: more than the 2^3 values LUT_3D_SIZE gives");
		std::string truncated = MakeCube(3, Warm);
		truncated.resize(truncated.rfind('\n', truncated.size() - 2) + 1); // Without its last value
		CHECK(ParseError(truncated) == "line 27: the table ends after 26 of its 3^3 values");
		CHECK(ParseError(MakeCube(2, Warm, "DOMAIN_MIN 0 0.5 0\nDOMAIN_MAX 1 0.5 1\n")).starts_with("the domain is empty"));
		// Bytes that are no UTF-8 are quoted as valid text.
		const std::string quoted = ParseError("LUT_3D_SIZE 2\n\xFF\xFE 0 0\n");
		CHECK(quoted == "line 2: expected three numbers, found \xEF\xBF\xBD\xEF\xBF\xBD 0 0");
	}

	TEST_CASE("Resampling keeps a table of its size, refines exactly and maps the file's domain")
	{
		const std::optional<CubeTable> table = ParseCubeFile(Bytes(MakeCube(5, Warm)), nullptr);
		REQUIRE(table);
		CHECK(ResampleCubeTable(*table, 5) == table->Colors);
		// A lattice of (5 - 1) * 4 + 1 points interpolates the same function trilinearly: its points are the coarse lookup's.
		const std::vector<glm::vec3> fine = ResampleCubeTable(*table, 17);
		REQUIRE(fine.size() == 17u * 17u * 17u);
		for (const glm::vec3 color : { glm::vec3(0.1f, 0.6f, 0.95f), glm::vec3(0.33f, 0.0f, 0.5f), glm::vec3(0.77f, 0.51f, 0.02f) })
		{
			const glm::vec3 coarse = ColorLookup::Sample(5, table->Colors, color);
			const glm::vec3 refined = ColorLookup::Sample(17, fine, color);
			CHECK(glm::all(glm::lessThan(glm::abs(coarse - refined), glm::vec3(1e-5f))));
		}
		// A domain of [0, 2]: the input 1 is the table's middle, and inputs past the domain's minimum clamp to its first point.
		const std::optional<CubeTable> wide = ParseCubeFile(Bytes(MakeCube(3, Warm, "DOMAIN_MIN -1 -1 -1\nDOMAIN_MAX 1 1 1\n")), nullptr);
		REQUIRE(wide);
		const std::vector<glm::vec3> mapped = ResampleCubeTable(*wide, 3);
		REQUIRE(mapped.size() == 27);
		CHECK(mapped[0] == wide->Colors[13]); // Input 0: the middle of [-1, 1]
		CHECK(mapped[26] == wide->Colors[26]);
		// A cancelled resampling gives nothing.
		std::atomic<bool> cancel { true };
		CHECK(ResampleCubeTable(*table, 33, &cancel).empty());
		CHECK_FALSE(ParseCubeFile(Bytes(MakeCube(17, Warm)), nullptr, &cancel).has_value());
	}

	TEST_CASE("The importer stores a lookup of the size its setting chooses")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ColorLookupImport");
		const std::filesystem::path source = directory / "Warm.cube";
		REQUIRE(FileSystem::WriteText(source, MakeCube(17, Warm, "TITLE \"Warm\"\n")));
		const ColorLookupImporter importer;
		CHECK(importer.GetDefaultSettings(source) == nlohmann::json { { "Size", "33" } });

		auto import = [&](const nlohmann::json& settings, std::string& error) -> Ref<ColorLookup>
		{
			ImportContext context;
			context.Handle = UUID(0xC0105ull);
			context.SourcePath = source;
			context.AssetDirectory = directory;
			context.Settings = settings;
			ImportResult result;
			if (!importer.Import(context, result, &error))
				return nullptr;
			CHECK(result.Warnings.empty());
			return ColorLookup::Deserialize(result.Data, &error);
		};
		std::string error;
		const Ref<ColorLookup> standard = import(importer.GetDefaultSettings(source), error);
		REQUIRE_MESSAGE(standard, error);
		CHECK(standard->GetSize() == 33);
		const Ref<ColorLookup> own = import({ { "Size", "File" } }, error);
		REQUIRE_MESSAGE(own, error);
		CHECK(own->GetSize() == 17);
		const Ref<ColorLookup> large = import({ { "Size", "65" } }, error);
		REQUIRE_MESSAGE(large, error);
		CHECK(large->GetSize() == 65);
		// The refinement keeps the grade: the stored points of 33 are the file's lookup at their inputs, within half precision.
		const std::optional<CubeTable> table = ParseCubeFile(Bytes(MakeCube(17, Warm)), nullptr);
		REQUIRE(table);
		const std::vector<glm::vec3> stored = standard->GetColors();
		for (const uint32_t index : { 0u, 1000u, 20000u, 35936u })
		{
			const glm::vec3 input = glm::vec3(index % 33, (index / 33) % 33, index / (33 * 33)) / 32.0f;
			CHECK(glm::all(glm::lessThan(glm::abs(stored[index] - ColorLookup::Sample(17, table->Colors, input)), glm::vec3(1e-3f))));
		}

		// Settings an editor gives are checked against the importer's; a malformed file fails naming itself and the line.
		nlohmann::json normalized;
		CHECK(NormalizeImportSettings(importer, { { "Size", "65" } }, normalized));
		CHECK_FALSE(NormalizeImportSettings(importer, { { "Size", "40" } }, normalized, &error));
		REQUIRE(FileSystem::WriteText(source, "LUT_3D_SIZE 2\n0 0 zero\n"));
		CHECK_FALSE(import(importer.GetDefaultSettings(source), error));
		CHECK(error == "Warm.cube: line 2: expected three numbers, found 0 0 zero");
	}
}
