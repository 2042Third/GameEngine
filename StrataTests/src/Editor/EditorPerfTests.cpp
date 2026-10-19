#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/HierarchyView.h"
#include "Editor/PrefabSync.h"
#include "Editor/SceneRecovery.h"
#include "Editor/ThumbnailCache.h"
#include "EditorLayer.h"
#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Perf/StressProject.h"
#include "Renderer/GPUTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/GraphicsDevice.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Renderer/Renderer.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Renderer/Texture.h>
#include <Strata/Renderer/TextureStreamer.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Perf tests of the editor on generated scenes and projects (CTest StrataTests.Perf and, with a GPU, StrataTests.PerfGPU:
// Release and Dist only; see Docs/DeveloperGuide.md, "Perf tests"). Statistics, each confirmed by measuring again when
// over its budget: the median time of a command over its calls (200 single-entity commands; three rounds of the
// 10,000-entity delete and its undo, each of which takes tens of milliseconds), the median of batched or repeated calls of
// the hierarchy's rows, the difference of the median editor frames with and without a panel or a selection, and the
// longest frame of a re-import.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr size_t c_Roots = 100'000;

	// A stress project with one 4096x4096 texture, the material that uses it and an entity drawing it.
	StressProjectSpec GetLargeTextureSpec()
	{
		StressProjectSpec spec;
		spec.Textures2k = 0;
		spec.Textures4k = 1;
		spec.Materials = 1;
		spec.Entities = 1;
		spec.GridResolution = 2;
		return spec;
	}

	std::optional<StressProject> WriteLargeTextureProject(const std::string& name, std::string& error)
	{
		return WriteStressProject(CreateTemporaryDirectory(name) / "Project", GetLargeTextureSpec(), error);
	}

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

	TEST_CASE("The hierarchy's rows of 100,000 roots: idle frames cost nothing, a change rebuilds them")
	{
		EditorWithRoots editor(c_Roots);
		const Ref<Scene>& scene = editor.Context.GetEditScene();
		HierarchyView view;
		REQUIRE(view.Update(scene));
		REQUIRE(view.GetRows().size() == c_Roots);

		// Unchanged frames: a few comparisons, whatever the scene's size (timed in batches: each call takes nanoseconds; the
		// median batch).
		constexpr int c_Batch = 1000;
		Perf::CheckBudgetConfirmed("Perf.Editor.HierarchyIdleUpdate100kRoots", [&]
		{
			const Perf::Measurement idle = Perf::Measure("Perf.Editor.HierarchyIdleUpdate100kRoots", 10, 100, [&]
			{
				for (int call = 0; call < c_Batch; call++)
					view.Update(scene);
			});
			return idle.MedianMs * 1000.0 / c_Batch;
		});
		CHECK(view.GetRebuildCount() == 1);

		// An entity created among them: the rows are rebuilt once, in the next frame (the median rebuild).
		constexpr size_t c_RebuildCalls = 55;
		size_t created = 0;
		Perf::CheckBudgetConfirmed("Perf.Editor.HierarchyRebuild100kRoots", [&]
		{
			const Perf::Measurement rebuild = Perf::Measure("Perf.Editor.HierarchyRebuild100kRoots", 5, c_RebuildCalls - 5, [&]
			{
				scene->CreateEntity("Created");
				REQUIRE(view.Update(scene));
			});
			created += c_RebuildCalls;
			return rebuild.MedianMs;
		});
		CHECK(view.GetRows().size() == c_Roots + created);
	}

	TEST_CASE("The Hierarchy panel's share of an editor frame with 100,000 roots")
	{
		// The real editor UI on the harness (no GPU: the viewport draws nothing, so the frames are the UI's own work), with
		// the launcher dismissed and 100,000 top-level entities in the edited scene.
		HarnessEditor editor;
		editor.Layer->DismissLauncher();
		Perf::CreateFlatRoots(*editor.Context().GetEditScene(), c_Roots);
		editor.Context().GetEditScene()->UpdateWorldTransforms();
		editor.Frames(3);
		EditorPanelRegistry& panels = editor.Layer->GetPanels();
		REQUIRE(panels.IsOpen(EditorPanels::c_Hierarchy));
		ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Hierarchy);
		REQUIRE(window);
		REQUIRE(window->Active);

		// The same frames with the panel open and closed, both measured afresh by every attempt: the difference of the medians
		// is what the panel costs. Its budget is far below the authoring workstream's acceptance limit of 0.3 ms (the panel
		// that drew every root cost 12.9 ms here).
		constexpr uint32_t c_Warmup = 10;
		constexpr uint32_t c_Frames = 200;
		Perf::CheckBudgetConfirmed("Perf.Editor.HierarchyPanelFrame100kRoots", [&]
		{
			panels.SetOpen(EditorPanels::c_Hierarchy, true);
			editor.Frames(2);
			REQUIRE(window->Active);
			const Perf::Measurement open = Perf::Measure("Perf.Editor.EditorFrame100kRootsHierarchyOpen", c_Warmup, c_Frames, [&] { editor.Frames(1); });
			panels.SetOpen(EditorPanels::c_Hierarchy, false);
			editor.Frames(2);
			REQUIRE_FALSE(window->Active);
			const Perf::Measurement closed = Perf::Measure("Perf.Editor.EditorFrame100kRootsHierarchyClosed", c_Warmup, c_Frames, [&] { editor.Frames(1); });
			const double cost = std::max(open.MedianMs - closed.MedianMs, 0.0);
			MESSAGE("Hierarchy panel: " << cost << " ms of a " << open.MedianMs << " ms frame");
			return cost * 1000.0;
		});
	}

	TEST_CASE("What 100,000 selected entities add to an editor frame")
	{
		HarnessEditor editor;
		editor.Layer->DismissLauncher();
		const std::vector<Entity> roots = Perf::CreateFlatRoots(*editor.Context().GetEditScene(), c_Roots);
		editor.Context().GetEditScene()->UpdateWorldTransforms();
		std::vector<UUID> all;
		for (const Entity root : roots)
			all.push_back(root.GetUUID());
		// The Inspector compares every selected entity's values each frame (mixed values), the Hierarchy marks the rows in view:
		// the difference of the median frames with and without the selection, both measured afresh by every attempt.
		Perf::CheckBudgetConfirmed("Perf.Editor.SelectionFrame100kSelected", [&]
		{
			editor.Context().ClearSelection();
			editor.Frames(3);
			const Perf::Measurement none = Perf::Measure("Perf.Editor.EditorFrameNothingSelected", 5, 50, [&] { editor.Frames(1); });
			editor.Context().SetSelection(all);
			editor.Frames(3);
			const Perf::Measurement selected = Perf::Measure("Perf.Editor.EditorFrame100kSelected", 5, 50, [&] { editor.Frames(1); });
			const double cost = std::max(selected.MedianMs - none.MedianMs, 0.0);
			MESSAGE("100,000 entities selected: " << cost << " ms per frame");
			return cost;
		});
	}

	TEST_CASE("Changing the import settings of a 4096x4096 texture never holds a frame")
	{
		// The editor's job system: the import and the reload run on its I/O and worker threads.
		Perf::ScopedApplicationJobSystem jobs;
		std::string error;
		const std::optional<StressProject> project = WriteLargeTextureProject("PerfImportSettings", error);
		REQUIRE_MESSAGE(project, error);
		const AssetHandle texture = project->Textures.front();

		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		REQUIRE_MESSAGE(context.OpenProject(project->Directory, &error), error);
		REQUIRE(AssetManager::LoadAssetSync<Texture>(texture));

		// One re-import with the given settings, in frames as the editor runs them (the context's update, then the commands',
		// while the viewport uses the texture); returns the longest frame in milliseconds.
		const auto reimport = [&](const nlohmann::json& settings)
		{
			auto completed = std::make_shared<std::optional<EditorCommandResult>>();
			auto start = std::chrono::steady_clock::now();
			runner.Run(context, commands, "asset.setImportSettings", { { "asset", UUIDToJson(texture) }, { "settings", settings } },
				[completed](const EditorCommandResult& result) { *completed = result; });
			double maxFrame = MillisecondsSince(start);
			uint32_t frames = 0;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
			while ((!*completed || AssetManager::GetAssetState(texture) != AssetState::Ready) && std::chrono::steady_clock::now() < deadline)
			{
				start = std::chrono::steady_clock::now();
				AssetManager::GetAsset<Texture>(texture);
				context.Update(Timestep(1.0f / 60.0f));
				runner.Update(context);
				maxFrame = std::max(maxFrame, MillisecondsSince(start));
				frames++;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			REQUIRE(*completed);
			INFO((*completed)->Error);
			REQUIRE((*completed)->Success);
			REQUIRE(AssetManager::GetAssetState(texture) == AssetState::Ready);
			CHECK(frames > 1); // The import outlasted a frame: it really ran beside them
			MESSAGE(settings.dump(), ": ", frames, " frames, the longest ", maxFrame, " ms");
			return maxFrame;
		};

		// The longest frame (a maximum by nature: one held frame is the hitch the user sees) while the texture re-imports
		// uncompressed (decoding, filtering its mips) and then block-compressed again (BC7 on every worker): the authoring
		// workstream's criterion, whose acceptance limit of 33 ms (two frames at 60 Hz) is far above the budget.
		Perf::CheckBudgetConfirmed("Perf.Editor.ImportSettings4kMaxFrame", [&]
		{
			const double uncompressed = reimport({ { "Compression", "Uncompressed" } });
			CHECK(AssetManager::GetAsset<Texture>(texture)->GetSpecification().Format == TextureFormat::RGBA8SRGB);
			const double compressed = reimport({ { "Compression", "Auto" } });
			CHECK(AssetManager::GetAsset<Texture>(texture)->GetSpecification().Format == GetStressTextureFormat());
			return std::max(uncompressed, compressed);
		});
		CHECK(AssetManager::GetAsset<Texture>(texture)->GetWidth() == 4096u);
	}

	TEST_CASE("Finding what uses an asset among 10,000 assets")
	{
		// 10,000 materials, every hundredth using one texture, and the open scene using it too: asset.findReferences answers
		// from the asset manager's index of who references whom (and the open scene from memory), never by reading documents.
		constexpr uint32_t c_Materials = 10'000;
		constexpr uint32_t c_Users = 100;
		Perf::ScopedApplicationJobSystem jobs;
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		const std::filesystem::path project = CreateTemporaryDirectory("PerfFindReferences") / "Game";
		const EditorCommandResult created = commands.Execute(context, "project.create", { { "directory", FileSystem::ToUTF8(project) }, { "name", "Game" } });
		REQUIRE_MESSAGE(created.Success, created.Error);
		REQUIRE(FileSystem::WriteBytes(project / "Assets" / "Textures" / "Shared.png", CreateSolidPNG(4, 4, 200, 100, 50)));
		std::string error;
		REQUIRE_MESSAGE(context.OpenProject(project, &error), error);
		const AssetHandle texture = context.GetAssetManager()->FindAssetByPath("Textures/Shared.png");
		REQUIRE(texture.IsValid());
		const std::string plain = JsonUtils::Dump(Material::Create()->Serialize(), 1, '\t');
		MaterialProperties properties;
		properties.BaseColorMap = texture;
		const std::string textured = JsonUtils::Dump(Material::Create(properties)->Serialize(), 1, '\t');
		for (uint32_t index = 0; index < c_Materials; index++)
			REQUIRE(FileSystem::WriteText(project / "Assets" / "Materials" / fmt::format("Perf{}.stmat", index), index % (c_Materials / c_Users) == 0 ? textured : plain));
		REQUIRE_MESSAGE(context.OpenProject(project, &error), error);
		const nlohmann::json parameters = { { "asset", UUIDToJson(texture) } };
		const EditorCommandResult first = commands.Execute(context, "asset.findReferences", parameters);
		REQUIRE_MESSAGE(first.Success, first.Error);
		CHECK(first.Value["referencedBy"].size() == c_Users);

		// The median call: one lookup of the index and a walk of the open scene's entities.
		Perf::CheckBudgetConfirmed("Perf.Editor.FindReferences10k", [&]
		{
			return Perf::Measure("Perf.Editor.FindReferences10k", 5, 50, [&]
			{
				const EditorCommandResult result = commands.Execute(context, "asset.findReferences", parameters);
				REQUIRE(result.Success);
			}).MedianMs;
		});
	}

	TEST_CASE("Recording a command in the activity journal costs a few microseconds")
	{
		EditorWithRoots editor(c_Roots);
		CommandJournal& journal = editor.Context.GetJournal();
		const nlohmann::json parameters = { { "entity", UUIDToJson(editor.Roots[c_Roots / 2].GetUUID()) } };
		// 100,000 entity.get calls in batches of 10,000, alternately with the journal recording and without: the metric is the
		// median batch's time per call with it minus the median without it, in microseconds.
		constexpr int c_Batches = 10;
		constexpr int c_CallsPerBatch = 10'000;
		auto timeBatch = [&](bool recording)
		{
			journal.SetEnabled(recording);
			const auto start = std::chrono::steady_clock::now();
			for (int call = 0; call < c_CallsPerBatch; call++)
				editor.Commands.Execute(editor.Context, "entity.get", parameters);
			return MillisecondsSince(start) * 1000.0 / c_CallsPerBatch;
		};
		Perf::CheckBudgetConfirmed("Perf.Editor.JournalPerCommand", [&]
		{
			std::vector<double> on;
			std::vector<double> off;
			timeBatch(true); // Warm-up: the ring fills to its limits once
			for (int batch = 0; batch < c_Batches / 2; batch++)
			{
				on.push_back(timeBatch(true));
				off.push_back(timeBatch(false));
			}
			journal.SetEnabled(true);
			const double withJournal = Perf::Summarize(on).MedianMs;
			const double without = Perf::Summarize(off).MedianMs;
			MESSAGE("entity.get: ", withJournal, " us per call with the journal, ", without, " us without");
			return std::max(withJournal - without, 0.0);
		});
		CHECK(journal.GetEntries().size() == CommandJournal::c_MaxEntries);
	}

	TEST_CASE("The recovery journal of a 100,000-entity scene is written without holding up frames")
	{
		// The I/O threads the journal's writes run on, as in the editor.
		Perf::ScopedApplicationJobSystem jobs;
		const std::filesystem::path directory = CreateTemporaryDirectory("PerfRecovery") / "Game";
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		REQUIRE(commands.Execute(context, "project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } }).Success);
		const std::vector<Entity> roots = Perf::CreateFlatRoots(*context.GetEditScene(), c_Roots);
		REQUIRE(commands.Execute(context, "scene.saveAs", { { "path", "Scenes/Big.stscene" } }).Success);
		SceneRecovery& recovery = context.GetRecovery();
		recovery.Wait(); // The saved document's hash
		Scene& scene = *context.GetEditScene();
		auto timeFrame = [&context]()
		{
			const auto start = std::chrono::steady_clock::now();
			context.Update(Timestep(1.0f / 60.0f));
			return MillisecondsSince(start);
		};

		// Edits of 10,000 of the roots (one undo step each, component.set of their positions), each followed by the frames while its journal is written: the
		// first asks for the write as Update does once edits stop, the others go on until the file is written. Each write's
		// longest frame above the median idle frame is the hitch a person would see, and the time the I/O threads spent on it
		// (JobSystem::GetStats) is the time asset loads and mip reads, which share them, could wait behind it; the metrics
		// are their medians over the writes of an attempt (the journal grows with every write), so one write the machine's
		// other work held up does not decide them.
		constexpr int c_Writes = 7;
		size_t round = 0;
		struct JournalWrite
		{
			double HitchMs = 0.0;
			double IOMs = 0.0;
		};
		auto writeJournal = [&](const nlohmann::json& edited, double idleMs)
		{
			// A position no earlier round set: every round changes every entity it lists.
			round++;
			const nlohmann::json values = { { "Translation", { static_cast<double>(round), 0.0, 0.0 } } };
			REQUIRE(commands.Execute(context, "component.set", { { "entities", edited }, { "component", "Transform" }, { "values", values } }).Success);
			// The frame after the edit updates what it changed (world transforms): the edit's cost, not the journal's.
			context.Update(Timestep(1.0f / 60.0f));
			const std::chrono::nanoseconds ioBefore = JobSystem::GetStats().IO.BusyTime;
			const auto writeStart = std::chrono::steady_clock::now();
			double longest = 0.0;
			{
				const auto start = std::chrono::steady_clock::now();
				context.Update(Timestep(1.0f / 60.0f));
				REQUIRE(recovery.Flush(scene, context.IsSceneModified()));
				longest = MillisecondsSince(start);
			}
			uint32_t frames = 1;
			while (recovery.IsBusy())
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				longest = std::max(longest, timeFrame());
				frames++;
			}
			const double ioMs = std::chrono::duration<double, std::milli>(JobSystem::GetStats().IO.BusyTime - ioBefore).count();
			MESSAGE("Journal of ", edited.size(), " entities written in ", MillisecondsSince(writeStart), " ms over ", frames, " frames; the longest ", longest,
				" ms, the idle median ", idleMs, " ms; the I/O threads busy ", ioMs, " ms");
			return JournalWrite { std::max(longest - idleMs, 0.0), ioMs };
		};
		Perf::CheckBudgetsConfirmed({ "Perf.Editor.RecoveryWriteMaxFrame100k", "Perf.Editor.RecoveryWriteIOTime100k" }, [&]
		{
			std::vector<double> idle;
			for (int frame = 0; frame < 60; frame++)
				idle.push_back(timeFrame());
			const double idleMs = Perf::Summarize(idle).MedianMs;
			std::vector<double> hitches;
			std::vector<double> io;
			for (int write = 0; write < c_Writes; write++)
			{
				nlohmann::json edited = nlohmann::json::array();
				for (size_t index = round % 10; index < roots.size(); index += 10)
					edited.push_back(UUIDToJson(roots[index].GetUUID()));
				const JournalWrite written = writeJournal(edited, idleMs);
				hitches.push_back(written.HitchMs);
				io.push_back(written.IOMs);
			}
			return Perf::MetricValues { { "Perf.Editor.RecoveryWriteMaxFrame100k", Perf::Summarize(hitches).MedianMs },
				{ "Perf.Editor.RecoveryWriteIOTime100k", Perf::Summarize(io).MedianMs } };
		});
		CHECK(recovery.GetStatus().LastWrite > 0);
		// For the record (Docs/Architecture.md, "Autosave and recovery"): the time for a journal of every entity.
		nlohmann::json all = nlohmann::json::array();
		for (const Entity root : roots)
			all.push_back(UUIDToJson(root.GetUUID()));
		writeJournal(all, 0.0);

		// Snapshots (what a scene that was never saved starts with, a step only a snapshot captures) are taken on the main
		// thread, of at most SceneRecovery::c_MaxSnapshotEntities entities: the median time of capturing that many bounds the
		// frame it costs.
		const std::vector<Entity> snapshot(roots.begin(), roots.begin() + SceneRecovery::c_MaxSnapshotEntities);
		std::vector<UUID> ids;
		for (const Entity entity : snapshot)
			ids.push_back(entity.GetUUID());
		Perf::CheckBudgetConfirmed("Perf.Editor.RecoverySnapshotMaxEntities", [&]
		{
			return Perf::Measure("Perf.Editor.RecoverySnapshotMaxEntities", 3, 15, [&]
			{
				const std::vector<EntityState> states = SceneEdit::CaptureEntities(scene, ids);
				CHECK(states.size() == ids.size());
			}).MedianMs;
		});
	}

	TEST_CASE("Opening a scene of 10,000 prefab instances holds no frame, and applying one reaches the others in one step")
	{
		// The loading worker and the I/O threads, as in the editor.
		Perf::ScopedApplicationJobSystem jobs;
		const std::filesystem::path directory = CreateTemporaryDirectory("PerfPrefabInstancesOpen") / "Game";
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		auto run = [&](std::string_view name, const nlohmann::json& parameters) { return RunCommandToCompletion(context, commands, name, parameters); };
		run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });

		// A prefab of a root and four children, instantiated once by prefab.instantiate (which records the version of the
		// prefab the instance took and the hash of its values, as every instance the editor makes does), then copied 9,999
		// times in the scene's document: a forest of up-to-date instances.
		constexpr size_t c_Instances = 10'000;
		const std::string tree = run("entity.create", { { "name", "Tree" } })["id"].get<std::string>();
		for (int branch = 0; branch < 4; branch++)
			run("entity.create", { { "name", fmt::format("Branch {}", branch) }, { "parent", tree } });
		run("prefab.create", { { "entities", { tree } }, { "path", "Prefabs/Tree.stprefab" } });
		run("scene.new", { { "force", true } });
		const std::string instance = run("prefab.instantiate", { { "prefab", "Prefabs/Tree.stprefab" } })["id"].get<std::string>();
		nlohmann::json document = SceneSerializer::Serialize(*context.GetEditScene());
		nlohmann::json& entities = document["Scene"]["Entities"];
		std::vector<nlohmann::json> parts;
		for (const nlohmann::json& entity : entities)
		{
			if (entity["ID"] == instance || entity.value("Parent", "") == instance)
				parts.push_back(entity);
		}
		REQUIRE(parts.size() == 5);
		REQUIRE(parts[0]["Components"]["PrefabInstance"]["PrefabVersion"] != "0000000000000000");
		for (size_t copy = 1; copy < c_Instances; copy++)
		{
			const std::string root = UUID().ToString();
			for (const nlohmann::json& part : parts)
			{
				nlohmann::json entity = part;
				if (part["ID"] == instance)
				{
					entity["ID"] = root;
					entity["Components"]["Transform"]["Translation"] = { static_cast<double>(copy % 100) * 3.0, 0.0, static_cast<double>(copy / 100) * 3.0 };
				}
				else
				{
					entity["ID"] = UUID().ToString();
					entity["Parent"] = root;
				}
				entities.push_back(std::move(entity));
			}
		}
		const std::string text = JsonUtils::Dump(document, 1, '\t');
		std::string error;
		REQUIRE_MESSAGE(context.GetAssetManager()->CreateNativeAsset("Scenes/Forest.stscene", std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()),
			&error).IsValid(), error);

		// The longest frame (an editor update and the command's poll) while scene.open opens it: the frame that swaps the scene
		// in checks every instance against its prefab (PrefabSync's catch-up), a maximum by nature.
		Perf::CheckBudgetConfirmed("Perf.Editor.SceneOpen10kPrefabInstancesMaxFrame", [&]
		{
			run("scene.new", { { "force", true } });
			const uint64_t checks = context.GetPrefabSync().GetCatchUpCheckCount();
			EditorCommandResult result = commands.Execute(context, "scene.open", { { "scene", "Scenes/Forest.stscene" } });
			double longest = 0.0;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
			while (result.IsPending() && std::chrono::steady_clock::now() < deadline)
			{
				const auto start = std::chrono::steady_clock::now();
				context.Update(Timestep(1.0f / 60.0f));
				const bool finished = PollCommandResult(result, context);
				longest = std::max(longest, MillisecondsSince(start));
				// The loading worker prepares the scene between frames, as between an editor's.
				if (!finished)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			REQUIRE_MESSAGE(result.Success, result.Error);
			CHECK(context.GetEditScene()->GetEntityCount() == c_Instances * 5);
			// Every instance was looked at, and none had missed a change of its prefab.
			CHECK(context.GetPrefabSync().GetCatchUpCheckCount() == checks + c_Instances);
			CHECK_FALSE(context.IsSceneModified());
			return longest;
		});

		// prefab.apply of one instance's change: the file, and every other instance takes it (PrefabOverrides::Propagate), in
		// one undo step that tracks only what changed. The median of five applies, each of a new value.
		const std::string branch = context.GetEditScene()->GetEntityByUUID(*UUIDFromJson(nlohmann::json(instance))).GetChildren().front().GetUUID().ToString();
		int round = 0;
		Perf::CheckBudgetConfirmed("Perf.Editor.PrefabApply10kInstances", [&]
		{
			std::vector<double> samples;
			for (int apply = 0; apply < 5; apply++)
			{
				round++;
				run("component.set", { { "entity", branch }, { "component", "Transform" }, { "values", { { "Translation", { round, 0, 0 } } } } });
				const auto start = std::chrono::steady_clock::now();
				const nlohmann::json applied = run("prefab.apply", { { "entity", instance } });
				samples.push_back(MillisecondsSince(start));
				CHECK(applied["updatedInstances"] == c_Instances - 1);
			}
			return Perf::Summarize(samples).MedianMs;
		});
	}
}

TEST_SUITE("PerfGPU.Editor")
{
	TEST_CASE("Changing the import settings of a 4096x4096 texture never holds a frame that uploads and draws it")
	{
		// The test above without a GPU leaves out the part of a re-import that runs on the main thread: the reloaded texture's
		// upload (Texture::FinalizeOnMainThread, in steps within the frame's upload budget). Here the editor's frames run on
		// the shared device, as the editor runs them (device frame, renderer frame, the context's update with the assets'
		// finalization, the commands', then the scene drawn with the texture), and each frame's work between the device's
		// begin and end is timed (what the editor's frame time shows: without the waits for the GPU).
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		GraphicsDevice& device = gpu.GetDevice();
		Perf::ScopedApplicationJobSystem jobs;
		std::string error;
		const std::optional<StressProject> project = WriteLargeTextureProject("PerfGPUImportSettings", error);
		REQUIRE_MESSAGE(project, error);
		const AssetHandle texture = project->Textures.front();

		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		REQUIRE_MESSAGE(context.OpenProject(project->Directory, &error), error);
		Ref<Scene> scene;
		{
			Ref<SceneAsset> sceneAsset = AssetManager::LoadAssetSync<SceneAsset>(project->WorldScene);
			REQUIRE(sceneAsset);
			scene = sceneAsset->CreateScene(&error);
			REQUIRE_MESSAGE(scene, error);
		}
		constexpr uint32_t c_Width = 1280;
		constexpr uint32_t c_Height = 720;
		const SceneCamera camera = GetStressSweepFrame(GetLargeTextureSpec(), StressSweepPath(), 0, static_cast<float>(c_Width) / static_cast<float>(c_Height)).Camera;
		// Drawn whole (a view without streaming waits for every level), so that every re-import uploads the whole new texture
		// in steps, the work this test is about: with streaming, this view needs no more than the texture's tail.
		SceneRendererSpecification specification;
		specification.TextureStreaming = false;
		SceneRenderer renderer(specification);
		renderer.SetViewportSize(c_Width, c_Height);
		// A second view of 10,000 cubes, drawn in the frames of the last measurement: its draws are extracted in parallel
		// chunks (more than one chunk's proxies in view), so the frame waits for work of its own on the workers while the
		// import compresses. Such a frame must neither run the compression itself nor wait behind it.
		constexpr size_t c_Cubes = 10'000;
		Scene cubes("Cubes");
		Perf::AddCameraAndLight(cubes);
		Perf::CreateCubeGrid(cubes, c_Cubes);
		const float gridHalf = static_cast<float>(std::ceil(std::sqrt(static_cast<double>(c_Cubes)))) * Perf::c_GridSpacing * 0.5f;
		const float aspect = static_cast<float>(c_Width) / static_cast<float>(c_Height);
		SceneCamera cubesCamera = LookAt(glm::vec3(0.0f, gridHalf * 1.6f, gridHalf * 1.3f), glm::vec3(0.0f), aspect);
		cubesCamera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), aspect, 0.1f, gridHalf * 10.0f);
		cubesCamera.Far = gridHalf * 10.0f;
		SceneRenderer cubesRenderer;
		cubesRenderer.SetViewportSize(c_Width, c_Height);
		bool drawCubes = false;
		// One frame; returns its work in milliseconds.
		const auto frame = [&]()
		{
			REQUIRE(device.BeginFrame());
			const auto start = std::chrono::steady_clock::now();
			Renderer::BeginFrame();
			AssetManager::GetAsset<Texture>(texture);
			context.Update(Timestep(1.0f / 60.0f));
			runner.Update(context);
			CHECK(renderer.Render(*scene, camera));
			if (drawCubes)
				CHECK(cubesRenderer.Render(cubes, cubesCamera));
			const double milliseconds = MillisecondsSince(start);
			device.EndFrame();
			return milliseconds;
		};
		// The texture is uploaded and drawn once it is whole (the view's renderer counts it pending until then) and its
		// streamer, which raised it, has nothing left to do.
		EditorAssetManager* manager = context.GetAssetManager();
		REQUIRE(manager);
		const TextureStreamer* streamer = TextureStreamer::Find(*manager);
		REQUIRE(streamer);
		const auto drawn = [&]()
		{
			const AssetManagerStats stats = manager->GetStats();
			return AssetManager::GetAssetState(texture) == AssetState::Ready && renderer.GetStats().PendingAssets == 0 && stats.Detail.Reads == 0
				&& stats.Detail.Changes == 0 && stats.Detail.AssetsBelowWanted == 0 && streamer->GetTexturesLookedAt() == 0;
		};
		// The texture uploaded and drawn before the measured re-imports.
		const auto loaded = std::chrono::steady_clock::now() + std::chrono::minutes(2);
		while (!drawn() && std::chrono::steady_clock::now() < loaded)
			frame();
		REQUIRE(drawn());

		// One re-import with the given settings, until the new texture is uploaded and drawn at the levels the view wants;
		// returns the longest frame, or with importOnly the longest frame while the import runs (until the command answers,
		// before the new texture loads).
		const auto reimport = [&](const nlohmann::json& settings, bool importOnly = false)
		{
			auto completed = std::make_shared<std::optional<EditorCommandResult>>();
			const auto start = std::chrono::steady_clock::now();
			runner.Run(context, commands, "asset.setImportSettings", { { "asset", UUIDToJson(texture) }, { "settings", settings } },
				[completed](const EditorCommandResult& result) { *completed = result; });
			double maxFrame = MillisecondsSince(start);
			uint32_t frames = 0;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
			while ((!*completed || !drawn()) && std::chrono::steady_clock::now() < deadline)
			{
				const bool importing = !*completed;
				const double milliseconds = frame();
				if (importing || !importOnly)
					maxFrame = std::max(maxFrame, milliseconds);
				frames++;
			}
			REQUIRE(*completed);
			INFO((*completed)->Error);
			REQUIRE((*completed)->Success);
			REQUIRE(drawn());
			CHECK(frames > 1);
			AssetDetailLevels levels;
			REQUIRE(AssetManager::GetAsset<Texture>(texture)->GetDetailLevels(levels));
			MESSAGE(settings.dump(), ": ", frames, " frames, the longest ", maxFrame, " ms; levels from ", levels.Resident, " of ", levels.Count,
				" resident (wanted ", levels.Wanted, ")");
			return maxFrame;
		};

		// The longest frame of a re-import uncompressed (an 89 MB upload in steps) and of one back to BC7 (every worker
		// compressing for seconds), as in the test above; the acceptance limit of 33 ms is far above the budget.
		Perf::CheckBudgetConfirmed("PerfGPU.Editor.ImportSettings4kMaxFrame", [&]
		{
			const double uncompressed = reimport({ { "Compression", "Uncompressed" } });
			const double compressed = reimport({ { "Compression", "Auto" } });
			return std::max(uncompressed, compressed);
		});
		// With the 10,000 cubes drawn too, the longest frame while the texture is compressed to BC7 again on the background
		// threads: frames whose parallel work must neither run the compression nor wait behind it (the frames that upload
		// the new texture afterwards are the metric above's).
		drawCubes = true;
		frame();
		CHECK(cubesRenderer.GetStats().Instances == c_Cubes);
		Perf::CheckBudgetConfirmed("PerfGPU.Editor.ImportSettings4kMaxFrame10kCubes", [&]
		{
			reimport({ { "Compression", "Uncompressed" } });
			return reimport({ { "Compression", "Auto" } }, true);
		});
		device.WaitForIdle();
		CHECK(AssetManager::GetAsset<Texture>(texture)->GetWidth() == 4096u);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Importing a 4096x4096 texture never holds a frame")
	{
		// asset.import as the editor runs it: the copy on an I/O job, the import (decoding, mips, BC7 on the background
		// threads) beside the frames, then the new texture's upload in steps once a frame requests it. Frames as in the test
		// above, on the shared device.
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		GraphicsDevice& device = gpu.GetDevice();
		Perf::ScopedApplicationJobSystem jobs;
		std::string error;
		const std::optional<StressProject> project = WriteLargeTextureProject("PerfGPUImportTexture", error);
		REQUIRE_MESSAGE(project, error);
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		REQUIRE_MESSAGE(context.OpenProject(project->Directory, &error), error);
		// The stress texture's source, as a file from outside the project.
		const std::filesystem::path source = context.GetAssetManager()->GetAbsolutePath(project->Textures.front());
		const std::filesystem::path external = project->Directory.parent_path() / "Downloads" / "Large.png";
		REQUIRE(FileSystem::CreateDirectories(external.parent_path()));
		REQUIRE(FileSystem::Copy(source, external));

		uint32_t imports = 0;
		const auto frame = [&](AssetHandle requested)
		{
			REQUIRE(device.BeginFrame());
			const auto start = std::chrono::steady_clock::now();
			Renderer::BeginFrame();
			if (requested.IsValid())
				AssetManager::GetAsset<Texture>(requested);
			context.Update(Timestep(1.0f / 60.0f));
			runner.Update(context);
			const double milliseconds = MillisecondsSince(start);
			device.EndFrame();
			// Frames at the editor's pace rather than as fast as the loop spins (millions of empty frames would measure the
			// machine's rarest hiccup rather than the import).
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return milliseconds;
		};
		// The longest frame (a maximum by nature: one held frame is the hitch the user sees) from the command until the
		// imported texture is loaded, which uploads its mip tail: nothing draws it here, and its finer levels stream once
		// something does (the import's work ends with the first load; GPU.TextureStreaming and PerfGPU.Streaming cover the
		// streaming). The acceptance limit is 33 ms (two frames at 60 Hz).
		Perf::CheckBudgetConfirmed("PerfGPU.Editor.ImportTexture4kMaxFrame", [&]
		{
			auto completed = std::make_shared<std::optional<EditorCommandResult>>();
			const auto start = std::chrono::steady_clock::now();
			runner.Run(context, commands, "asset.import", { { "file", FileSystem::ToUTF8(external) }, { "directory", fmt::format("Imported{}", imports++) } },
				[completed](const EditorCommandResult& result) { *completed = result; });
			double maxFrame = MillisecondsSince(start);
			uint32_t frames = 0;
			AssetHandle imported = UUID::Null();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
			while ((!*completed || AssetManager::GetAssetState(imported) != AssetState::Ready) && std::chrono::steady_clock::now() < deadline)
			{
				maxFrame = std::max(maxFrame, frame(imported));
				frames++;
				if (*completed && !imported.IsValid())
				{
					INFO((*completed)->Error);
					REQUIRE((*completed)->Success);
					imported = *UUIDFromJson((*completed)->Value["asset"]);
				}
			}
			REQUIRE(AssetManager::GetAssetState(imported) == AssetState::Ready);
			CHECK(frames > 1); // The import outlasted a frame: it ran beside them
			CHECK(AssetManager::GetAsset<Texture>(imported)->GetWidth() == 4096u);
			MESSAGE("asset.import of a 4096x4096 texture until its mip tail is in: ", frames, " frames, the longest ", maxFrame, " ms");
			return maxFrame;
		});
		device.WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Rendering the thumbnails of a project never holds a frame")
	{
		// The Content Browser showing every asset of a stress project for the first time: the thumbnails of its textures
		// (mip tails decoded on I/O jobs), materials, model and mesh (rendered one at a time, read back without stalling),
		// encoded and written as PNG files on I/O jobs. Frames as in the tests above, on the shared device.
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		GraphicsDevice& device = gpu.GetDevice();
		Perf::ScopedApplicationJobSystem jobs;
		StressProjectSpec spec;
		spec.Textures2k = 8;
		spec.Textures4k = 1;
		spec.Materials = 9;
		spec.Entities = 100;
		spec.GridResolution = 128;
		std::string error;
		const std::optional<StressProject> project = WriteStressProject(CreateTemporaryDirectory("PerfGPUThumbnails") / "Project", spec, error);
		REQUIRE_MESSAGE(project, error);
		EditorContext context { EditorContextSpecification { false } };
		REQUIRE_MESSAGE(context.OpenProject(project->Directory, &error), error);
		ThumbnailCache* thumbnails = context.GetThumbnails();
		REQUIRE(thumbnails);
		std::vector<AssetHandle> assets = project->Textures;
		assets.insert(assets.end(), project->Materials.begin(), project->Materials.end());
		assets.push_back(project->GridModel);
		assets.push_back(project->GridMesh);

		const auto frame = [&]()
		{
			REQUIRE(device.BeginFrame());
			const auto start = std::chrono::steady_clock::now();
			Renderer::BeginFrame();
			for (AssetHandle asset : assets)
				thumbnails->Request(asset);
			context.Update(Timestep(1.0f / 60.0f));
			const double milliseconds = MillisecondsSince(start);
			device.EndFrame();
			// Frames at the editor's pace rather than as fast as the loop spins.
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return milliseconds;
		};
		const auto renderAll = [&]()
		{
			double maxFrame = 0.0;
			uint32_t frames = 0;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
			do
			{
				maxFrame = std::max(maxFrame, frame());
				frames++;
			}
			while (thumbnails->IsBusy() && std::chrono::steady_clock::now() < deadline);
			REQUIRE_FALSE(thumbnails->IsBusy());
			for (AssetHandle asset : assets)
				CHECK(thumbnails->GetState(asset) == ThumbnailState::Ready);
			CHECK(thumbnails->GetStats().Failed == 0);
			MESSAGE(assets.size(), " thumbnails: ", frames, " frames, the longest ", maxFrame, " ms");
			return maxFrame;
		};
		// The thumbnail renderer's first frame creates its targets and pipelines, once per session: done before the
		// measurement, as every thumbnail render after it measures the steady state.
		const std::vector<AssetHandle> everything = assets;
		assets = { project->Materials.front() };
		renderAll();
		assets = everything;

		// Each attempt starts as the first does: without thumbnails, with nothing of an earlier attempt left to destroy, and
		// with the project's assets unloaded, so the textures the materials draw stream their levels anew. Confirmations
		// otherwise measured another state: their first frame destroyed the 20 thumbnails of the attempt before (2.1-2.3 ms
		// while frames destroyed everything at once, DeferredReleaseQueue), and the textures had their levels already (43-45
		// frames instead of 59-61).
		EditorAssetManager* manager = context.GetAssetManager();
		REQUIRE(manager);
		const auto startCold = [&]()
		{
			thumbnails->Clear();
			device.WaitForIdle();
			for (AssetHandle asset : everything)
				manager->UnloadAsset(asset);
			device.WaitForIdle();
			Renderer::GetDeferredReleases().Collect();
			REQUIRE(Renderer::GetDeferredReleases().GetPendingCount() == 0);
		};

		// The longest frame (a maximum by nature: one held frame is the hitch the user sees) until every thumbnail is ready.
		uint64_t rendered = 0;
		Perf::CheckBudgetConfirmed("PerfGPU.Editor.ThumbnailsMaxFrame", [&]
		{
			startCold();
			const uint64_t before = thumbnails->GetStats().ScenesRendered;
			const double maxFrame = renderAll();
			rendered = thumbnails->GetStats().ScenesRendered - before;
			return maxFrame;
		});
		CHECK(rendered == spec.Materials + 2);
		device.WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
