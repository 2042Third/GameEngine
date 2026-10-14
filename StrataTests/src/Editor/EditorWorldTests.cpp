#include <doctest/doctest.h>

#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "EditorLayer.h"
#include "Panels/StreamingPanel.h"
#include "Physics/PhysicsTestUtils.h"
#include "UI/ItemProbe.h"

#include <Strata/Physics/PhysicsSystem.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// An editor without a project, past the launcher, editing a scene of a floor and `boxes` boxes at rest on it.
	struct PhysicsEditor : HarnessEditor
	{
		explicit PhysicsEditor(uint32_t boxes)
		{
			Layer->DismissLauncher();
			Scene& scene = *Context().GetActiveScene();
			CreateGround(scene);
			for (uint32_t index = 0; index < boxes; index++)
				CreateDynamicBox(scene, "Box " + std::to_string(index), glm::vec3(static_cast<float>(index) * 2.0f - 10.0f, 0.5f, 0.0f));
			Frames(2);
		}
	};

}

TEST_SUITE("Editor.World")
{
	TEST_CASE("physics.stats reports the running game's bodies, capacity and growth, and nothing while editing")
	{
		PhysicsEditor editor(10);
		CHECK(editor.Run("physics.stats") == nlohmann::json { { "running", false } });

		editor.Run("play.start");
		editor.Frames(3);
		nlohmann::json stats = editor.Run("physics.stats");
		CHECK(stats["running"] == true);
		CHECK(stats["simulating"] == true);
		CHECK(stats["bodies"]["total"] == 11);
		CHECK(stats["bodies"]["static"] == 1);
		CHECK(stats["bodies"]["dynamic"] == 10);
		CHECK(stats["bodies"]["slotsUsed"] == 11);
		CHECK(stats["capacity"]["maxBodies"] == 65536);
		CHECK(stats["capacity"]["reservedMB"].get<double>() > 1.0);
		CHECK(stats["waitingForSlot"] == 0);
		CHECK(stats["growths"]["count"] == 0);
		CHECK(stats["steps"].get<uint64_t>() > 0);

		// What the last frame built and destroyed.
		Scene& game = *editor.Context().GetActiveScene();
		CreateDynamicBox(game, "Spawned", glm::vec3(0.0f, 5.0f, 0.0f));
		editor.Frames(1);
		CHECK(editor.Run("physics.stats")["addedLastFrame"] == 1);
		editor.Frames(1);
		CHECK(editor.Run("physics.stats")["addedLastFrame"] == 0);

		editor.Run("play.stop");
		editor.Frames(1);
		CHECK(editor.Run("physics.stats") == nlohmann::json { { "running", false } });
	}

	TEST_CASE("Bodies without collision show in a pill that opens the Streaming panel's Physics section")
	{
		// A world held at its growth limit: 6 of the 11 bodies wait for a slot while the game runs.
		PhysicsSettings limited;
		limited.GrowthLimit = 5;
		ScopedPhysicsSettings scopedLimit(limited);
		PhysicsEditor editor(10);
		editor.Context().GetActiveScene()->GetSettings().PhysicsCapacity.MaxBodies = 5;
		CHECK_FALSE(UI::ItemProbe::Find("Status.Physics").has_value());

		editor.Run("play.start");
		editor.Frames(3);
		CHECK(editor.GetPillText("Status.Physics") == "6 bodies without collision");
		CHECK(editor.Run("physics.stats")["waitingForSlot"] == 6);
		CHECK_FALSE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming));

		REQUIRE(editor.Click("Status.Physics"));
		editor.Frames(3);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming));
		const StreamingPanel* streaming = editor.Layer->GetPanels().Get<StreamingPanel>(EditorPanels::c_Streaming);
		REQUIRE(streaming);
		REQUIRE(streaming->GetPhysics().has_value());
		CHECK(streaming->GetPhysics()->Bodies == 5);
		CHECK(streaming->GetPhysics()->MaxBodies == 5);
		const std::optional<UI::ItemProbe::Item> bodies = UI::ItemProbe::Find("Streaming.PhysicsBodies");
		REQUIRE(bodies);
		CHECK(UI::ItemProbe::Find("Streaming.PhysicsWaiting").has_value());

		// Removed bodies make room: the waiting ones get theirs, and the pill goes.
		Scene& game = *editor.Context().GetActiveScene();
		std::vector<Entity> removed;
		for (uint32_t index = 0; index < 6; index++)
			removed.push_back(game.FindEntityByName("Box " + std::to_string(index)));
		game.DestroyEntities(removed);
		editor.Frames(3);
		CHECK(editor.Run("physics.stats")["waitingForSlot"] == 0);
		CHECK_FALSE(UI::ItemProbe::Find("Status.Physics").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Streaming.PhysicsWaiting").has_value());

		// While editing there is no physics to show.
		editor.Run("play.stop");
		editor.Frames(2);
		CHECK_FALSE(streaming->GetPhysics().has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Streaming.PhysicsBodies").has_value());
	}
}
