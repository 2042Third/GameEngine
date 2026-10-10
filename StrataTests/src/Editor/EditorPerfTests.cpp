#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"

#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

// Perf tests of editor commands on generated scenes (CTest StrataTests.Perf, Release and Dist only; see AGENTS.md "Testing").

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr size_t c_Roots = 100'000;

	// An editor without a UI, whose edited scene holds `roots` empty root entities.
	struct EditorWithRoots
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		std::vector<Entity> Roots;

		explicit EditorWithRoots(size_t roots)
		{
			Roots = Perf::CreateFlatRoots(*Context.GetEditScene(), roots);
			Context.GetEditScene()->UpdateWorldTransforms();
		}

		// Runs a command that must succeed (the test ends with its error otherwise).
		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			if (!result.Success)
				FAIL(std::string(name), " failed: ", result.Error);
			return result.Value;
		}
	};

	double MillisecondsSince(std::chrono::steady_clock::time_point start)
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

}

TEST_SUITE("Perf.Editor")
{
	TEST_CASE("Creating and deleting single entities among 100,000 roots")
	{
		EditorWithRoots editor(c_Roots);
		Scene& scene = *editor.Context.GetEditScene();

		constexpr uint32_t c_Warmup = 10;
		constexpr uint32_t c_Iterations = 200;
		const Perf::Measurement create = Perf::Measure("Perf.Editor.EntityCreate100kRoots", c_Warmup, c_Iterations, [&]
		{
			editor.Run("entity.create", { { "name", "Created" } });
		});
		CHECK(scene.GetEntityCount() == c_Roots + c_Warmup + c_Iterations);
		Perf::CheckBudget("Perf.Editor.EntityCreate100kRoots", create.MedianMs);

		// Roots spread over the whole list: each deletion compacts the root list and changes the positions after it.
		constexpr size_t c_Spacing = c_Roots / (c_Warmup + c_Iterations);
		size_t next = 0;
		const Perf::Measurement destroy = Perf::Measure("Perf.Editor.EntityDelete100kRoots", c_Warmup, c_Iterations, [&]
		{
			const UUID target = editor.Roots[next++ * c_Spacing].GetUUID();
			editor.Run("entity.delete", { { "entities", nlohmann::json::array({ UUIDToJson(target) }) } });
		});
		CHECK(scene.GetEntityCount() == c_Roots);
		Perf::CheckBudget("Perf.Editor.EntityDelete100kRoots", destroy.MedianMs);

		std::string error;
		CHECK_MESSAGE(scene.ValidateHierarchy(&error), error);
	}

	TEST_CASE("Deleting 10,000 of 100,000 roots is one undo step, and undoing it is fast")
	{
		EditorWithRoots editor(c_Roots);
		Scene& scene = *editor.Context.GetEditScene();
		// Undo recreates the deleted entities with their UUIDs (but new handles).
		const std::vector<UUID> rootIds = scene.GetRootEntities();
		REQUIRE(rootIds.size() == c_Roots);
		nlohmann::json doomed = nlohmann::json::array();
		for (size_t index = 0; index < c_Roots; index += 10)
			doomed.push_back(UUIDToJson(rootIds[index]));
		const nlohmann::json parameters = { { "entities", doomed } };

		constexpr int c_Rounds = 3;
		std::vector<double> deleteSamples;
		std::vector<double> undoSamples;
		for (int round = 0; round < c_Rounds; round++)
		{
			const size_t position = editor.Context.GetUndoStack().GetPosition();
			auto start = std::chrono::steady_clock::now();
			editor.Run("entity.delete", parameters);
			deleteSamples.push_back(MillisecondsSince(start));
			CHECK(scene.GetEntityCount() == c_Roots - doomed.size());
			CHECK(editor.Context.GetUndoStack().GetPosition() == position + 1);

			start = std::chrono::steady_clock::now();
			editor.Run("edit.undo");
			undoSamples.push_back(MillisecondsSince(start));
			CHECK(scene.GetEntityCount() == c_Roots);
			CHECK(editor.Context.GetUndoStack().GetPosition() == position);
		}

		Perf::CheckBudget("Perf.Editor.Delete10kOf100kRoots", Perf::Summarize(deleteSamples).MedianMs);
		Perf::CheckBudget("Perf.Editor.UndoDelete10kOf100kRoots", Perf::Summarize(undoSamples).MedianMs);

		// Undo restored every root at its position.
		CHECK(scene.GetRootEntities() == rootIds);
		std::string error;
		CHECK_MESSAGE(scene.ValidateHierarchy(&error), error);
	}
}
