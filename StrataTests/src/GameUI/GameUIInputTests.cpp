#include <doctest/doctest.h>

#include "Perf/SceneGenerators.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/GameUI/GameUIComponents.h"
#include "Strata/GameUI/GameUIHitTest.h"
#include "Strata/GameUI/GameUIScene.h"
#include "Strata/GameUI/GameUISystem.h"
#include "Strata/Input/Input.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneServices.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

namespace
{

	constexpr float c_FrameTime = 1.0f / 60.0f;

	UIRectComponent BoxRect(const glm::vec2& min, const glm::vec2& size)
	{
		UIRectComponent rect;
		rect.AnchorMin = glm::vec2(0.0f);
		rect.AnchorMax = glm::vec2(0.0f);
		rect.Pivot = glm::vec2(0.0f);
		rect.Position = min;
		rect.Size = size;
		return rect;
	}

	// A scene whose canvases have one unit per pixel (ConstantPixelSize) on a 1920x1080 input screen, so rects are where the
	// pointer is. Play starts with Play(); every Frame() is an input frame and a runtime update.
	struct UIInputScene
	{
		Ref<Scene> Owner = CreateRef<Scene>("UI Input");
		Scene& Target = *Owner;
		Entity Canvas;
		std::vector<UIEventInfo> Events;

		UIInputScene()
		{
			Input::Reset();
			Input::SetViewport(glm::vec2(0.0f), glm::vec2(1920.0f, 1080.0f));
			Canvas = AddCanvas("Canvas");
		}

		~UIInputScene()
		{
			if (Target.IsRunning())
				Target.OnRuntimeStop();
			Input::Reset();
		}

		UIInputScene(const UIInputScene&) = delete;
		UIInputScene& operator=(const UIInputScene&) = delete;

		Entity AddCanvas(const std::string& name, int32_t sortOrder = 0)
		{
			Entity canvas = Target.CreateEntity(name);
			canvas.AddComponent<UIRectComponent>();
			UICanvasComponent& component = canvas.AddComponent<UICanvasComponent>();
			component.ScaleMode = UICanvasScaleMode::ConstantPixelSize;
			component.SortOrder = sortOrder;
			return canvas;
		}

		Entity AddImage(Entity parent, const std::string& name, const glm::vec2& min, const glm::vec2& size, bool raycastTarget = true)
		{
			Entity entity = Target.CreateChildEntity(parent, name);
			entity.AddComponent<UIRectComponent>(BoxRect(min, size));
			entity.AddComponent<UIImageComponent>().RaycastTarget = raycastTarget;
			return entity;
		}

		Entity AddButton(Entity parent, const std::string& name, const glm::vec2& min, const glm::vec2& size)
		{
			Entity button = AddImage(parent, name, min, size);
			button.AddComponent<UISelectableComponent>();
			button.AddComponent<UIButtonComponent>();
			return button;
		}

		Entity AddToggle(Entity parent, const std::string& name, const glm::vec2& min, UUID group = UUID::Null())
		{
			Entity toggle = AddImage(parent, name, min, glm::vec2(40.0f));
			toggle.AddComponent<UISelectableComponent>();
			toggle.AddComponent<UIToggleComponent>().Group = group;
			return toggle;
		}

		Entity AddSlider(Entity parent, const std::string& name, const glm::vec2& min, const glm::vec2& size, UIFillDirection direction)
		{
			Entity slider = AddImage(parent, name, min, size);
			slider.AddComponent<UISelectableComponent>();
			UISliderComponent& component = slider.AddComponent<UISliderComponent>();
			component.MinValue = 0.0f;
			component.MaxValue = 10.0f;
			component.Value = 4.0f;
			component.Direction = direction;
			return slider;
		}

		void Play()
		{
			Target.OnRuntimeStart();
			IUIEvents* events = Target.GetService<IUIEvents>();
			REQUIRE(events);
			CHECK(events->AddUIEventListener([this](const UIEventInfo& event) { Events.push_back(event); }) != 0);
		}

		void Frame(float deltaTime = c_FrameTime)
		{
			Input::BeginFrame();
			Target.OnUpdateRuntime(deltaTime);
		}

		void MoveTo(const glm::vec2& position)
		{
			Input::SimulateMouseMove(position);
			Frame();
		}

		void Press()
		{
			Input::SimulateMouseButton(Mouse::ButtonLeft, true);
			Frame();
		}

		void Release()
		{
			Input::SimulateMouseButton(Mouse::ButtonLeft, false);
			Frame();
		}

		void Click(const glm::vec2& position)
		{
			MoveTo(position);
			Press();
			Release();
		}

		void Key(KeyCode key, float deltaTime = c_FrameTime)
		{
			Input::SimulateKey(key, true);
			Frame(deltaTime);
			Input::SimulateKey(key, false);
			Frame(deltaTime);
		}

		GameUISystem& System()
		{
			GameUISystem* system = Target.GetSystem<GameUISystem>();
			REQUIRE(system);
			return *system;
		}

		std::string Focused()
		{
			const Entity focused = System().GetFocused();
			return focused ? focused.GetName() : std::string();
		}

		size_t Count(UIEventType type) const
		{
			return static_cast<size_t>(std::count_if(Events.begin(), Events.end(), [type](const UIEventInfo& event) { return event.Type == type; }));
		}

		UIVisualState State(Entity selectable)
		{
			return GameUIScene::Get(Target).GetVisualState(selectable.GetHandle(), selectable.GetComponent<UISelectableComponent>());
		}
	};

	// The hit tester over the scene's current layout of the default 1920x1080 screen; hits by entity name.
	struct Hits
	{
		Scene& Target;
		GameUIScene& UI;
		GameUIHitTester Tester;
		std::vector<UICanvasView> Views;

		explicit Hits(Scene& scene)
			: Target(scene), UI(GameUIScene::Get(scene))
		{
			Update();
		}

		bool Update()
		{
			const std::span<const UICanvasView> views = UI.EnsureLayout(GameUIScene::c_DefaultScreen);
			Views.assign(views.begin(), views.end());
			return Tester.Update(UI, Views);
		}

		std::string At(const glm::vec2& pixel, bool anyCanvas = false)
		{
			const std::optional<UIHitResult> hit = Tester.HitTest(pixel, anyCanvas);
			return hit ? Entity(hit->Entity, &Target).GetName() : std::string();
		}
	};

	// The topmost raycast target by brute force: every element of every view, from the last drawn.
	entt::entity FindTopmostByBruteForce(GameUIScene& ui, std::span<const UICanvasView> views, const glm::vec2& pixel)
	{
		for (size_t view = views.size(); view-- > 0;)
		{
			if (!views[view].ReceivesInput)
				continue;
			const glm::vec2 point = pixel / views[view].Scale;
			for (uint32_t element = ui.GetElementCount(views[view]); element-- > 0;)
			{
				if (ui.IsElementVisible(views[view], element) && ui.IsRaycastTarget(views[view], element) && ui.GetElementBox(views[view], element).Contains(point))
					return ui.GetElementEntity(views[view], element);
			}
		}
		return entt::null;
	}

	// The game's window on a display with a content scale: its size in pixels.
	struct FixedGameWindow final : public IGameWindow
	{
		GameWindowState State;

		bool SetWindowMode(GameWindowMode) override { return false; }
		bool SetResolution(uint32_t, uint32_t) override { return false; }
		bool SetVSync(bool) override { return false; }
		GameWindowState GetState() override { return State; }
	};

	// A 3x3 grid of buttons named "B<row><column>", 200 units apart across and 150 down.
	std::vector<Entity> AddGrid(UIInputScene& scene)
	{
		std::vector<Entity> buttons;
		for (int row = 0; row < 3; row++)
		{
			for (int column = 0; column < 3; column++)
			{
				const std::string name = "B" + std::to_string(row) + std::to_string(column);
				buttons.push_back(scene.AddButton(scene.Canvas, name, { 100.0f + 200.0f * column, 100.0f + 150.0f * row }, { 150.0f, 100.0f }));
			}
		}
		return buttons;
	}

}

TEST_SUITE("GameUI.HitTest")
{
	TEST_CASE("The topmost raycast target wins: canvases by sort order, then the last element drawn")
	{
		UIInputScene scene;
		// The high canvas comes first in the hierarchy: its sort order, not its place, puts it on top.
		Entity high = scene.AddCanvas("High", 5);
		scene.Target.SetSiblingIndex(high, 0);
		scene.AddImage(high, "C", { 150.0f, 150.0f }, { 50.0f, 50.0f });
		scene.AddImage(scene.Canvas, "A", { 0.0f, 0.0f }, { 200.0f, 200.0f });
		Entity b = scene.AddImage(scene.Canvas, "B", { 100.0f, 100.0f }, { 200.0f, 200.0f });
		scene.AddImage(b, "B Child", { 150.0f, 0.0f }, { 50.0f, 50.0f }); // Drawn after its parent: on top of it

		Hits hits(scene.Target);
		CHECK(hits.At({ 50.0f, 50.0f }) == "A");
		CHECK(hits.At({ 120.0f, 120.0f }) == "B");
		CHECK(hits.At({ 260.0f, 120.0f }) == "B Child");
		CHECK(hits.At({ 160.0f, 160.0f }) == "C");
		CHECK(hits.At({ 500.0f, 500.0f }).empty());

		high.GetComponent<UICanvasComponent>().SortOrder = -1;
		high.MarkModified<UICanvasComponent>();
		CHECK(hits.Update());
		CHECK(hits.At({ 160.0f, 160.0f }) == "B");
		CHECK_FALSE(hits.Update()); // Nothing changed since
	}

	TEST_CASE("Elements that are not raycast targets let the pointer through")
	{
		UIInputScene scene;
		scene.AddImage(scene.Canvas, "Back", { 0.0f, 0.0f }, { 400.0f, 400.0f });
		scene.AddImage(scene.Canvas, "Decoration", { 0.0f, 0.0f }, { 100.0f, 100.0f }, false);
		Entity label = scene.Target.CreateChildEntity(scene.Canvas, "Label");
		label.AddComponent<UIRectComponent>(BoxRect({ 100.0f, 0.0f }, { 100.0f, 100.0f }));
		label.AddComponent<UITextComponent>(); // Texts are not raycast targets by default
		Entity bare = scene.Target.CreateChildEntity(scene.Canvas, "Bare Selectable");
		bare.AddComponent<UIRectComponent>(BoxRect({ 200.0f, 0.0f }, { 100.0f, 100.0f }));
		bare.AddComponent<UISelectableComponent>(); // A selectable is a target without an image

		Hits hits(scene.Target);
		CHECK(hits.At({ 50.0f, 50.0f }) == "Back");
		CHECK(hits.At({ 150.0f, 50.0f }) == "Back");
		CHECK(hits.At({ 250.0f, 50.0f }) == "Bare Selectable");

		label.GetComponent<UITextComponent>().RaycastTarget = true;
		label.MarkModified<UITextComponent>();
		hits.Update();
		CHECK(hits.At({ 150.0f, 50.0f }) == "Label");
	}

	TEST_CASE("Canvases that do not receive input, inactive elements and hidden checkmarks are not hit")
	{
		UIInputScene scene;
		scene.AddImage(scene.Canvas, "Back", { 0.0f, 0.0f }, { 400.0f, 400.0f });
		Entity overlay = scene.AddCanvas("Overlay", 3);
		overlay.GetComponent<UICanvasComponent>().ReceivesInput = false;
		scene.AddImage(overlay, "Overlay Image", { 0.0f, 0.0f }, { 100.0f, 100.0f });
		Entity hidden = scene.AddImage(scene.Canvas, "Hidden", { 100.0f, 0.0f }, { 100.0f, 100.0f });
		hidden.SetActive(false);
		Entity toggle = scene.AddToggle(scene.Canvas, "Toggle", { 200.0f, 200.0f });
		Entity check = scene.AddImage(toggle, "Check", { 0.0f, 0.0f }, { 100.0f, 100.0f });
		toggle.GetComponent<UIToggleComponent>().Checkmark = check.GetUUID();
		toggle.GetComponent<UIToggleComponent>().IsOn = false;

		Hits hits(scene.Target);
		CHECK(hits.At({ 50.0f, 50.0f }) == "Back");
		CHECK(hits.At({ 50.0f, 50.0f }, true) == "Overlay Image"); // Tools may look at every canvas
		CHECK(hits.At({ 150.0f, 50.0f }) == "Back");
		CHECK(hits.At({ 270.0f, 270.0f }) == "Back"); // The checkmark of a toggle that is off
		CHECK(hits.At({ 210.0f, 210.0f }) == "Toggle");

		toggle.GetComponent<UIToggleComponent>().IsOn = true;
		toggle.MarkModified<UIToggleComponent>();
		hidden.SetActive(true);
		CHECK(hits.Update());
		CHECK(hits.At({ 270.0f, 270.0f }) == "Check");
		CHECK(hits.At({ 150.0f, 50.0f }) == "Hidden");
	}

	TEST_CASE("Only changes of what hit tests see rebuild the grids, and an unsignaled one fails validation")
	{
		UIInputScene scene;
		scene.AddImage(scene.Canvas, "Back", { 0.0f, 0.0f }, { 400.0f, 400.0f });
		Entity image = scene.AddImage(scene.Canvas, "Image", { 100.0f, 100.0f }, { 100.0f, 100.0f });
		Entity label = scene.Target.CreateChildEntity(scene.Canvas, "Label");
		label.AddComponent<UIRectComponent>(BoxRect({ 200.0f, 0.0f }, { 100.0f, 100.0f }));
		label.AddComponent<UITextComponent>();
		Hits hits(scene.Target);
		CHECK_FALSE(hits.Update());

		// Colors and texts change every frame in games: the grids stay.
		image.GetComponent<UIImageComponent>().Color = { 1.0f, 0.0f, 0.0f, 1.0f };
		image.MarkModified<UIImageComponent>();
		label.GetComponent<UITextComponent>().Text = "Score 10";
		label.MarkModified<UITextComponent>();
		CHECK_FALSE(hits.Update());

		image.GetComponent<UIImageComponent>().RaycastTarget = false;
		image.MarkModified<UIImageComponent>();
		CHECK(hits.Update());
		CHECK(hits.At({ 150.0f, 150.0f }) == "Back");
		image.AddComponent<UISelectableComponent>();
		CHECK(hits.Update());
		CHECK(hits.At({ 150.0f, 150.0f }) == "Image");
		image.RemoveComponent<UISelectableComponent>();
		CHECK(hits.Update());
		CHECK(hits.At({ 150.0f, 150.0f }) == "Back");
		image.GetComponent<UIImageComponent>().RaycastTarget = true;
		image.MarkModified<UIImageComponent>();
		image.RemoveComponent<UIImageComponent>();
		CHECK(hits.Update());
		CHECK(hits.At({ 150.0f, 150.0f }) == "Back");
		image.AddComponent<UIImageComponent>();
		CHECK(hits.Update());
		CHECK(hits.At({ 150.0f, 150.0f }) == "Image");

		label.GetComponent<UITextComponent>().RaycastTarget = true; // No signal
		std::string error;
		CHECK_FALSE(hits.UI.Validate(&error));
		CHECK(error.find("'Label'") != std::string::npos);
		CHECK(error.find("raycast target") != std::string::npos);
		label.MarkModified<UITextComponent>();
		CHECK(hits.UI.Validate());
		CHECK(hits.Update());
		CHECK(hits.At({ 250.0f, 50.0f }) == "Label");
	}

	TEST_CASE("Rect edges are half-open: the minimum edges hit, the maximum edges do not")
	{
		UIInputScene scene;
		scene.AddImage(scene.Canvas, "Box", { 100.0f, 100.0f }, { 50.0f, 50.0f });
		Hits hits(scene.Target);
		CHECK(hits.At({ 100.0f, 100.0f }) == "Box");
		CHECK(hits.At({ 149.99f, 149.99f }) == "Box");
		CHECK(hits.At({ 150.0f, 120.0f }).empty());
		CHECK(hits.At({ 120.0f, 150.0f }).empty());
		CHECK(hits.At({ 99.99f, 120.0f }).empty());
	}

	TEST_CASE("A query compares at most 64 candidates among 10,000 elements and finds what a full search finds")
	{
		Scene scene("Widgets");
		Tests::Perf::CreateUIWidgets(scene);
		GameUIScene& ui = GameUIScene::Get(scene);
		const std::span<const UICanvasView> laidOut = ui.EnsureLayout(GameUIScene::c_DefaultScreen);
		const std::vector<UICanvasView> views(laidOut.begin(), laidOut.end());
		REQUIRE(ui.GetElementCount(views.front()) == 10'001);
		GameUIHitTester tester;
		CHECK(tester.Update(ui, views));

		uint32_t maxCandidates = 0;
		uint32_t hitCount = 0;
		for (float y = 0.5f; y < 1080.0f; y += 7.3f)
		{
			for (float x = 0.5f; x < 1920.0f; x += 11.1f)
			{
				const std::optional<UIHitResult> hit = tester.HitTest({ x, y });
				maxCandidates = std::max(maxCandidates, tester.GetLastCandidateCount());
				const entt::entity expected = FindTopmostByBruteForce(ui, views, { x, y });
				CHECK((hit ? hit->Entity : entt::null) == expected);
				hitCount += hit ? 1 : 0;
			}
		}
		CHECK(hitCount > 1000);
		CHECK(maxCandidates <= 64);
		MESSAGE("At most ", maxCandidates, " candidates per query");
	}
}

TEST_SUITE("GameUI.Capture")
{
	TEST_CASE("A press over the UI is hidden from the game until its release, also when the pointer leaves the UI")
	{
		UIInputScene scene;
		scene.AddImage(scene.Canvas, "Panel", { 100.0f, 100.0f }, { 200.0f, 200.0f });
		scene.Play();
		scene.MoveTo({ 150.0f, 150.0f });
		CHECK(scene.System().IsPointerOverUI());
		Input::SimulateScroll({ 0.0f, 1.0f });
		scene.Frame();
		CHECK(Input::GetScrollDelta() == glm::vec2(0.0f)); // Scrolling over the UI is the UI's

		scene.Press();
		CHECK(Input::IsPointerCaptured());
		CHECK_FALSE(Input::IsMouseButtonPressed(Mouse::ButtonLeft));
		CHECK_FALSE(Input::IsMouseButtonDown(Mouse::ButtonLeft));
		scene.MoveTo({ 900.0f, 900.0f });
		CHECK_FALSE(scene.System().IsPointerOverUI());
		CHECK_FALSE(Input::IsMouseButtonDown(Mouse::ButtonLeft));
		Input::SimulateScroll({ 0.0f, 1.0f });
		scene.Frame();
		CHECK(Input::GetScrollDelta() == glm::vec2(0.0f)); // Still the UI's gesture
		scene.Release();
		CHECK_FALSE(Input::IsMouseButtonReleased(Mouse::ButtonLeft)); // The release ends the UI's gesture, unseen

		// Off the UI with no gesture, the game has the pointer again.
		scene.Frame();
		CHECK_FALSE(Input::IsPointerCaptured());
		scene.Press();
		CHECK(Input::IsMouseButtonPressed(Mouse::ButtonLeft));
	}

	TEST_CASE("A press elsewhere stays the game's when the pointer moves over the UI")
	{
		UIInputScene scene;
		Entity button = scene.AddButton(scene.Canvas, "Button", { 100.0f, 100.0f }, { 200.0f, 200.0f });
		scene.Play();
		scene.MoveTo({ 900.0f, 900.0f });
		scene.Press();
		CHECK(Input::IsMouseButtonPressed(Mouse::ButtonLeft));
		scene.MoveTo({ 150.0f, 150.0f });
		CHECK(scene.System().IsPointerOverUI());
		CHECK(Input::IsMouseButtonDown(Mouse::ButtonLeft));
		CHECK(scene.State(button) == UIVisualState::Hover); // Hovered, not pressed: the press was not over it
		scene.Release();
		CHECK(Input::IsMouseButtonReleased(Mouse::ButtonLeft));
		CHECK(scene.Count(UIEventType::Click) == 0);
		// The next frame over the UI hides the buttons again.
		scene.Frame();
		CHECK(Input::IsPointerCaptured());
	}

	TEST_CASE("A scene without UI under the pointer gives the game every click, and stopping play clears capture")
	{
		UIInputScene scene;
		scene.AddImage(scene.Canvas, "Panel", { 100.0f, 100.0f }, { 200.0f, 200.0f });
		scene.Play();
		scene.MoveTo({ 600.0f, 600.0f });
		scene.Press();
		CHECK(Input::IsMouseButtonPressed(Mouse::ButtonLeft));
		CHECK_FALSE(scene.System().IsPointerOverUI());
		scene.Release();

		scene.MoveTo({ 150.0f, 150.0f });
		scene.Press();
		CHECK(Input::IsPointerCaptured());
		scene.Target.OnRuntimeStop();
		CHECK_FALSE(Input::IsPointerCaptured());
	}

	TEST_CASE("On a display with a content scale, input coordinates become the pixels the game renders")
	{
		UIInputScene scene;
		scene.AddButton(scene.Canvas, "Pixels", { 200.0f, 200.0f }, { 100.0f, 100.0f });
		Entity physical = scene.AddCanvas("Physical", 1);
		physical.GetComponent<UICanvasComponent>().ScaleMode = UICanvasScaleMode::ConstantPhysicalSize;
		scene.AddButton(physical, "Units", { 300.0f, 100.0f }, { 50.0f, 50.0f }); // Pixels (600, 200) to (700, 300)
		FixedGameWindow window;
		window.State.Width = 2048;
		window.State.Height = 1152;
		window.State.ContentScale = 2.0f;
		EngineServices::ScopedProvider<IGameWindow> provider(window);
		Input::SetViewport(glm::vec2(0.0f), glm::vec2(1024.0f, 576.0f)); // Points
		const UIInputScreen screen = GameUIScene::GetInputScreen();
		CHECK(screen.Screen.Size == glm::vec2(2048.0f, 1152.0f));
		CHECK(screen.Screen.ContentScale == 2.0f);
		CHECK(screen.InputToPixels == glm::vec2(2.0f));

		scene.Play();
		scene.MoveTo({ 125.0f, 125.0f });
		CHECK(scene.System().GetHovered().GetName() == "Pixels");
		scene.MoveTo({ 325.0f, 125.0f });
		CHECK(scene.System().GetHovered().GetName() == "Units");
		scene.MoveTo({ 90.0f, 90.0f }); // Pixels (180, 180): beside both
		CHECK_FALSE(scene.System().GetHovered());
		scene.Target.OnRuntimeStop();
	}

	TEST_CASE("Without canvases the UI system leaves input alone")
	{
		Input::Reset();
		Ref<Scene> scene = CreateRef<Scene>("No UI");
		scene->OnRuntimeStart();
		Input::SimulateMouseMove({ 10.0f, 10.0f });
		Input::SimulateMouseButton(Mouse::ButtonLeft, true);
		Input::BeginFrame();
		scene->OnUpdateRuntime(c_FrameTime);
		CHECK(Input::IsMouseButtonPressed(Mouse::ButtonLeft));
		CHECK_FALSE(Input::IsPointerCaptured());
		GameUISystem* system = scene->GetSystem<GameUISystem>();
		REQUIRE(system);
		CHECK_FALSE(system->IsPointerOverUI());
		CHECK(GameUIScene::Find(*scene) == nullptr); // Nothing was even created
		scene->OnRuntimeStop();
		Input::Reset();
	}
}

TEST_SUITE("GameUI.Events")
{
	TEST_CASE("A button clicks on release over the button it was pressed on, and focuses on the press")
	{
		UIInputScene scene;
		Entity button = scene.AddButton(scene.Canvas, "Button", { 100.0f, 100.0f }, { 200.0f, 100.0f });
		Entity receiver = scene.Target.CreateEntity("Receiver");
		button.GetComponent<UIButtonComponent>().EventTarget = receiver.GetUUID();
		scene.Play();

		scene.MoveTo({ 150.0f, 150.0f });
		scene.Press();
		CHECK(scene.Focused() == "Button");
		CHECK(scene.State(button) == UIVisualState::Pressed);
		CHECK(scene.Count(UIEventType::Click) == 0);
		scene.Release();
		REQUIRE(scene.Count(UIEventType::Click) == 1);
		const UIEventInfo& click = scene.Events.back();
		CHECK(click.Widget == button);
		CHECK(click.WidgetID == button.GetUUID());
		CHECK(click.Target == receiver);

		// Pressed, dragged out, released: nothing. Dragged back in before the release: a click.
		scene.Press();
		scene.MoveTo({ 600.0f, 600.0f });
		CHECK(scene.State(button) == UIVisualState::Focused);
		scene.Release();
		CHECK(scene.Count(UIEventType::Click) == 1);
		scene.MoveTo({ 150.0f, 150.0f });
		scene.Press();
		scene.MoveTo({ 900.0f, 150.0f });
		scene.MoveTo({ 160.0f, 160.0f });
		scene.Release();
		CHECK(scene.Count(UIEventType::Click) == 2);

		// A target that is the widget itself is no target. A press with another button does nothing.
		button.GetComponent<UIButtonComponent>().EventTarget = button.GetUUID();
		scene.Click({ 150.0f, 150.0f });
		REQUIRE(scene.Count(UIEventType::Click) == 3);
		CHECK_FALSE(scene.Events.back().Target);
		Input::SimulateMouseButton(Mouse::ButtonRight, true);
		scene.Frame();
		Input::SimulateMouseButton(Mouse::ButtonRight, false);
		scene.Frame();
		CHECK(scene.Count(UIEventType::Click) == 3);
	}

	TEST_CASE("Toggles flip on click, one at a time within a group, and report each change once")
	{
		UIInputScene scene;
		Entity group = scene.Target.CreateEntity("Group");
		Entity first = scene.AddToggle(scene.Canvas, "First", { 100.0f, 100.0f }, group.GetUUID());
		Entity second = scene.AddToggle(scene.Canvas, "Second", { 200.0f, 100.0f }, group.GetUUID());
		Entity loose = scene.AddToggle(scene.Canvas, "Loose", { 300.0f, 100.0f });
		first.GetComponent<UIToggleComponent>().IsOn = true;
		scene.Play();

		scene.Click({ 220.0f, 120.0f });
		CHECK_FALSE(first.GetComponent<UIToggleComponent>().IsOn);
		CHECK(second.GetComponent<UIToggleComponent>().IsOn);
		REQUIRE(scene.Count(UIEventType::ValueChanged) == 2);
		std::vector<UIEventInfo> changes;
		std::copy_if(scene.Events.begin(), scene.Events.end(), std::back_inserter(changes), [](const UIEventInfo& event) { return event.Type == UIEventType::ValueChanged; });
		CHECK(changes[0].Widget == first);
		CHECK(changes[0].Value == 0.0f);
		CHECK(changes[1].Widget == second);
		CHECK(changes[1].Value == 1.0f);

		// The one that is on stays on when clicked: a group always has one on.
		scene.Click({ 220.0f, 120.0f });
		CHECK(second.GetComponent<UIToggleComponent>().IsOn);
		CHECK(scene.Count(UIEventType::ValueChanged) == 2);

		// A toggle without a group flips both ways.
		scene.Click({ 320.0f, 120.0f });
		scene.Click({ 320.0f, 120.0f });
		CHECK_FALSE(loose.GetComponent<UIToggleComponent>().IsOn);
		CHECK(scene.Count(UIEventType::ValueChanged) == 4);
		CHECK(scene.Count(UIEventType::Click) == 0); // Toggles raise value changes, not clicks
	}

	TEST_CASE("A slider follows the pointer while dragged, also outside its rect, snapping to whole numbers")
	{
		UIInputScene scene;
		Entity slider = scene.AddSlider(scene.Canvas, "Slider", { 100.0f, 100.0f }, { 200.0f, 20.0f }, UIFillDirection::LeftToRight);
		slider.GetComponent<UISliderComponent>().WholeNumbers = true;
		Entity vertical = scene.AddSlider(scene.Canvas, "Vertical", { 400.0f, 100.0f }, { 20.0f, 200.0f }, UIFillDirection::BottomToTop);
		scene.Play();

		scene.MoveTo({ 151.0f, 110.0f }); // A quarter and a bit: 2.55 -> 3
		scene.Press();
		CHECK(slider.GetComponent<UISliderComponent>().Value == 3.0f);
		scene.MoveTo({ 152.0f, 300.0f }); // Off the slider, still dragging: the same whole number
		CHECK(slider.GetComponent<UISliderComponent>().Value == 3.0f);
		scene.MoveTo({ 1000.0f, 300.0f });
		CHECK(slider.GetComponent<UISliderComponent>().Value == 10.0f);
		scene.MoveTo({ 0.0f, 300.0f });
		CHECK(slider.GetComponent<UISliderComponent>().Value == 0.0f);
		scene.Release();
		CHECK(scene.Count(UIEventType::ValueChanged) == 3); // 3, 10, 0: once per change
		CHECK(scene.Count(UIEventType::Click) == 0);
		scene.MoveTo({ 300.0f, 110.0f });
		CHECK(slider.GetComponent<UISliderComponent>().Value == 0.0f); // Not dragging any more

		// Bottom to top: the bottom edge is the minimum.
		scene.MoveTo({ 410.0f, 250.0f });
		scene.Press();
		scene.Release();
		CHECK(vertical.GetComponent<UISliderComponent>().Value == doctest::Approx(2.5f));
		CHECK(scene.Events.back().Value == doctest::Approx(2.5f));
	}

	TEST_CASE("Disabled widgets neither hover, press, focus nor raise events, and block what is under them")
	{
		UIInputScene scene;
		Entity back = scene.AddButton(scene.Canvas, "Back", { 0.0f, 0.0f }, { 600.0f, 600.0f });
		Entity button = scene.AddButton(scene.Canvas, "Disabled", { 100.0f, 100.0f }, { 200.0f, 100.0f });
		button.GetComponent<UISelectableComponent>().Interactable = false;
		Entity toggle = scene.AddToggle(scene.Canvas, "Disabled Toggle", { 400.0f, 100.0f });
		toggle.GetComponent<UISelectableComponent>().Interactable = false;
		scene.Play();

		scene.MoveTo({ 150.0f, 150.0f });
		CHECK_FALSE(scene.System().GetHovered());
		CHECK(scene.System().IsPointerOverUI());
		scene.Press();
		CHECK(scene.State(button) == UIVisualState::Disabled);
		scene.Release();
		scene.Click({ 410.0f, 110.0f });
		CHECK_FALSE(toggle.GetComponent<UIToggleComponent>().IsOn);
		CHECK(scene.Events.empty());
		CHECK(scene.Focused().empty());
		CHECK_FALSE(scene.System().SetFocus(button));
		CHECK(scene.State(back) == UIVisualState::Normal);
	}

	TEST_CASE("Listeners removed during a dispatch are not called again, and ones added receive the next events")
	{
		UIInputScene scene;
		scene.AddButton(scene.Canvas, "Button", { 100.0f, 100.0f }, { 200.0f, 100.0f });
		scene.Play();
		IUIEvents& events = *scene.Target.GetService<IUIEvents>();
		int firstCalls = 0;
		int secondCalls = 0;
		int addedCalls = 0;
		UIEventListenerID second = 0;
		const UIEventListenerID first = events.AddUIEventListener([&](const UIEventInfo& event)
		{
			if (event.Type != UIEventType::Click)
				return;
			firstCalls++;
			events.RemoveUIEventListener(second);
			events.AddUIEventListener([&](const UIEventInfo& added) { addedCalls += added.Type == UIEventType::Click ? 1 : 0; });
		});
		second = events.AddUIEventListener([&](const UIEventInfo& event) { secondCalls += event.Type == UIEventType::Click ? 1 : 0; });
		CHECK(events.AddUIEventListener(UIEventCallback()) == 0);

		scene.Click({ 150.0f, 150.0f });
		CHECK(firstCalls == 1);
		CHECK(secondCalls == 0);
		CHECK(addedCalls == 0);
		CHECK(events.RemoveUIEventListener(first));
		CHECK_FALSE(events.RemoveUIEventListener(first));
		scene.Click({ 150.0f, 150.0f });
		CHECK(firstCalls == 1);
		CHECK(addedCalls == 1);
	}

	TEST_CASE("Tints fade over unscaled time, also at time scale 0, and only fading selectables keep a state")
	{
		UIInputScene scene;
		Entity button = scene.AddButton(scene.Canvas, "Button", { 100.0f, 100.0f }, { 200.0f, 100.0f });
		UISelectableComponent& selectable = button.GetComponent<UISelectableComponent>();
		selectable.NormalColor = glm::vec4(1.0f);
		selectable.HoverColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
		selectable.FadeDuration = 0.25f;
		scene.Play();
		scene.Target.SetTimeScale(0.0f);
		GameUIScene& ui = GameUIScene::Get(scene.Target);

		Input::SimulateMouseMove({ 150.0f, 150.0f });
		scene.Frame(0.125f); // The fade starts at the normal color
		CHECK(scene.State(button) == UIVisualState::Hover);
		CHECK(ui.GetTintColor(button.GetHandle(), selectable, UIVisualState::Hover).x == doctest::Approx(1.0f));
		scene.Frame(0.125f);
		CHECK(ui.GetTintColor(button.GetHandle(), selectable, UIVisualState::Hover).x == doctest::Approx(0.75f));
		scene.Frame(0.125f);
		CHECK(ui.GetTintColor(button.GetHandle(), selectable, UIVisualState::Hover).x == doctest::Approx(0.5f));

		// Away: back to normal from where it is, then no state is kept.
		Input::SimulateMouseMove({ 900.0f, 900.0f });
		scene.Frame(0.125f);
		scene.Frame(0.125f);
		CHECK(scene.State(button) == UIVisualState::Normal);
		CHECK(ui.GetTintColor(button.GetHandle(), selectable, UIVisualState::Normal).x == doctest::Approx(0.75f));
		scene.Frame(0.125f);
		CHECK(ui.GetTintColor(button.GetHandle(), selectable, UIVisualState::Normal).x == doctest::Approx(1.0f));
		CHECK(scene.Target.GetUnscaledTime() == doctest::Approx(0.75));
		CHECK(scene.Target.GetTime() == doctest::Approx(0.0));
	}
}

TEST_SUITE("GameUI.Navigation")
{
	TEST_CASE("Arrow keys move the focus through a 3x3 grid, starting at the top-left selectable")
	{
		UIInputScene scene;
		AddGrid(scene);
		scene.Play();
		std::vector<std::string> sequence;
		for (const KeyCode key : { Key::Down, Key::Right, Key::Right, Key::Right, Key::Down, Key::Down, Key::Left, Key::Up, Key::Up, Key::Left, Key::Up })
		{
			scene.Key(key);
			sequence.push_back(scene.Focused());
		}
		CHECK(sequence == std::vector<std::string> { "B00", "B01", "B02", "B02", "B12", "B22", "B21", "B11", "B01", "B00", "B00" });
		CHECK(scene.Count(UIEventType::FocusChanged) == 2 * 8 + 1); // Eight moves (a loss and a gain each) after the first gain
	}

	TEST_CASE("On an irregular menu the focus goes to the nearest selectable ahead, by along + 2 x across")
	{
		UIInputScene scene;
		scene.AddButton(scene.Canvas, "Title", { 700.0f, 100.0f }, { 500.0f, 100.0f });    // Center (950, 150)
		scene.AddButton(scene.Canvas, "Play", { 600.0f, 300.0f }, { 200.0f, 80.0f });      // (700, 340)
		scene.AddButton(scene.Canvas, "Settings", { 1000.0f, 320.0f }, { 300.0f, 80.0f }); // (1150, 360)
		scene.AddButton(scene.Canvas, "Quit", { 900.0f, 500.0f }, { 100.0f, 60.0f });      // (950, 530)
		scene.Play();
		std::vector<std::string> sequence;
		for (const KeyCode key : { Key::Down, Key::Down, Key::Left, Key::Right, Key::Down, Key::Up, Key::Right, Key::Left, Key::Left })
		{
			scene.Key(key);
			sequence.push_back(scene.Focused());
		}
		CHECK(sequence == std::vector<std::string> { "Title", "Quit", "Play", "Settings", "Quit", "Title", "Settings", "Play", "Play" });
	}

	TEST_CASE("Explicit neighbours, Navigation None and disabled selectables")
	{
		UIInputScene scene;
		std::vector<Entity> grid = AddGrid(scene);
		Entity& b00 = grid[0];
		UISelectableComponent& explicitNavigation = b00.GetComponent<UISelectableComponent>();
		explicitNavigation.Navigation = UINavigation::Explicit;
		explicitNavigation.NavigateRight = grid[2].GetUUID(); // Skips B01
		explicitNavigation.NavigateDown = grid[4].GetUUID();  // B11: disabled below, so Down stays
		grid[4].GetComponent<UISelectableComponent>().Interactable = false;
		grid[7].GetComponent<UISelectableComponent>().Navigation = UINavigation::None; // B21: never navigated to
		scene.Play();

		REQUIRE(scene.System().SetFocus(b00));
		scene.Key(Key::Down);
		CHECK(scene.Focused() == "B00");
		scene.Key(Key::Right);
		CHECK(scene.Focused() == "B02");
		scene.Key(Key::Left);
		CHECK(scene.Focused() == "B01");
		scene.Key(Key::Down); // B11 is disabled, B21 not navigable: the nearest ahead is B10 or B12, B10 drawn first
		CHECK(scene.Focused() == "B10");
		scene.Key(Key::Right); // Over the disabled B11 and B12 is next
		CHECK(scene.Focused() == "B12");

		// A selectable with Navigation None keeps the focus a click gave it.
		scene.Click({ 360.0f, 420.0f });
		CHECK(scene.Focused() == "B21");
		scene.Key(Key::Up);
		CHECK(scene.Focused() == "B21");
	}

	TEST_CASE("Navigation starts at the topmost input canvas's FirstSelected, else its top-left selectable")
	{
		UIInputScene scene;
		std::vector<Entity> grid = AddGrid(scene);
		Entity popup = scene.AddCanvas("Popup", 2);
		scene.AddButton(popup, "OK", { 1000.0f, 600.0f }, { 100.0f, 50.0f });
		scene.AddButton(popup, "Cancel", { 1200.0f, 600.0f }, { 100.0f, 50.0f });
		Entity cover = scene.AddCanvas("Cover", 9);
		cover.GetComponent<UICanvasComponent>().ReceivesInput = false;
		scene.AddButton(cover, "Unreachable", { 0.0f, 0.0f }, { 50.0f, 50.0f });
		scene.Play();

		scene.Key(Key::Enter); // Submit without a focus starts it, clicking nothing
		CHECK(scene.Focused() == "OK");
		CHECK(scene.Count(UIEventType::Click) == 0);

		REQUIRE(scene.System().SetFocus(Entity()));
		popup.SetActive(false);
		scene.Canvas.GetComponent<UICanvasComponent>().FirstSelected = grid[8].GetUUID();
		scene.Canvas.MarkModified<UICanvasComponent>();
		scene.Key(Key::Left);
		CHECK(scene.Focused() == "B22");
	}

	TEST_CASE("A held key repeats after 0.4 s and then every 0.1 s, the same in every run")
	{
		const auto run = []()
		{
			UIInputScene scene;
			for (int index = 0; index < 12; index++)
				scene.AddButton(scene.Canvas, "R" + std::to_string(index), { 50.0f + 150.0f * index, 100.0f }, { 100.0f, 50.0f });
			scene.Play();
			REQUIRE(scene.System().SetFocus(scene.Target.FindEntityByName("R0")));
			std::vector<int> moves;
			Input::SimulateKey(Key::Right, true);
			std::string previous = "R0";
			// Frames of 1/32 s: the key is pressed in frame 0 and held through frame 31 (0.97 s).
			for (int frame = 0; frame < 32; frame++)
			{
				scene.Frame(1.0f / 32.0f);
				if (scene.Focused() != previous)
					moves.push_back(frame);
				previous = scene.Focused();
			}
			Input::SimulateKey(Key::Right, false);
			scene.Frame(1.0f / 32.0f);
			return std::make_pair(moves, scene.Focused());
		};
		const auto [moves, focused] = run();
		// Held times 0.40625, 0.5, 0.625, 0.71875, 0.8125 and 0.90625 s are the first at or past 0.4, 0.5, ... 0.9 s.
		CHECK(moves == std::vector<int> { 0, 13, 16, 20, 23, 26, 29 });
		CHECK(focused == "R7");
		CHECK(run() == std::make_pair(moves, focused));
	}

	TEST_CASE("The d-pad and the left stick navigate, the stick with hysteresis")
	{
		UIInputScene scene;
		for (int index = 0; index < 6; index++)
			scene.AddButton(scene.Canvas, "R" + std::to_string(index), { 50.0f + 150.0f * index, 100.0f }, { 100.0f, 50.0f });
		scene.Play();
		REQUIRE(scene.System().SetFocus(scene.Target.FindEntityByName("R0")));
		Input::SimulateGamepadButton(0, GamepadButton::DPadRight, true);
		scene.Frame();
		Input::SimulateGamepadButton(0, GamepadButton::DPadRight, false);
		scene.Frame();
		CHECK(scene.Focused() == "R1");

		for (const float value : { 0.6f, 0.4f, 0.45f, 0.3f, 0.55f, 0.2f })
		{
			Input::SimulateGamepadAxis(2, GamepadAxis::LeftX, value);
			scene.Frame();
		}
		CHECK(scene.Focused() == "R3"); // Two pushes: 0.4 and 0.45 still hold the first
		Input::SimulateGamepadAxis(2, GamepadAxis::LeftX, -0.8f);
		scene.Frame();
		CHECK(scene.Focused() == "R2");
	}

	TEST_CASE("A focused slider steps along its direction; across it, the focus moves on")
	{
		UIInputScene scene;
		scene.AddButton(scene.Canvas, "Above", { 100.0f, 0.0f }, { 200.0f, 50.0f });
		Entity slider = scene.AddSlider(scene.Canvas, "Slider", { 100.0f, 100.0f }, { 200.0f, 20.0f }, UIFillDirection::LeftToRight);
		slider.GetComponent<UISliderComponent>().KeyboardStep = 1.5f;
		Entity reversed = scene.AddSlider(scene.Canvas, "Reversed", { 400.0f, 100.0f }, { 200.0f, 20.0f }, UIFillDirection::RightToLeft);
		reversed.GetComponent<UISliderComponent>().WholeNumbers = true; // Step: a tenth of the range, at least 1
		Entity vertical = scene.AddSlider(scene.Canvas, "Vertical", { 700.0f, 100.0f }, { 20.0f, 200.0f }, UIFillDirection::BottomToTop);
		scene.Play();

		REQUIRE(scene.System().SetFocus(slider));
		scene.Key(Key::Right);
		CHECK(slider.GetComponent<UISliderComponent>().Value == doctest::Approx(5.5f));
		for (int step = 0; step < 4; step++)
			scene.Key(Key::Left);
		CHECK(slider.GetComponent<UISliderComponent>().Value == doctest::Approx(0.0f)); // 4, 2.5, 1, then clamped
		CHECK(scene.Count(UIEventType::ValueChanged) == 5);
		scene.Key(Key::Left);
		CHECK(scene.Count(UIEventType::ValueChanged) == 5); // No change, no event

		REQUIRE(scene.System().SetFocus(reversed));
		scene.Key(Key::Left); // Toward its end
		CHECK(reversed.GetComponent<UISliderComponent>().Value == 5.0f);
		REQUIRE(scene.System().SetFocus(vertical));
		scene.Key(Key::Up);
		CHECK(vertical.GetComponent<UISliderComponent>().Value == doctest::Approx(5.0f));
		scene.Key(Key::Left); // Across a vertical slider: navigation
		CHECK(scene.Focused() == "Reversed");

		REQUIRE(scene.System().SetFocus(slider));
		scene.Key(Key::Up);
		CHECK(scene.Focused() == "Above");
	}

	TEST_CASE("Submit clicks the focused button and flips the focused toggle; Cancel raises nothing")
	{
		UIInputScene scene;
		scene.AddButton(scene.Canvas, "Button", { 100.0f, 100.0f }, { 200.0f, 100.0f });
		Entity toggle = scene.AddToggle(scene.Canvas, "Toggle", { 100.0f, 300.0f });
		scene.Play();
		REQUIRE(scene.System().SetFocus(scene.Target.FindEntityByName("Button")));
		scene.Frame();
		scene.Events.clear();
		scene.Key(Key::Enter);
		CHECK(scene.Count(UIEventType::Click) == 1);
		REQUIRE(scene.System().SetFocus(toggle));
		Input::SimulateGamepadButton(0, GamepadButton::A, true);
		scene.Frame();
		Input::SimulateGamepadButton(0, GamepadButton::A, false);
		scene.Frame();
		CHECK(toggle.GetComponent<UIToggleComponent>().IsOn);
		const size_t count = scene.Events.size();
		Input::SimulateKey(Key::Escape, true);
		scene.Frame();
		CHECK(scene.System().GetActions().IsPressed(UIAction::Cancel)); // For scripts to read
		Input::SimulateKey(Key::Escape, false);
		scene.Frame();
		CHECK(scene.Events.size() == count);
		CHECK(scene.Focused() == "Toggle");
	}
}

namespace
{

	// An input field "<name>" at (100, 100), 300 x 40: an image, a selectable, the field, its text element "<name> Text"
	// (left-aligned, 20 units per em, inset by 8) and its placeholder "<name> Hint".
	Entity AddField(UIInputScene& scene, const std::string& name, uint32_t limit = 0)
	{
		Entity field = scene.AddImage(scene.Canvas, name, { 100.0f, 100.0f }, { 300.0f, 40.0f });
		field.AddComponent<UISelectableComponent>();
		Entity text = scene.Target.CreateChildEntity(field, name + " Text");
		UIRectComponent rect;
		rect.AnchorMin = glm::vec2(0.0f);
		rect.AnchorMax = glm::vec2(1.0f);
		rect.Size = glm::vec2(-16.0f);
		text.AddComponent<UIRectComponent>(rect);
		UITextComponent& label = text.AddComponent<UITextComponent>();
		label.Alignment = TextAlignment::Left;
		label.FontSize = 20.0f;
		label.Text = "ignored: the field's text shows";
		Entity hint = scene.Target.CreateChildEntity(field, name + " Hint");
		hint.AddComponent<UIRectComponent>(rect);
		hint.AddComponent<UITextComponent>().Text = "Your name";
		UIInputFieldComponent& component = field.AddComponent<UIInputFieldComponent>();
		component.TextElement = text.GetUUID();
		component.Placeholder = hint.GetUUID();
		component.CharacterLimit = limit;
		return field;
	}

	std::string FieldText(Entity field)
	{
		return field.GetComponent<UIInputFieldComponent>().Text;
	}

	void Type(UIInputScene& scene, std::string_view text)
	{
		Input::SimulateText(text);
		scene.Frame();
	}

	// A key pressed and released with modifiers held around it.
	void KeyWith(UIInputScene& scene, KeyCode modifier, KeyCode key)
	{
		Input::SimulateKey(modifier, true);
		scene.Frame();
		scene.Key(key);
		Input::SimulateKey(modifier, false);
		scene.Frame();
	}

}

TEST_SUITE("GameUI.InputField")
{
	TEST_CASE("A focused field takes typed text at its caret and keeps the keyboard from the game")
	{
		UIInputScene scene;
		Entity field = AddField(scene, "Name");
		scene.Play();
		scene.Frame();
		CHECK_FALSE(Input::IsKeyboardCaptured());
		scene.Click({ 200.0f, 120.0f });
		CHECK(scene.Focused() == "Name");
		CHECK(Input::IsKeyboardCaptured());

		Input::SimulateText("Hi there");
		Input::SimulateKey(Key::W, true);
		scene.Frame();
		CHECK(FieldText(field) == "Hi there");
		CHECK_FALSE(Input::IsKeyDown(Key::W)); // The game does not walk while a name is typed
		CHECK(Input::GetTypedText().empty());
		Input::SimulateKey(Key::W, false);
		// Multi-byte characters go in whole.
		Type(scene, "\xC3\xA9!");
		CHECK(FieldText(field) == "Hi there\xC3\xA9!");
		CHECK(scene.Count(UIEventType::Submit) == 0);

		// Losing the focus gives the keyboard back.
		CHECK(scene.System().SetFocus(Entity()));
		scene.Frame();
		CHECK_FALSE(Input::IsKeyboardCaptured());
		Type(scene, "x");
		CHECK(FieldText(field) == "Hi there\xC3\xA9!");
		std::string error;
		CHECK_MESSAGE(GameUIScene::Get(scene.Target).Validate(&error), error);
	}

	TEST_CASE("Backspace, Delete, the arrows, Home and End edit at the caret; Shift selects and typing replaces the selection")
	{
		UIInputScene scene;
		Entity field = AddField(scene, "Name");
		field.GetComponent<UIInputFieldComponent>().Text = "abc\xC3\xA9";
		scene.Play();
		scene.Frame();
		REQUIRE(scene.System().SetFocus(field)); // The caret goes to the end
		scene.Frame();
		scene.Key(Key::Backspace); // A two-byte character goes whole
		CHECK(FieldText(field) == "abc");
		scene.Key(Key::Left);
		scene.Key(Key::Left);
		Type(scene, "X");
		CHECK(FieldText(field) == "aXbc");
		scene.Key(Key::Delete);
		CHECK(FieldText(field) == "aXc");
		scene.Key(Key::Home);
		Type(scene, "<");
		scene.Key(Key::End);
		Type(scene, ">");
		CHECK(FieldText(field) == "<aXc>");
		// Shift+Left twice selects "c>", which typing replaces.
		KeyWith(scene, Key::LeftShift, Key::Left);
		KeyWith(scene, Key::LeftShift, Key::Left);
		Type(scene, "Z");
		CHECK(FieldText(field) == "<aXZ");
		// Ctrl+A selects everything; Backspace clears it, and the placeholder shows again.
		KeyWith(scene, Key::LeftControl, Key::A);
		scene.Key(Key::Backspace);
		CHECK(FieldText(field).empty());
		std::string error;
		CHECK_MESSAGE(GameUIScene::Get(scene.Target).Validate(&error), error);
	}

	TEST_CASE("The character limit cuts typed and set text, and line breaks never enter a field")
	{
		UIInputScene scene;
		Entity field = AddField(scene, "Code", 4);
		scene.Play();
		scene.Frame();
		REQUIRE(scene.System().SetFocus(field));
		scene.Frame();
		Type(scene, "ab\ncd\tef");
		CHECK(FieldText(field) == "abcd");
		Type(scene, "g");
		CHECK(FieldText(field) == "abcd");
		std::string error;
		CHECK(scene.System().SetText(field, "\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC", &error)); // Five euro signs
		CHECK(FieldText(field) == "\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC");
		std::string read;
		CHECK(scene.System().GetText(field, read));
		CHECK(read == FieldText(field));
		// The caret follows text set by code: typing after it goes at a character boundary.
		scene.Key(Key::Backspace);
		CHECK(FieldText(field) == "\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC");
	}

	TEST_CASE("Enter submits, Escape and a gamepad's Cancel stop editing; a held key repeats like the UI actions")
	{
		UIInputScene scene;
		Entity field = AddField(scene, "Chat");
		field.GetComponent<UIInputFieldComponent>().Text = "0123456789";
		scene.Play();
		scene.Frame();
		REQUIRE(scene.System().SetFocus(field));
		scene.Frame();

		// Backspace held: once on the press, then after 0.4 s and every 0.1 s, with fixed steps of 0.1 s.
		Input::SimulateKey(Key::Backspace, true);
		scene.Frame(0.1f);
		CHECK(FieldText(field) == "012345678");
		for (int step = 0; step < 5; step++) // 0.5 s held: repeats at 0.4 and 0.5
			scene.Frame(0.1f);
		CHECK(FieldText(field) == "0123456");
		Input::SimulateKey(Key::Backspace, false);
		scene.Frame(0.1f);

		scene.Key(Key::Enter);
		REQUIRE(scene.Count(UIEventType::Submit) == 1);
		CHECK(std::find_if(scene.Events.begin(), scene.Events.end(), [](const UIEventInfo& event) { return event.Type == UIEventType::Submit; })->Widget == field);
		CHECK(scene.Focused() == "Chat"); // Submitting keeps editing
		scene.Key(Key::Space);
		CHECK(FieldText(field) == "0123456"); // Space is no Submit while the field has the keyboard: only typed text goes in
		CHECK(scene.Count(UIEventType::Click) == 0);

		scene.Key(Key::Escape);
		CHECK(scene.Focused().empty());
		CHECK_FALSE(Input::IsKeyboardCaptured());

		// A gamepad: Submit focuses and submits, Cancel stops editing.
		REQUIRE(scene.System().SetFocus(field));
		scene.Frame();
		Input::SimulateGamepadButton(0, GamepadButton::A, true);
		scene.Frame();
		Input::SimulateGamepadButton(0, GamepadButton::A, false);
		scene.Frame();
		CHECK(scene.Count(UIEventType::Submit) == 2);
		Input::SimulateGamepadButton(0, GamepadButton::B, true);
		scene.Frame();
		Input::SimulateGamepadButton(0, GamepadButton::B, false);
		scene.Frame();
		CHECK(scene.Focused().empty());
	}

	TEST_CASE("A click puts the caret at the nearest character; the caret blinks; the placeholder shows only while the text is empty")
	{
		UIInputScene scene;
		Entity field = AddField(scene, "Name");
		const Entity hint = scene.Target.FindEntityByName("Name Hint");
		GameUIScene& ui = GameUIScene::Get(scene.Target);
		scene.Play();
		scene.Frame();
		UICanvasView view;
		uint32_t hintElement = 0;
		REQUIRE(ui.FindElement(hint.GetHandle(), GameUIScene::c_DefaultScreen, view, hintElement));
		CHECK(ui.IsElementVisible(view, hintElement));

		field.GetComponent<UIInputFieldComponent>().Text = "abcdef";
		field.MarkModified<UIInputFieldComponent>();
		scene.Frame();
		REQUIRE(ui.FindElement(hint.GetHandle(), GameUIScene::c_DefaultScreen, view, hintElement));
		CHECK_FALSE(ui.IsElementVisible(view, hintElement));

		// The text starts at x = 108 (the field's left edge inset by 8): a click left of it puts the caret first, one far right last.
		scene.Click({ 101.0f, 120.0f });
		Type(scene, "<");
		scene.Click({ 395.0f, 120.0f });
		Type(scene, ">");
		CHECK(FieldText(field) == "<abcdef>");
		const std::optional<uint32_t> middle = ui.FindInputFieldCaret(field.GetHandle(), GameUIScene::c_DefaultScreen, { 108.0f + 3.0f * 9.0f, 120.0f });
		REQUIRE(middle.has_value());
		CHECK(*middle > 0);
		CHECK(*middle < 8);

		// The caret's quad comes and goes every 0.53 s of unscaled time (also at time scale 0).
		scene.Target.SetTimeScale(0.0f);
		auto caretShown = [&]()
		{
			uint32_t textElement = 0;
			REQUIRE(ui.FindElement(scene.Target.FindEntityByName("Name Text").GetHandle(), GameUIScene::c_DefaultScreen, view, textElement));
			// The caret is the one solid quad narrower than 3 units (1.2 at 20 units per em).
			const std::vector<RenderData::UIQuad>& quads = ui.EnsureQuads(view);
			return std::count_if(quads.begin(), quads.end(), [](const RenderData::UIQuad& quad)
				{ return (quad.Flags & RenderData::c_UIQuadKindMask) == RenderData::c_UIQuadImage && quad.Rect.z - quad.Rect.x < 3.0f; }) == 1;
		};
		CHECK(caretShown());
		scene.Frame(0.6f);
		CHECK_FALSE(caretShown());
		scene.Frame(0.5f);
		CHECK(caretShown());
		std::string error;
		CHECK_MESSAGE(ui.Validate(&error), error);
	}
}

namespace
{

	// A world-space canvas "Sign" at the origin facing +Z, 400 x 200 units of 1 cm (4 x 2 m, its center at the origin), seen
	// by a primary camera 5 m in front of it: the canvas's center is at the screen's.
	Entity AddWorldCanvas(UIInputScene& scene)
	{
		Entity camera = scene.Target.CreateEntity("Camera");
		camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 5.0f };
		camera.AddComponent<CameraComponent>();
		Entity sign = scene.Target.CreateEntity("Sign");
		sign.AddComponent<UIRectComponent>();
		UICanvasComponent& canvas = sign.AddComponent<UICanvasComponent>();
		canvas.RenderMode = UICanvasRenderMode::WorldSpace;
		canvas.ReferenceResolution = { 400.0f, 200.0f };
		canvas.WorldUnitsPerUnit = 0.01f;
		return sign;
	}

}

TEST_SUITE("GameUI.WorldSpace")
{
	TEST_CASE("A world-space canvas is laid out at its reference size on every screen, on a plane through its entity")
	{
		UIInputScene scene;
		Entity sign = AddWorldCanvas(scene);
		sign.GetComponent<TransformComponent>().Translation = { 1.0f, 2.0f, -3.0f };
		sign.MarkModified<TransformComponent>();
		GameUIScene& ui = GameUIScene::Get(scene.Target);
		for (const UIScreen& screen : { UIScreen { { 1920.0f, 1080.0f }, 1.0f }, UIScreen { { 800.0f, 1200.0f }, 2.0f } })
		{
			const std::span<const UICanvasView> views = ui.EnsureLayout(screen);
			const auto world = std::find_if(views.begin(), views.end(), [](const UICanvasView& view) { return view.WorldSpace; });
			REQUIRE(world != views.end());
			CHECK(world->UnitSize == glm::vec2(400.0f, 200.0f));
			CHECK(world->Scale == 1.0f);
			CHECK_FALSE(world->PixelSnap);
			// The pivot (the center) at the entity's origin, +X right, canvas +Y down becoming -Y, 1 cm per unit.
			const glm::mat4 matrix = ui.GetCanvasWorldMatrix(*world);
			auto at = [&](const glm::vec2& point) { return glm::vec3(matrix * glm::vec4(point, 0.0f, 1.0f)); };
			CHECK(glm::length(at({ 200.0f, 100.0f }) - glm::vec3(1.0f, 2.0f, -3.0f)) < 1e-4f);
			CHECK(glm::length(at({ 400.0f, 200.0f }) - glm::vec3(3.0f, 1.0f, -3.0f)) < 1e-4f);
			CHECK(glm::length(at({ 0.0f, 0.0f }) - glm::vec3(-1.0f, 3.0f, -3.0f)) < 1e-4f);
		}
		// Hidden screen-space UI leaves it in the views, and only it (the scene's other canvas is screen-space).
		CHECK(ui.GetWorldCanvasCount() == 1);
		CHECK(ui.EnsureLayout(GameUIScene::c_DefaultScreen).size() == 2);
		const std::span<const UICanvasView> worldOnly = ui.EnsureLayout(GameUIScene::c_DefaultScreen, false);
		REQUIRE(worldOnly.size() == 1);
		CHECK(worldOnly[0].WorldSpace);
	}

	TEST_CASE("The pointer's ray finds the point of a world-space canvas a pixel shows, and the pixel a point shows at")
	{
		UIInputScene scene;
		AddWorldCanvas(scene);
		GameUIScene& ui = GameUIScene::Get(scene.Target);
		const UIScreen screen = GameUIScene::c_DefaultScreen;
		const std::optional<glm::mat4> viewProjection = GameUIPointer::GetGameViewProjection(scene.Target, screen);
		REQUIRE(viewProjection.has_value());
		const UICanvasView world = *std::find_if(ui.EnsureLayout(screen).begin(), ui.EnsureLayout(screen).end(), [](const UICanvasView& view) { return view.WorldSpace; });
		const glm::mat4 matrix = ui.GetCanvasWorldMatrix(world);
		for (const glm::vec2 point : { glm::vec2(200.0f, 100.0f), glm::vec2(10.0f, 190.0f), glm::vec2(399.0f, 1.0f) })
		{
			CAPTURE(point);
			const std::optional<glm::vec2> pixel = GameUIPointer::ProjectCanvasPoint(*viewProjection, matrix, screen.Size, point);
			REQUIRE(pixel.has_value());
			const std::optional<UIPointerRay> ray = GameUIPointer::MakeRay(*viewProjection, screen.Size, *pixel);
			REQUIRE(ray.has_value());
			const std::optional<std::pair<glm::vec2, float>> crossing = GameUIPointer::IntersectCanvas(matrix, *ray);
			REQUIRE(crossing.has_value());
			CHECK(glm::length(crossing->first - point) < 0.05f);
			CHECK(crossing->second > 4.0f);
		}
		// The center of the canvas is at the center of the screen; a ray pointing away crosses nothing.
		const std::optional<glm::vec2> center = GameUIPointer::ProjectCanvasPoint(*viewProjection, matrix, screen.Size, { 200.0f, 100.0f });
		REQUIRE(center.has_value());
		CHECK(glm::length(*center - glm::vec2(960.0f, 540.0f)) < 0.5f);
		CHECK_FALSE(GameUIPointer::IntersectCanvas(matrix, UIPointerRay { glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, 1.0f) }).has_value());
	}

	TEST_CASE("Navigation starts on the screen's canvases, and a canvas in the scene navigates within itself")
	{
		UIInputScene scene;
		Entity sign = AddWorldCanvas(scene); // Drawn after the screen's canvas
		scene.AddButton(sign, "Sign A", { 20.0f, 20.0f }, { 100.0f, 50.0f });
		scene.AddButton(sign, "Sign B", { 20.0f, 120.0f }, { 100.0f, 50.0f });
		scene.AddButton(scene.Canvas, "Menu", { 800.0f, 900.0f }, { 200.0f, 80.0f });
		scene.Play();
		scene.Key(Key::Down);
		CHECK(scene.Focused() == "Menu");
		scene.Key(Key::Up); // The sign's buttons are not above it on the screen (their units are no pixels)
		CHECK(scene.Focused() == "Menu");
		REQUIRE(scene.System().SetFocus(scene.Target.FindEntityByName("Sign A")));
		scene.Key(Key::Down);
		CHECK(scene.Focused() == "Sign B");
		scene.Key(Key::Down);
		CHECK(scene.Focused() == "Sign B");
	}

	TEST_CASE("A world-space button is clicked through the game camera, below the screen's canvases; sliders drag along it")
	{
		UIInputScene scene;
		Entity sign = AddWorldCanvas(scene);
		Entity button = scene.AddButton(sign, "Sign Button", { 150.0f, 75.0f }, { 100.0f, 50.0f });
		Entity slider = scene.AddSlider(sign, "Sign Slider", { 0.0f, 150.0f }, { 400.0f, 50.0f }, UIFillDirection::LeftToRight);
		scene.Play();
		scene.Frame();
		scene.Click({ 960.0f, 540.0f });
		REQUIRE(scene.Count(UIEventType::Click) == 1);
		CHECK(scene.Events.back().Widget == button);
		CHECK(scene.System().IsPointerOverUI());

		// A screen-space panel over the center takes the click instead.
		Entity cover = scene.AddImage(scene.Canvas, "Cover", { 900.0f, 500.0f }, { 120.0f, 80.0f });
		scene.Frame();
		scene.Click({ 960.0f, 540.0f });
		CHECK(scene.Count(UIEventType::Click) == 1);
		scene.Target.DestroyEntity(cover);

		// The slider follows the pointer along the canvas: from its left part to beyond its right end.
		GameUIScene& ui = GameUIScene::Get(scene.Target);
		const UICanvasView world = *std::find_if(ui.EnsureLayout(GameUIScene::c_DefaultScreen).begin(), ui.EnsureLayout(GameUIScene::c_DefaultScreen).end(),
			[](const UICanvasView& view) { return view.WorldSpace; });
		const glm::mat4 viewProjection = *GameUIPointer::GetGameViewProjection(scene.Target, GameUIScene::c_DefaultScreen);
		const glm::vec2 left = *GameUIPointer::ProjectCanvasPoint(viewProjection, ui.GetCanvasWorldMatrix(world), { 1920.0f, 1080.0f }, { 2.0f, 175.0f });
		const glm::vec2 beyond = *GameUIPointer::ProjectCanvasPoint(viewProjection, ui.GetCanvasWorldMatrix(world), { 1920.0f, 1080.0f }, { 600.0f, 175.0f });
		scene.MoveTo(left);
		scene.Press();
		CHECK(slider.GetComponent<UISliderComponent>().Value == doctest::Approx(0.05f).epsilon(0.05));
		scene.MoveTo(beyond);
		CHECK(slider.GetComponent<UISliderComponent>().Value == 10.0f);
		scene.Release();
	}
}
