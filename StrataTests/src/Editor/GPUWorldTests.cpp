#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorViewport.h"
#include "Editor/ViewportRenderer.h"
#include "Renderer/GPUTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Scene/SceneTestUtils.h"
#include "TestHelpers.h"
#include "World/WorldTestUtils.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Math/Math.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Renderer.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneWorkBudget.h>
#include <Strata/World/WorldComponents.h>
#include <Strata/World/WorldStreamingSystem.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string_view>
#include <thread>

using namespace Strata;

TEST_SUITE("GPU.World")
{
	TEST_CASE("The viewport picks entities 20 km from the origin through the entity-ID buffer")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		auto run = [&](std::string_view name, const nlohmann::json& parameters)
		{
			const EditorCommandResult result = commands.Execute(context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		};
		// A cube in the middle of the view and its neighbor at its right, 20 km out on X, seen from 4 m.
		auto create = [&](const char* name, float x)
		{
			const nlohmann::json created = run("entity.create", { { "name", name }, { "components", {
				{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } },
				{ "Transform", { { "Translation", { x, 0.0f, 0.0f } } } } } } });
			return *UUIDFromJson(created["id"]);
		};
		constexpr float c_FarX = 20'000.0f;
		const UUID cube = create("Far Cube", c_FarX);
		const UUID neighbor = create("Far Neighbor", c_FarX + 1.5f);
		run("camera.set", { { "position", { c_FarX, 0.0f, 4.0f } }, { "target", { c_FarX, 0.0f, 0.0f } } });

		EditorViewport& viewport = context.GetViewport();
		ViewportRenderer* renderer = viewport.GetRenderer();
		REQUIRE(renderer);
		const glm::uvec2 size(64, 64);
		auto render = [&]()
		{
			const std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Automatic, 1.0f);
			REQUIRE(view);
			REQUIRE(renderer->Render(context, size, *view, viewport.GetSettings(), view->EditorOverlays));
		};
		auto pick = [&](const glm::uvec2& pixel)
		{
			render();
			REQUIRE(viewport.RequestPick(pixel, size, ViewportPickMode::Replace));
			for (int frame = 0; frame < 5000 && viewport.IsPickPending(); frame++)
			{
				viewport.UpdatePicking(context);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			REQUIRE_FALSE(viewport.IsPickPending());
			return context.GetSelection();
		};
		CHECK(pick(glm::uvec2(32, 32)) == std::vector<UUID> { cube });
		CHECK(pick(glm::uvec2(58, 32)) == std::vector<UUID> { neighbor });
		CHECK(pick(glm::uvec2(32, 2)).empty());
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A streaming world prepares no more render proxies per frame than the frame's entity budget")
	{
		// A 6 x 6 world of 60 cubes per cell, streamed by a source driven across it and back while a renderer draws every
		// frame. Cells instantiate within the scene's entity budget, and the render scene prepares the proxies of what was
		// created in the same frame: never more than that budget. (A cell leaves rendering in the frame its unload starts,
		// by design: its proxies turn inactive then, which the renderer counts as updates too.)
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Tests::ScopedJobSystem jobs(2, 1);
		Tests::GeneratedWorldSpecification specification;
		specification.GridX = 6;
		specification.GridZ = 6;
		specification.EntitiesPerCell = 60;
		specification.BodiesPerCell = 0;
		Tests::MountedWorld mounted("GPUWorldProxies", specification);

		Scene scene("Persistent");
		Tests::AddNeutralPostProcess(scene);
		constexpr uint32_t c_EntityBudget = 50;
		scene.GetSettings().MaxEntitiesInstantiatedPerFrame = c_EntityBudget;
		scene.GetSettings().MaxEntitiesDestroyedPerFrame = 40;
		scene.CreateEntity("World").AddComponent<WorldComponent>().World = mounted.World.World;
		Entity source = scene.CreateEntity("Source");
		source.AddComponent<StreamingSourceComponent>().Radius = 40.0f;
		scene.OnRuntimeStart();
		const WorldStreamingSystem* world = scene.GetSystem<WorldStreamingSystem>();
		REQUIRE(world);

		SceneRenderer renderer;
		renderer.SetViewportSize(64, 64);
		SceneCamera camera = Tests::LookAt(glm::vec3(96.0f, 260.0f, 97.0f), glm::vec3(96.0f, 0.0f, 96.0f));
		camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f);
		camera.Far = 1000.0f;
		uint32_t mostPrepared = 0;
		uint64_t prepared = 0;
		uint64_t unloads = 0;
		uint32_t mostVisible = 0;
		for (uint32_t frame = 0; frame < 600; frame++)
		{
			// Across the world and back at 1.2 m per frame, along its middle row.
			const float x = 16.0f + 160.0f * std::abs(std::fmod(static_cast<float>(frame) / 150.0f, 2.0f) - 1.0f);
			source.GetComponent<TransformComponent>().Translation = glm::vec3(x, 1.0f, 80.0f);
			source.MarkModified<TransformComponent>();
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			mounted.Manager->Update();
			SceneWorkBudget budget = SceneWorkBudget::ForFrame(scene.GetSettings());
			scene.OnUpdateRuntime(Timestep(1.0f / 60.0f), &budget);
			REQUIRE(renderer.Render(scene, camera));
			gpu.GetDevice().EndFrame();

			REQUIRE(world->GetStreamer());
			uint32_t unloadsStarted = 0;
			for (const WorldStreamingDecision& decision : world->GetStreamer()->GetLastDecisions())
			{
				if (decision.Kind == WorldStreamingDecision::Action::Unload)
					unloadsStarted++;
			}
			const uint32_t deactivated = unloadsStarted * specification.EntitiesPerCell;
			const SceneRendererStats& stats = renderer.GetStats();
			const WorldStreamingStats streaming = world->GetStreamer()->GetStats();
			INFO("frame ", frame, ": ", stats.ProxyUpdates, " proxy updates, ", streaming.EntitiesCreatedLastUpdate, " entities created, ",
				unloadsStarted, " unloads started");
			CHECK(streaming.EntitiesCreatedLastUpdate <= c_EntityBudget);
			CHECK(stats.ProxyUpdates <= streaming.EntitiesCreatedLastUpdate + deactivated);
			const uint32_t preparedThisFrame = stats.ProxyUpdates - std::min(stats.ProxyUpdates, deactivated);
			CHECK(preparedThisFrame <= c_EntityBudget);
			mostPrepared = std::max(mostPrepared, preparedThisFrame);
			prepared += preparedThisFrame;
			unloads += unloadsStarted;
			mostVisible = std::max(mostVisible, stats.VisibleProxies);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		MESSAGE("Most proxies prepared in a frame: ", mostPrepared, ", in all: ", prepared, "; ", unloads, " unloads");
		// The drive streamed: cells came and went, and their cubes were drawn.
		CHECK(prepared > 1000);
		CHECK(unloads > 0);
		CHECK(mostVisible >= specification.EntitiesPerCell);
		Tests::CheckSceneCaches(scene);
		scene.OnRuntimeStop();
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
