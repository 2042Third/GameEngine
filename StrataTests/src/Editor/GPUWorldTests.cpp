#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorViewport.h"
#include "Editor/ViewportRenderer.h"
#include "Renderer/GPUTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <chrono>
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
}
