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
// Statistics: the median time of a command over its calls (200 single-entity commands; three rounds of the 10,000-entity
// delete and its undo, each of which takes tens of milliseconds), confirmed by measuring again when over its budget.

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
		size_t created = 0;
		Perf::CheckBudgetConfirmed("Perf.Editor.EntityCreate100kRoots", [&]
		{
			const Perf::Measurement create = Perf::Measure("Perf.Editor.EntityCreate100kRoots", c_Warmup, c_Iterations, [&]
			{
				editor.Run("entity.create", { { "name", "Created" } });
			});
			created += c_Warmup + c_Iterations;
			return create.MedianMs;
		});
		CHECK(scene.GetEntityCount() == c_Roots + created);

		// Roots spread over the whole list: each deletion compacts the root list and changes the positions after it. Every
		// attempt deletes roots spread like that, next to those of the attempt before.
		constexpr size_t c_Spacing = c_Roots / (c_Warmup + c_Iterations);
		static_assert(c_Spacing >= Perf::c_ConfirmationAttempts, "every attempt needs roots of its own");
		size_t deleted = 0;
		uint32_t attempt = 0;
		Perf::CheckBudgetConfirmed("Perf.Editor.EntityDelete100kRoots", [&]
		{
			size_t next = 0;
			const Perf::Measurement destroy = Perf::Measure("Perf.Editor.EntityDelete100kRoots", c_Warmup, c_Iterations, [&]
			{
				const UUID target = editor.Roots[next++ * c_Spacing + attempt].GetUUID();
				editor.Run("entity.delete", { { "entities", nlohmann::json::array({ UUIDToJson(target) }) } });
			});
			attempt++;
			deleted += next;
			return destroy.MedianMs;
		});
		CHECK(scene.GetEntityCount() == c_Roots + created - deleted);

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

		// Each round deletes and undoes once, so the median of three rounds ignores one that the machine's other work slowed.
		Perf::CheckBudgetsConfirmed({ "Perf.Editor.Delete10kOf100kRoots", "Perf.Editor.UndoDelete10kOf100kRoots" }, [&]
		{
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
			return Perf::MetricValues {
				{ "Perf.Editor.Delete10kOf100kRoots", Perf::Summarize(deleteSamples).MedianMs },
				{ "Perf.Editor.UndoDelete10kOf100kRoots", Perf::Summarize(undoSamples).MedianMs } };
		});

		// Undo restored every root at its position.
		CHECK(scene.GetRootEntities() == rootIds);
		std::string error;
		CHECK_MESSAGE(scene.ValidateHierarchy(&error), error);
	}
}
