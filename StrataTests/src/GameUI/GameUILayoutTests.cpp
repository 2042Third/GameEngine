#include <doctest/doctest.h>

#include "Perf/SceneGenerators.h"
#include "Scene/SceneTestUtils.h"
#include "Strata/GameUI/GameUIComponents.h"
#include "Strata/GameUI/GameUIHitTest.h"
#include "Strata/GameUI/GameUIScene.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/FontFace.h"
#include "Strata/Renderer/TextLayout.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	constexpr float c_Tolerance = 1e-4f;

	// A scene with one canvas whose unit is one pixel (ConstantPixelSize), so rects are in pixels.
	struct UITestScene
	{
		Ref<Scene> Owner = CreateRef<Scene>("UI");
		Scene& Target = *Owner;
		Entity Canvas;

		explicit UITestScene(UICanvasScaleMode mode = UICanvasScaleMode::ConstantPixelSize)
		{
			Canvas = Target.CreateEntity("Canvas");
			Canvas.AddComponent<UIRectComponent>();
			Canvas.AddComponent<UICanvasComponent>().ScaleMode = mode;
		}

		Entity Add(Entity parent, const std::string& name, const UIRectComponent& rect)
		{
			Entity entity = Target.CreateChildEntity(parent, name);
			entity.AddComponent<UIRectComponent>(rect);
			return entity;
		}

		GameUIScene& GetUI() { return GameUIScene::Get(Target); }

		UIBox Box(Entity entity, const UIScreen& screen = {})
		{
			GameUIScene& ui = GetUI();
			ui.EnsureLayout(screen);
			UICanvasView view;
			uint32_t element = 0;
			REQUIRE(ui.FindElement(entity.GetHandle(), screen, view, element));
			return ui.GetElementBox(view, element);
		}

		bool Visible(Entity entity, const UIScreen& screen = {})
		{
			GameUIScene& ui = GetUI();
			ui.EnsureLayout(screen);
			UICanvasView view;
			uint32_t element = 0;
			REQUIRE(ui.FindElement(entity.GetHandle(), screen, view, element));
			return ui.IsElementVisible(view, element);
		}
	};

	UIRectComponent PointRect(const glm::vec2& anchor, const glm::vec2& pivot, const glm::vec2& position, const glm::vec2& size)
	{
		UIRectComponent rect;
		rect.AnchorMin = anchor;
		rect.AnchorMax = anchor;
		rect.Pivot = pivot;
		rect.Position = position;
		rect.Size = size;
		return rect;
	}

	UIRectComponent StretchRect(const glm::vec2& anchorMin, const glm::vec2& anchorMax, const glm::vec2& size = glm::vec2(0.0f))
	{
		UIRectComponent rect;
		rect.AnchorMin = anchorMin;
		rect.AnchorMax = anchorMax;
		rect.Size = size;
		return rect;
	}

	void CheckBox(const UIBox& box, const glm::vec2& min, const glm::vec2& size)
	{
		CHECK(box.Min.x == doctest::Approx(min.x).epsilon(c_Tolerance));
		CHECK(box.Min.y == doctest::Approx(min.y).epsilon(c_Tolerance));
		CHECK(box.Size.x == doctest::Approx(size.x).epsilon(c_Tolerance));
		CHECK(box.Size.y == doctest::Approx(size.y).epsilon(c_Tolerance));
	}

	float ShapedWidth(std::string_view text)
	{
		ShapedText shaped;
		ShapeText(*Font::GetDefault()->GetFace(), text, {}, shaped);
		return shaped.Width;
	}

	// Lays out and builds the quads of every canvas for the screen, as a frame of the UI pass does.
	void Frame(GameUIScene& ui, const UIScreen& screen = {})
	{
		for (const UICanvasView& view : ui.EnsureLayout(screen))
			ui.EnsureQuads(view);
	}

}

TEST_SUITE("GameUI.Layout")
{
	TEST_CASE("Rects follow their anchors, pivots, positions and sizes exactly on every screen")
	{
		for (const glm::vec2 screenSize : { glm::vec2(1920.0f, 1080.0f), glm::vec2(1280.0f, 720.0f), glm::vec2(2560.0f, 1440.0f), glm::vec2(1080.0f, 1920.0f) })
		{
			CAPTURE(screenSize);
			const UIScreen screen { screenSize, 1.0f };
			const float width = screenSize.x;
			const float height = screenSize.y;
			UITestScene ui;
			Entity point = ui.Add(ui.Canvas, "Point", PointRect({ 0.5f, 0.5f }, { 0.5f, 0.5f }, { 10.0f, -20.0f }, { 200.0f, 50.0f }));
			Entity corner = ui.Add(ui.Canvas, "Corner", PointRect({ 1.0f, 0.0f }, { 1.0f, 0.0f }, { -16.0f, 16.0f }, { 300.0f, 100.0f }));
			Entity inset = ui.Add(ui.Canvas, "Inset", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }, { -20.0f, -20.0f }));
			UIRectComponent bar = StretchRect({ 0.0f, 1.0f }, { 1.0f, 1.0f }, { -40.0f, 60.0f });
			bar.Pivot = { 0.5f, 1.0f };
			bar.Position = { 0.0f, -8.0f };
			Entity bottomBar = ui.Add(ui.Canvas, "Bottom Bar", bar);
			Entity nested = ui.Add(inset, "Nested", PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 5.0f, 5.0f }, { 50.0f, 50.0f }));
			Entity negative = ui.Add(ui.Canvas, "Negative", PointRect({ 0.25f, 0.75f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { -10.0f, 30.0f }));

			CheckBox(ui.Box(point, screen), { width * 0.5f + 10.0f - 100.0f, height * 0.5f - 20.0f - 25.0f }, { 200.0f, 50.0f });
			CheckBox(ui.Box(corner, screen), { width - 16.0f - 300.0f, 16.0f }, { 300.0f, 100.0f });
			CheckBox(ui.Box(inset, screen), { 10.0f, 10.0f }, { width - 20.0f, height - 20.0f });
			CheckBox(ui.Box(bottomBar, screen), { 20.0f, height - 8.0f - 60.0f }, { width - 40.0f, 60.0f });
			CheckBox(ui.Box(nested, screen), { 15.0f, 15.0f }, { 50.0f, 50.0f });
			// Negative sizes clamp to 0 around the pivot point.
			CheckBox(ui.Box(negative, screen), { width * 0.25f, height * 0.75f - 15.0f }, { 0.0f, 30.0f });
			// The canvas itself is the screen.
			CheckBox(ui.Box(ui.Canvas, screen), { 0.0f, 0.0f }, { width, height });
		}
	}

	TEST_CASE("The safe area insets the canvas's rect and everything anchored in it")
	{
		UITestScene ui;
		ui.Canvas.GetComponent<UICanvasComponent>().SafeAreaPadding = { 10.0f, 20.0f, 30.0f, 40.0f };
		Entity fill = ui.Add(ui.Canvas, "Fill", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }));
		Entity topRight = ui.Add(ui.Canvas, "Top Right", PointRect({ 1.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f }, { 50.0f, 50.0f }));
		CheckBox(ui.Box(ui.Canvas), { 10.0f, 20.0f }, { 1920.0f - 40.0f, 1080.0f - 60.0f });
		CheckBox(ui.Box(fill), { 10.0f, 20.0f }, { 1920.0f - 40.0f, 1080.0f - 60.0f });
		CheckBox(ui.Box(topRight), { 1920.0f - 30.0f - 50.0f, 20.0f }, { 50.0f, 50.0f });
	}

	TEST_CASE("Vertical and horizontal groups stack their children with padding, spacing and every alignment")
	{
		const glm::vec2 sizes[3] = { { 100.0f, 50.0f }, { 150.0f, 60.0f }, { 80.0f, 70.0f } };
		for (const UILayoutType type : { UILayoutType::Vertical, UILayoutType::Horizontal })
		{
			const int main = type == UILayoutType::Horizontal ? 0 : 1;
			const int cross = 1 - main;
			for (int alignment = 0; alignment < 9; alignment++)
			{
				for (const bool stretch : { false, true })
				{
					CAPTURE(static_cast<int>(type));
					CAPTURE(alignment);
					CAPTURE(stretch);
					UITestScene ui;
					Entity group = ui.Add(ui.Canvas, "Group", PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 500.0f, 400.0f }));
					UILayoutComponent& layout = group.AddComponent<UILayoutComponent>();
					layout.Type = type;
					layout.Padding = { 10.0f, 20.0f, 30.0f, 40.0f };
					layout.Spacing = { 4.0f, 5.0f };
					layout.ChildAlignment = static_cast<UIChildAlignment>(alignment);
					layout.StretchChildren = stretch;
					std::vector<Entity> children;
					for (const glm::vec2& size : sizes)
						children.push_back(ui.Add(group, "Child", PointRect({ 0.3f, 0.7f }, { 0.2f, 0.9f }, { 33.0f, 44.0f }, size)));

					const glm::vec2 innerMin(10.0f, 20.0f);
					const glm::vec2 innerSize(500.0f - 40.0f, 400.0f - 60.0f);
					const float alignMain = static_cast<float>(main == 0 ? alignment % 3 : alignment / 3) * 0.5f;
					const float alignCross = static_cast<float>(cross == 0 ? alignment % 3 : alignment / 3) * 0.5f;
					float block = layout.Spacing[main] * 2.0f;
					for (const glm::vec2& size : sizes)
						block += size[main];
					float position = innerMin[main] + (innerSize[main] - block) * alignMain;
					for (size_t index = 0; index < children.size(); index++)
					{
						const UIBox box = ui.Box(children[index]);
						CHECK(box.Min[main] == doctest::Approx(position));
						CHECK(box.Size[main] == doctest::Approx(sizes[index][main]));
						const float crossSize = stretch ? innerSize[cross] : sizes[index][cross];
						CHECK(box.Size[cross] == doctest::Approx(crossSize));
						CHECK(box.Min[cross] == doctest::Approx(innerMin[cross] + (innerSize[cross] - crossSize) * alignCross));
						position += sizes[index][main] + layout.Spacing[main];
					}
				}
			}
		}
	}

	TEST_CASE("Groups skip inactive children and leave those ignoring layout to their anchors")
	{
		UITestScene ui;
		Entity group = ui.Add(ui.Canvas, "Group", PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 200.0f, 400.0f }));
		group.AddComponent<UILayoutComponent>().Spacing = { 0.0f, 10.0f };
		Entity first = ui.Add(group, "First", PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 50.0f, 20.0f }));
		Entity hidden = ui.Add(group, "Hidden", PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 50.0f, 30.0f }));
		UIRectComponent free = PointRect({ 1.0f, 1.0f }, { 1.0f, 1.0f }, { -5.0f, -5.0f }, { 40.0f, 40.0f });
		free.IgnoreLayout = true;
		Entity ignoring = ui.Add(group, "Ignoring", free);
		Entity last = ui.Add(group, "Last", PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 50.0f, 25.0f }));

		CheckBox(ui.Box(last), { 0.0f, 20.0f + 10.0f + 30.0f + 10.0f }, { 50.0f, 25.0f });
		hidden.AddComponent<InactiveComponent>();
		CheckBox(ui.Box(last), { 0.0f, 30.0f }, { 50.0f, 25.0f });
		CHECK_FALSE(ui.Visible(hidden));
		CheckBox(ui.Box(first), { 0.0f, 0.0f }, { 50.0f, 20.0f });
		CheckBox(ui.Box(ignoring), { 200.0f - 5.0f - 40.0f, 400.0f - 5.0f - 40.0f }, { 40.0f, 40.0f });
		hidden.RemoveComponent<InactiveComponent>();
		CheckBox(ui.Box(last), { 0.0f, 70.0f }, { 50.0f, 25.0f });
		Tests::CheckSceneCaches(ui.Target);
	}

	TEST_CASE("Grids fill rows of cells, as many as fit or as many columns as asked for")
	{
		UITestScene ui;
		Entity grid = ui.Add(ui.Canvas, "Grid", PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 270.0f, 300.0f }));
		UILayoutComponent& layout = grid.AddComponent<UILayoutComponent>();
		layout.Type = UILayoutType::Grid;
		layout.Padding = { 5.0f, 5.0f, 5.0f, 5.0f };
		layout.Spacing = { 4.0f, 6.0f };
		layout.CellSize = { 40.0f, 30.0f };
		std::vector<Entity> cells;
		for (int index = 0; index < 8; index++)
			cells.push_back(ui.Add(grid, "Cell", PointRect({ 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 1.0f, 1.0f })));

		// (260 + 4) / 44 = 6 columns: two rows.
		CheckBox(ui.Box(cells[0]), { 5.0f, 5.0f }, { 40.0f, 30.0f });
		CheckBox(ui.Box(cells[5]), { 5.0f + 5.0f * 44.0f, 5.0f }, { 40.0f, 30.0f });
		CheckBox(ui.Box(cells[6]), { 5.0f, 5.0f + 36.0f }, { 40.0f, 30.0f });

		// Three columns, the block centered.
		layout.Columns = 3;
		layout.ChildAlignment = UIChildAlignment::MiddleCenter;
		grid.MarkModified<UILayoutComponent>();
		const float blockWidth = 3.0f * 40.0f + 2.0f * 4.0f;
		const float blockHeight = 3.0f * 30.0f + 2.0f * 6.0f;
		const glm::vec2 start(5.0f + (260.0f - blockWidth) * 0.5f, 5.0f + (290.0f - blockHeight) * 0.5f);
		CheckBox(ui.Box(cells[0]), start, { 40.0f, 30.0f });
		CheckBox(ui.Box(cells[4]), start + glm::vec2(44.0f, 36.0f), { 40.0f, 30.0f });
		CheckBox(ui.Box(cells[7]), start + glm::vec2(44.0f, 72.0f), { 40.0f, 30.0f });
	}

	TEST_CASE("Rects fit their text and their layout's children, bottom up")
	{
		UITestScene ui;
		UIRectComponent panelRect = PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 300.0f, 0.0f });
		panelRect.FitContent = UIFitContent::Height;
		Entity panel = ui.Add(ui.Canvas, "Panel", panelRect);
		UILayoutComponent& layout = panel.AddComponent<UILayoutComponent>();
		layout.Padding = { 8.0f, 8.0f, 8.0f, 8.0f };
		layout.Spacing = { 0.0f, 4.0f };
		layout.StretchChildren = true;
		UIRectComponent labelRect = PointRect({ 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 10.0f, 10.0f });
		labelRect.FitContent = UIFitContent::Both;
		Entity label = ui.Add(panel, "Label", labelRect);
		UITextComponent& text = label.AddComponent<UITextComponent>();
		text.Text = "Fitted label";
		text.FontSize = 20.0f;
		Entity fixed = ui.Add(panel, "Fixed", PointRect({ 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 10.0f, 40.0f }));

		const FontMetrics& metrics = Font::GetDefault()->GetFace()->GetMetrics();
		const float textHeight = (metrics.Ascent - metrics.Descent) * 20.0f;
		// The label's preferred height is its text; across the vertical group it is stretched to the padded width.
		CheckBox(ui.Box(label), { 18.0f, 18.0f }, { 300.0f - 16.0f, textHeight });
		CheckBox(ui.Box(fixed), { 18.0f, 18.0f + textHeight + 4.0f }, { 300.0f - 16.0f, 40.0f });
		CheckBox(ui.Box(panel), { 10.0f, 10.0f }, { 300.0f, 8.0f + textHeight + 4.0f + 40.0f + 8.0f });

		// Outside a group a fitted label takes its unwrapped text's width.
		Entity free = ui.Add(ui.Canvas, "Free", labelRect);
		free.AddComponent<UITextComponent>(text).Text = "Some longer label";
		CHECK(ui.Box(free).Size.x == doctest::Approx(ShapedWidth("Some longer label") * 20.0f));

		// A fitted height wraps the text at the rect's width.
		UIRectComponent wrappedRect = PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f });
		wrappedRect.Size.x = ShapedWidth("Two words") * 20.0f + 1.0f;
		wrappedRect.FitContent = UIFitContent::Height;
		Entity wrapped = ui.Add(ui.Canvas, "Wrapped", wrappedRect);
		wrapped.AddComponent<UITextComponent>(text).Text = "Two words two words";
		CHECK(ui.Box(wrapped).Size.y == doctest::Approx(textHeight + metrics.LineHeight * 20.0f));
	}

	TEST_CASE("Slider fills and handles follow the value, and a toggle's checkmark shows while it is on")
	{
		UITestScene ui;
		Entity slider = ui.Add(ui.Canvas, "Slider", PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 100.0f, 100.0f }, { 200.0f, 20.0f }));
		Entity fill = ui.Add(slider, "Fill", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }));
		Entity handle = ui.Add(slider, "Handle", PointRect({ 0.0f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 10.0f, 30.0f }));
		UISliderComponent& component = slider.AddComponent<UISliderComponent>();
		component.MinValue = 0.0f;
		component.MaxValue = 10.0f;
		component.Value = 2.5f;
		component.Fill = fill.GetUUID();
		component.Handle = handle.GetUUID();
		CheckBox(ui.Box(fill), { 100.0f, 100.0f }, { 50.0f, 20.0f });
		CheckBox(ui.Box(handle), { 150.0f - 5.0f, 110.0f - 15.0f }, { 10.0f, 30.0f });

		slider.GetComponent<UISliderComponent>().Direction = UIFillDirection::RightToLeft;
		slider.MarkModified<UISliderComponent>();
		CheckBox(ui.Box(fill), { 250.0f, 100.0f }, { 50.0f, 20.0f });
		CheckBox(ui.Box(handle), { 250.0f - 5.0f, 95.0f }, { 10.0f, 30.0f });

		slider.GetComponent<UISliderComponent>().Value = 10.0f;
		slider.GetComponent<UISliderComponent>().Direction = UIFillDirection::LeftToRight;
		slider.MarkModified<UISliderComponent>();
		CheckBox(ui.Box(fill), { 100.0f, 100.0f }, { 200.0f, 20.0f });

		Entity toggle = ui.Add(ui.Canvas, "Toggle", PointRect({ 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 30.0f, 30.0f }));
		Entity checkmark = ui.Add(toggle, "Checkmark", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }));
		Entity tick = ui.Add(checkmark, "Tick", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }));
		toggle.AddComponent<UIToggleComponent>().Checkmark = checkmark.GetUUID();
		CHECK_FALSE(ui.Visible(checkmark));
		CHECK_FALSE(ui.Visible(tick));
		CHECK(ui.Visible(toggle));
		toggle.GetComponent<UIToggleComponent>().IsOn = true;
		toggle.MarkModified<UIToggleComponent>();
		CHECK(ui.Visible(checkmark));
		CHECK(ui.Visible(tick));
	}

	TEST_CASE("Only root canvases define screens: a nested canvas is an element, stray UI components draw nothing")
	{
		UITestScene ui;
		Entity inner = ui.Add(ui.Canvas, "Inner Canvas", PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 20.0f, 30.0f }, { 400.0f, 300.0f }));
		UICanvasComponent& nestedCanvas = inner.AddComponent<UICanvasComponent>();
		nestedCanvas.ScaleMode = UICanvasScaleMode::ConstantPixelSize;
		nestedCanvas.ScaleFactor = 3.0f; // Ignored
		Entity child = ui.Add(inner, "Child", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }, { -10.0f, -10.0f }));
		CheckBox(ui.Box(inner), { 20.0f, 30.0f }, { 400.0f, 300.0f });
		CheckBox(ui.Box(child), { 25.0f, 35.0f }, { 390.0f, 290.0f });
		GameUIScene& scene = ui.GetUI();
		CHECK(scene.EnsureLayout({}).size() == 1);
		CHECK(scene.GetRootCanvasCount() == 1);

		// A rect outside any canvas, and a UI component without a rect, are no elements.
		Entity stray = ui.Target.CreateEntity("Stray");
		stray.AddComponent<UIRectComponent>();
		Entity bare = ui.Target.CreateChildEntity(ui.Canvas, "Bare");
		bare.AddComponent<UIImageComponent>();
		scene.EnsureLayout({});
		UICanvasView view;
		uint32_t element = 0;
		CHECK_FALSE(scene.FindElement(stray.GetHandle(), {}, view, element));
		CHECK_FALSE(scene.FindElement(bare.GetHandle(), {}, view, element));

		// Taking the outer canvas away makes the inner one a root.
		ui.Canvas.RemoveComponent<UICanvasComponent>();
		CHECK(scene.EnsureLayout({}).size() == 1);
		CHECK(scene.GetRootCanvasCount() == 1);
		CheckBox(ui.Box(child), { 5.0f, 5.0f }, { 1920.0f / 3.0f - 10.0f, 1080.0f / 3.0f - 10.0f });
	}
}

TEST_SUITE("GameUI.Layout")
{
	TEST_CASE("Elements moved between canvases, and canvases nested and unnested, lay out where they are")
	{
		UITestScene ui;
		Entity other = ui.Target.CreateEntity("Other Canvas");
		other.AddComponent<UIRectComponent>();
		UICanvasComponent& otherCanvas = other.AddComponent<UICanvasComponent>();
		otherCanvas.ScaleMode = UICanvasScaleMode::ConstantPixelSize;
		otherCanvas.ScaleFactor = 2.0f;
		Entity moved = ui.Add(ui.Canvas, "Moved", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }, { -10.0f, -10.0f }));
		Entity child = ui.Add(moved, "Child", StretchRect({ 0.0f, 0.0f }, { 0.5f, 1.0f }));
		CheckBox(ui.Box(child), { 5.0f, 5.0f }, { 955.0f, 1070.0f });
		ui.Target.SetParent(moved, other, false);
		CheckBox(ui.Box(moved), { 5.0f, 5.0f }, { 950.0f, 530.0f });
		CheckBox(ui.Box(child), { 5.0f, 5.0f }, { 475.0f, 530.0f });
		// The other canvas nested in the first: one root; the nested canvas is an element of it, its own rect (100x100 around
		// the center) holding the moved element.
		ui.Target.SetParent(other, ui.Canvas, false);
		CHECK(ui.GetUI().EnsureLayout({}).size() == 1);
		CheckBox(ui.Box(other), { 910.0f, 490.0f }, { 100.0f, 100.0f });
		CheckBox(ui.Box(child), { 915.0f, 495.0f }, { 45.0f, 90.0f });
		ui.Target.SetParent(other, Entity(), false);
		CHECK(ui.GetUI().EnsureLayout({}).size() == 2);
		CheckBox(ui.Box(child), { 5.0f, 5.0f }, { 475.0f, 530.0f });
		Tests::CheckSceneCaches(ui.Target);
	}
}

TEST_SUITE("GameUI.Scale")
{
	TEST_CASE("The scale modes give each screen its scale and its size in canvas units")
	{
		const glm::vec2 screens[] = { { 1920.0f, 1080.0f }, { 2560.0f, 1080.0f }, { 1024.0f, 768.0f }, { 1080.0f, 1920.0f } };
		for (const glm::vec2& screenSize : screens)
		{
			for (const float match : { 0.0f, 0.5f, 1.0f })
			{
				for (const float contentScale : { 1.0f, 1.5f, 2.0f })
				{
					CAPTURE(screenSize);
					CAPTURE(match);
					CAPTURE(contentScale);
					const UIScreen screen { screenSize, contentScale };
					UITestScene ui(UICanvasScaleMode::ScaleWithScreen);
					UICanvasComponent& canvas = ui.Canvas.GetComponent<UICanvasComponent>();
					canvas.ReferenceResolution = { 1280.0f, 720.0f };
					canvas.MatchWidthOrHeight = match;
					canvas.ScaleFactor = 1.25f;
					const float expected = std::exp2(std::log2(screenSize.x / 1280.0f) * (1.0f - match) + std::log2(screenSize.y / 720.0f) * match);
					std::span<const UICanvasView> views = ui.GetUI().EnsureLayout(screen);
					REQUIRE(views.size() == 1);
					CHECK(views[0].Scale == doctest::Approx(expected).epsilon(1e-5));
					CHECK(views[0].UnitSize.x == doctest::Approx(screenSize.x / expected).epsilon(1e-5));
					CHECK(views[0].UnitSize.y == doctest::Approx(screenSize.y / expected).epsilon(1e-5));

					canvas.ScaleMode = UICanvasScaleMode::ConstantPixelSize;
					ui.Canvas.MarkModified<UICanvasComponent>();
					views = ui.GetUI().EnsureLayout(screen);
					CHECK(views[0].Scale == doctest::Approx(1.25f).epsilon(1e-5));
					CHECK(views[0].UnitSize.x == doctest::Approx(screenSize.x / 1.25f).epsilon(1e-5));

					canvas.ScaleMode = UICanvasScaleMode::ConstantPhysicalSize;
					ui.Canvas.MarkModified<UICanvasComponent>();
					views = ui.GetUI().EnsureLayout(screen);
					CHECK(views[0].Scale == doctest::Approx(1.25f * contentScale).epsilon(1e-5));
					CHECK(views[0].UnitSize.y == doctest::Approx(screenSize.y / (1.25f * contentScale)).epsilon(1e-5));
				}
			}
		}
	}

	TEST_CASE("Canvases are drawn by sort order, then hierarchy order")
	{
		UITestScene ui;
		Entity second = ui.Target.CreateEntity("Second");
		second.AddComponent<UICanvasComponent>().SortOrder = -1;
		Entity third = ui.Target.CreateEntity("Third");
		third.AddComponent<UICanvasComponent>();
		const std::span<const UICanvasView> views = ui.GetUI().EnsureLayout({});
		REQUIRE(views.size() == 3);
		CHECK(views[0].Canvas == second.GetHandle());
		CHECK(views[1].Canvas == ui.Canvas.GetHandle());
		CHECK(views[2].Canvas == third.GetHandle());
	}
}

TEST_SUITE("GameUI.Dirty")
{
	TEST_CASE("A rect replaced in place (removed and added again, as undo restores it) is laid out again")
	{
		UITestScene ui;
		Entity panel = ui.Add(ui.Canvas, "Panel", PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 10.0f, 10.0f }, { 100.0f, 50.0f }));
		Entity label = ui.Add(panel, "Label", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }));
		CheckBox(ui.Box(label), { 10.0f, 10.0f }, { 100.0f, 50.0f });

		// The same entity, the same place in the hierarchy, new values: nothing moved, but the rect changed.
		panel.RemoveComponent<UIRectComponent>();
		panel.AddComponent<UIRectComponent>(PointRect({ 0.0f, 0.0f }, { 0.0f, 0.0f }, { 30.0f, 40.0f }, { 200.0f, 80.0f }));
		CheckBox(ui.Box(panel), { 30.0f, 40.0f }, { 200.0f, 80.0f });
		CheckBox(ui.Box(label), { 30.0f, 40.0f }, { 200.0f, 80.0f });
		std::string error;
		CHECK_MESSAGE(ui.GetUI().Validate(&error), error);

		// A change marked before another one rebuilds the canvas stays marked.
		panel.GetComponent<UIRectComponent>().Position = { 5.0f, 6.0f };
		panel.MarkModified<UIRectComponent>();
		Entity extra = ui.Add(ui.Canvas, "Extra", PointRect({ 1.0f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f }, { 10.0f, 10.0f }));
		CheckBox(ui.Box(panel), { 5.0f, 6.0f }, { 200.0f, 80.0f });
		CheckBox(ui.Box(extra), { 1910.0f, 1070.0f }, { 10.0f, 10.0f });
		CHECK_MESSAGE(ui.GetUI().Validate(&error), error);
	}

	TEST_CASE("An idle frame lays out, shapes and builds nothing; a change costs its element")
	{
		Scene scene("Widgets");
		const Tests::Perf::UIWidgets widgets = Tests::Perf::CreateUIWidgets(scene);
		GameUIScene& ui = GameUIScene::Get(scene);
		Frame(ui);
		GameUISceneStats stats = ui.TakeStats();
		CHECK(stats.ElementsLaidOut == 10'001);
		CHECK(stats.FullRebuilds == 1);
		CHECK(stats.TextsShaped == 4'800);
		CHECK(stats.QuadsBuilt > 4'800 + 5'200);

		Frame(ui);
		stats = ui.TakeStats();
		CHECK(stats.ElementsLaidOut == 0);
		CHECK(stats.TextsShaped == 0);
		CHECK(stats.QuadsBuilt == 0);
		CHECK(stats.FullRebuilds == 0);

		// A label's text: shaped again and its quads rewritten; its rect does not depend on its text, so nothing is laid out.
		Entity label = widgets.Labels[1234];
		label.GetComponent<UITextComponent>().Text = "Changed!";
		label.MarkModified<UITextComponent>();
		Frame(ui);
		stats = ui.TakeStats();
		CHECK(stats.TextsShaped == 1);
		CHECK(stats.ElementsLaidOut == 0);
		CHECK(stats.QuadsBuilt == 8);

		// A button's color: its quad alone.
		Entity button = widgets.Buttons[77];
		button.GetComponent<UIImageComponent>().Color = { 1.0f, 0.0f, 0.0f, 1.0f };
		button.MarkModified<UIImageComponent>();
		Frame(ui);
		stats = ui.TakeStats();
		CHECK(stats.QuadsBuilt == 1);
		CHECK(stats.ElementsLaidOut == 0);
		CHECK(stats.TextsShaped == 0);

		// A leaf moved under a plain parent: that element.
		label.GetComponent<UIRectComponent>().Position = { 1.0f, 0.0f };
		label.MarkModified<UIRectComponent>();
		Frame(ui);
		CHECK(ui.TakeStats().ElementsLaidOut == 1);

		// A button made inactive: its panel's group lays out again (the panel, 12 buttons and their labels).
		button.AddComponent<InactiveComponent>();
		Frame(ui);
		stats = ui.TakeStats();
		CHECK(stats.ElementsLaidOut == 25);
		button.RemoveComponent<InactiveComponent>();
		Frame(ui);
		CHECK(ui.TakeStats().ElementsLaidOut == 25);

		// A button moved to another panel: the subtrees of both groups, which hold the moved one.
		scene.SetParent(widgets.Buttons[0], widgets.Panels[1], false);
		Frame(ui);
		stats = ui.TakeStats();
		CHECK(stats.ElementsLaidOut == (1 + 11 * 2) + (1 + 13 * 2));
		CHECK(stats.FullRebuilds == 0);

		// A screen of the same aspect ratio under ScaleWithScreen: the same layout in canvas units.
		Frame(ui, UIScreen { { 3840.0f, 2160.0f }, 1.0f });
		stats = ui.TakeStats();
		CHECK(stats.ElementsLaidOut == 0);
		CHECK(stats.TextsShaped == 0);
		CHECK(stats.QuadsBuilt == 0);
		// Another aspect ratio: a layout of its own (every element), kept next to the first.
		Frame(ui, UIScreen { { 1280.0f, 1024.0f }, 1.0f });
		CHECK(ui.TakeStats().ElementsLaidOut == 10'001);
		Frame(ui);
		CHECK(ui.TakeStats().ElementsLaidOut == 0);
	}

	TEST_CASE("A label's text change lays out its panel when the label and its button fit their content")
	{
		Scene scene("Widgets");
		const Tests::Perf::UIWidgets widgets = Tests::Perf::CreateUIWidgets(scene, 400, 12, true);
		GameUIScene& ui = GameUIScene::Get(scene);
		Frame(ui);
		ui.TakeStats();
		Entity label = widgets.Labels[4321];
		label.GetComponent<UITextComponent>().Text = "Other text";
		label.MarkModified<UITextComponent>();
		Frame(ui);
		const GameUISceneStats stats = ui.TakeStats();
		CHECK(stats.TextsShaped == 1);
		CHECK(stats.ElementsLaidOut == 25);
	}

	TEST_CASE("A write without a signal makes validation fail, naming the entity")
	{
		UITestScene ui;
		Entity box = ui.Add(ui.Canvas, "Unsignaled", PointRect({ 0.5f, 0.5f }, { 0.5f, 0.5f }, { 0.0f, 0.0f }, { 10.0f, 10.0f }));
		Frame(ui.GetUI());
		std::string error;
		CHECK(ui.GetUI().Validate(&error));
		box.GetComponent<UIRectComponent>().Size = { 20.0f, 10.0f }; // No MarkModified
		CHECK_FALSE(ui.GetUI().Validate(&error));
		CHECK(error.find("Unsignaled") != std::string::npos);
		CHECK(error.find(box.GetUUID().ToString()) != std::string::npos);
	}

	TEST_CASE("Moving plain entities with children costs the canvases nothing unless elements move with them")
	{
		Scene scene("Widgets");
		const Tests::Perf::UIWidgets widgets = Tests::Perf::CreateUIWidgets(scene, 20, 12);
		// A hand with an item of nested meshes, and a container beside it, as gameplay reparents them.
		Entity hand = scene.CreateEntity("Hand");
		Entity container = scene.CreateEntity("Container");
		Entity item = scene.CreateChildEntity(hand, "Item");
		scene.CreateChildEntity(scene.CreateChildEntity(item, "Part"), "Detail");
		GameUIScene& ui = GameUIScene::Get(scene);
		Frame(ui);
		ui.TakeStats();
		for (int move = 0; move < 4; move++)
		{
			scene.SetParent(item, move % 2 == 0 ? container : hand, false);
			scene.SetParent(scene.CreateChildEntity(container, "Spawned"), item, false);
			Frame(ui);
			const GameUISceneStats stats = ui.TakeStats();
			CHECK(stats.QuadsBuilt == 0);
			CHECK(stats.ElementsLaidOut == 0);
			CHECK(stats.FullRebuilds == 0);
		}
		std::string error;
		CHECK_MESSAGE(ui.Validate(&error), error);
	}

	TEST_CASE("Elements below plain entities, and canvases below moved elements, follow their moves")
	{
		UITestScene ui;
		Entity other = ui.Target.CreateEntity("Other Canvas");
		other.AddComponent<UIRectComponent>();
		UICanvasComponent& otherCanvas = other.AddComponent<UICanvasComponent>();
		otherCanvas.ScaleMode = UICanvasScaleMode::ConstantPixelSize;
		otherCanvas.ScaleFactor = 2.0f;
		Entity group = ui.Target.CreateChildEntity(ui.Canvas, "Plain Group");
		Entity child = ui.Add(group, "Child", StretchRect({ 0.0f, 0.0f }, { 0.5f, 1.0f }));
		GameUIScene& scene = ui.GetUI();
		CheckBox(ui.Box(child), { 0.0f, 0.0f }, { 960.0f, 1080.0f });
		// The plain group into the other canvas, then out of every canvas.
		ui.Target.SetParent(group, other, false);
		CheckBox(ui.Box(child), { 0.0f, 0.0f }, { 480.0f, 540.0f });
		ui.Target.SetParent(group, Entity(), false);
		scene.EnsureLayout({});
		UICanvasView view;
		uint32_t element = 0;
		CHECK_FALSE(scene.FindElement(child.GetHandle(), {}, view, element));
		ui.Target.SetParent(group, ui.Canvas, false);
		CheckBox(ui.Box(child), { 0.0f, 0.0f }, { 960.0f, 1080.0f });

		// A canvas nested below an element, and one below a plain group, become roots when what holds them leaves.
		Entity panel = ui.Add(ui.Canvas, "Panel", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }));
		Entity inner = ui.Add(panel, "Inner Canvas", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }));
		inner.AddComponent<UICanvasComponent>().ScaleMode = UICanvasScaleMode::ConstantPixelSize;
		Entity deep = ui.Target.CreateChildEntity(group, "Deep Group");
		Entity deepCanvas = ui.Add(deep, "Deep Canvas", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }));
		deepCanvas.AddComponent<UICanvasComponent>().ScaleMode = UICanvasScaleMode::ConstantPixelSize;
		CHECK(scene.EnsureLayout({}).size() == 2);
		ui.Target.SetParent(panel, Entity(), false);
		CHECK(scene.EnsureLayout({}).size() == 3);
		ui.Target.SetParent(group, Entity(), false);
		CHECK(scene.EnsureLayout({}).size() == 4);
		ui.Target.SetParent(group, other, false);
		CHECK(scene.EnsureLayout({}).size() == 3);
		std::string error;
		CHECK_MESSAGE(scene.Validate(&error), error);
		Tests::CheckSceneCaches(ui.Target);
	}

	TEST_CASE("More hierarchy moves than the scene remembers still lay out correctly, at most once whole")
	{
		Scene scene("Widgets");
		const Tests::Perf::UIWidgets widgets = Tests::Perf::CreateUIWidgets(scene, 20, 12);
		GameUIScene& ui = GameUIScene::Get(scene);
		Frame(ui);
		ui.TakeStats();
		// Reordering the panel's buttons more often than the scene's move log holds.
		for (size_t move = 0; move < Scene::c_MaxHierarchyMoves + 10; move++)
			scene.SetSiblingIndex(widgets.Buttons[move % 12], (move * 7) % 12);
		Frame(ui);
		const GameUISceneStats stats = ui.TakeStats();
		CHECK(stats.ElementsLaidOut <= 1 + 20 + 20 * 24);
		std::string error;
		CHECK_MESSAGE(ui.Validate(&error), error);
	}
}

TEST_SUITE("GameUI.Mask")
{
	TEST_CASE("Masks clip their descendants to their padded rects, nested masks to where both overlap")
	{
		UITestScene ui;
		const glm::vec2 topLeft(0.0f);
		Entity panel = ui.Add(ui.Canvas, "Panel", PointRect(topLeft, topLeft, { 100.0f, 100.0f }, { 400.0f, 300.0f }));
		UIMaskComponent& mask = panel.AddComponent<UIMaskComponent>();
		mask.Padding = { 10.0f, 20.0f, 30.0f, 40.0f };
		Entity spill = ui.Add(panel, "Spill", PointRect(topLeft, topLeft, { -50.0f, -50.0f }, { 600.0f, 600.0f }));
		Entity inner = ui.Add(panel, "Inner", PointRect(topLeft, topLeft, { 200.0f, 150.0f }, { 400.0f, 400.0f }));
		inner.AddComponent<UIMaskComponent>().Softness = 4.0f;
		Entity deep = ui.Add(inner, "Deep", PointRect(topLeft, topLeft, { 0.0f, 0.0f }, { 50.0f, 50.0f }));
		Entity outside = ui.Add(ui.Canvas, "Outside", PointRect(topLeft, topLeft, { 0.0f, 0.0f }, { 50.0f, 50.0f }));

		GameUIScene& scene = ui.GetUI();
		scene.EnsureLayout(GameUIScene::c_DefaultScreen);
		auto clipOf = [&](Entity entity)
		{
			UICanvasView view;
			uint32_t element = 0;
			REQUIRE(scene.FindElement(entity.GetHandle(), GameUIScene::c_DefaultScreen, view, element));
			return scene.GetElementClip(view, element);
		};
		auto visibleOf = [&](Entity entity)
		{
			UICanvasView view;
			uint32_t element = 0;
			REQUIRE(scene.FindElement(entity.GetHandle(), GameUIScene::c_DefaultScreen, view, element));
			return scene.GetVisibleBox(view, element);
		};
		// A mask clips what is below it, never itself or what is beside it.
		CHECK_FALSE(clipOf(panel).has_value());
		CHECK_FALSE(clipOf(outside).has_value());
		CheckBox(visibleOf(panel), { 100.0f, 100.0f }, { 400.0f, 300.0f });
		// The panel's rect (100, 100)-(500, 400) inset by its padding.
		REQUIRE(clipOf(spill).has_value());
		CheckBox(*clipOf(spill), { 110.0f, 120.0f }, { 360.0f, 240.0f });
		CheckBox(visibleOf(spill), { 110.0f, 120.0f }, { 360.0f, 240.0f });
		REQUIRE(clipOf(inner).has_value());
		CheckBox(*clipOf(inner), { 110.0f, 120.0f }, { 360.0f, 240.0f });
		// Inside both: the inner rect (300, 250)-(700, 650) within (110, 120)-(470, 360).
		REQUIRE(clipOf(deep).has_value());
		CheckBox(*clipOf(deep), { 300.0f, 250.0f }, { 170.0f, 110.0f });
		CheckBox(visibleOf(deep), { 300.0f, 250.0f }, { 50.0f, 50.0f });

		// The quads name the clip rects, which carry the softness.
		for (const UICanvasView& view : scene.EnsureLayout(GameUIScene::c_DefaultScreen))
		{
			const std::span<const RenderData::UIClipRect> clips = scene.GetClipRects(view);
			REQUIRE(clips.size() == 2);
			CHECK(clips[0].Parameters.x == 0.0f);
			CHECK(clips[1].Parameters.x == 4.0f);
		}
		std::string error;
		CHECK_MESSAGE(scene.Validate(&error), error);

		// Removing the outer mask leaves the inner one, which no longer meets it.
		panel.RemoveComponent<UIMaskComponent>();
		scene.EnsureLayout(GameUIScene::c_DefaultScreen);
		CHECK_FALSE(clipOf(spill).has_value());
		REQUIRE(clipOf(deep).has_value());
		CheckBox(*clipOf(deep), { 300.0f, 250.0f }, { 400.0f, 400.0f });
	}

	TEST_CASE("Masked elements are hit only where their mask leaves them, and their quads name its clip rect")
	{
		UITestScene ui;
		const glm::vec2 topLeft(0.0f);
		Entity panel = ui.Add(ui.Canvas, "Panel", PointRect(topLeft, topLeft, { 100.0f, 100.0f }, { 200.0f, 100.0f }));
		panel.AddComponent<UIMaskComponent>();
		Entity item = ui.Add(panel, "Item", PointRect(topLeft, topLeft, { 0.0f, 50.0f }, { 200.0f, 100.0f }));
		item.AddComponent<UIImageComponent>();

		GameUIScene& scene = ui.GetUI();
		const std::span<const UICanvasView> views = scene.EnsureLayout(GameUIScene::c_DefaultScreen);
		GameUIHitTester tester;
		tester.Update(scene, views);
		// The item spans (100, 150)-(300, 250); the mask leaves (100, 150)-(300, 200).
		const std::optional<UIHitResult> inside = tester.HitTest({ 150.0f, 180.0f });
		REQUIRE(inside.has_value());
		CHECK(inside->Entity == item.GetHandle());
		CHECK_FALSE(tester.HitTest({ 150.0f, 220.0f }).has_value());
		std::vector<UIHitResult> all;
		tester.HitTestAll(scene, views, { 150.0f, 220.0f }, all);
		CHECK(std::none_of(all.begin(), all.end(), [&](const UIHitResult& hit) { return hit.Entity == item.GetHandle(); }));
		tester.HitTestAll(scene, views, { 150.0f, 180.0f }, all);
		CHECK(std::any_of(all.begin(), all.end(), [&](const UIHitResult& hit) { return hit.Entity == item.GetHandle(); }));

		const std::vector<RenderData::UIQuad>& quads = scene.EnsureQuads(views[0]);
		bool named = false;
		for (const RenderData::UIQuad& quad : quads)
		{
			if ((quad.Flags & RenderData::c_UIQuadKindMask) == RenderData::c_UIQuadImage)
			{
				CHECK(quad.Clip == 1);
				named = true;
			}
		}
		CHECK(named);
	}
}

TEST_SUITE("GameUI.Dirty")
{
	TEST_CASE("A mask's padding changes its clip rects alone; a write without a signal fails validation")
	{
		UITestScene ui;
		const glm::vec2 topLeft(0.0f);
		Entity panel = ui.Add(ui.Canvas, "Masked Panel", PointRect(topLeft, topLeft, { 100.0f, 100.0f }, { 200.0f, 100.0f }));
		panel.AddComponent<UIMaskComponent>();
		Entity item = ui.Add(panel, "Item", PointRect(topLeft, topLeft, { 0.0f, 0.0f }, { 50.0f, 50.0f }));
		item.AddComponent<UIImageComponent>();
		GameUIScene& scene = ui.GetUI();
		Frame(scene);
		scene.TakeStats();
		const UICanvasView view = scene.EnsureLayout(GameUIScene::c_DefaultScreen)[0];
		const uint64_t version = scene.GetClipVersion(view);
		const uint64_t hits = scene.GetHitStamp(view);

		// An idle frame computes nothing.
		Frame(scene);
		CHECK(scene.GetClipVersion(view) == version);

		panel.GetComponent<UIMaskComponent>().Padding = { 5.0f, 5.0f, 5.0f, 5.0f };
		panel.MarkModified<UIMaskComponent>();
		Frame(scene);
		const GameUISceneStats stats = scene.TakeStats();
		CHECK(stats.ElementsLaidOut == 0);
		CHECK(stats.QuadsBuilt == 0);
		CHECK(scene.GetClipVersion(view) == version + 1);
		CHECK(scene.GetHitStamp(view) != hits); // What the pointer can hit changed
		CheckBox(*scene.GetElementClip(view, 2), { 105.0f, 105.0f }, { 190.0f, 90.0f });

		// Moving the mask moves its clip rect.
		panel.GetComponent<UIRectComponent>().Position = { 120.0f, 100.0f };
		panel.MarkModified<UIRectComponent>();
		Frame(scene);
		CHECK(scene.TakeStats().ElementsLaidOut == 1 + 1);
		CheckBox(*scene.GetElementClip(view, 2), { 125.0f, 105.0f }, { 190.0f, 90.0f });

		std::string error;
		CHECK_MESSAGE(scene.Validate(&error), error);
		panel.GetComponent<UIMaskComponent>().Padding = { 0.0f, 0.0f, 0.0f, 0.0f }; // No MarkModified
		CHECK_FALSE(scene.Validate(&error));
		CHECK(error.find("Masked Panel") != std::string::npos);
	}

	TEST_CASE("An input field's text reshapes its text element alone, and an unsignaled write fails validation")
	{
		UITestScene ui;
		const glm::vec2 topLeft(0.0f);
		Entity field = ui.Add(ui.Canvas, "Name Field", PointRect(topLeft, topLeft, { 100.0f, 100.0f }, { 300.0f, 40.0f }));
		Entity text = ui.Add(field, "Shown", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }, { -16.0f, -8.0f }));
		text.AddComponent<UITextComponent>().Text = "never shown";
		Entity hint = ui.Add(field, "Hint", StretchRect({ 0.0f, 0.0f }, { 1.0f, 1.0f }));
		hint.AddComponent<UITextComponent>().Text = "Your name";
		UIInputFieldComponent& component = field.AddComponent<UIInputFieldComponent>();
		component.TextElement = text.GetUUID();
		component.Placeholder = hint.GetUUID();
		GameUIScene& scene = ui.GetUI();
		Frame(scene);
		scene.TakeStats();
		CHECK(ui.Visible(hint));
		const UICanvasView view = scene.EnsureLayout(GameUIScene::c_DefaultScreen)[0];
		CHECK(scene.GetElementText(view, 2).empty()); // The field's (empty) text, not the element's own

		field.GetComponent<UIInputFieldComponent>().Text = "Ada";
		field.MarkModified<UIInputFieldComponent>();
		Frame(scene);
		const GameUISceneStats stats = scene.TakeStats();
		CHECK(stats.TextsShaped == 1);
		CHECK(stats.ElementsLaidOut == 0);
		CHECK(scene.GetElementText(view, 2) == "Ada");
		CHECK_FALSE(ui.Visible(hint));

		std::string error;
		CHECK_MESSAGE(scene.Validate(&error), error);
		field.GetComponent<UIInputFieldComponent>().Text.clear(); // No MarkModified
		CHECK_FALSE(scene.Validate(&error));
		CHECK(error.find("Name Field") != std::string::npos);
	}
}
