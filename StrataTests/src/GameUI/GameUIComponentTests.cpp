#include <doctest/doctest.h>

#include "Scene/SceneTestUtils.h"
#include "Strata/GameUI/GameUIComponents.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"

#include <string>
#include <vector>

using namespace Strata;

namespace
{

	constexpr const char* c_UIComponents[] = { "UICanvas", "UIRect", "UIImage", "UIText", "UISelectable", "UIButton", "UIToggle", "UISlider", "UILayout" };

	// A canvas with a slider whose parts, a button and a toggle reference each other and an entity outside.
	struct ReferencingUI
	{
		Entity Outside;
		Entity Canvas;
		Entity Slider;
		Entity Fill;
		Entity Handle;
		Entity Button;
		Entity Toggle;
	};

	ReferencingUI CreateReferencingUI(Scene& scene)
	{
		ReferencingUI ui;
		ui.Outside = scene.CreateEntity("Outside");
		ui.Canvas = scene.CreateEntity("Canvas");
		ui.Canvas.AddComponent<UIRectComponent>();
		ui.Slider = scene.CreateChildEntity(ui.Canvas, "Slider");
		ui.Fill = scene.CreateChildEntity(ui.Slider, "Fill");
		ui.Handle = scene.CreateChildEntity(ui.Slider, "Handle");
		ui.Button = scene.CreateChildEntity(ui.Canvas, "Button");
		ui.Toggle = scene.CreateChildEntity(ui.Canvas, "Toggle");
		for (Entity element : { ui.Slider, ui.Fill, ui.Handle, ui.Button, ui.Toggle })
			element.AddComponent<UIRectComponent>();

		UICanvasComponent& canvas = ui.Canvas.AddComponent<UICanvasComponent>();
		canvas.FirstSelected = ui.Button.GetUUID();
		UISliderComponent& slider = ui.Slider.AddComponent<UISliderComponent>();
		slider.Fill = ui.Fill.GetUUID();
		slider.Handle = ui.Handle.GetUUID();
		slider.EventTarget = ui.Outside.GetUUID();
		UISelectableComponent& selectable = ui.Button.AddComponent<UISelectableComponent>();
		selectable.Navigation = UINavigation::Explicit;
		selectable.NavigateDown = ui.Toggle.GetUUID();
		selectable.NavigateUp = ui.Slider.GetUUID();
		ui.Button.AddComponent<UIButtonComponent>().EventTarget = ui.Outside.GetUUID();
		UIToggleComponent& toggle = ui.Toggle.AddComponent<UIToggleComponent>();
		toggle.Group = ui.Canvas.GetUUID();
		toggle.IsOn = true;
		return ui;
	}

}

TEST_SUITE("GameUI.Components")
{
	TEST_CASE("Every UI component is registered in the UI category and every one but the rect requires the rect")
	{
		for (const char* name : c_UIComponents)
		{
			INFO("Component ", name);
			const ComponentInfo* info = ComponentRegistry::Find(name);
			REQUIRE(info);
			CHECK(info->Category == "UI");
			CHECK(info->IsAddable());
			CHECK_FALSE(info->Description.empty());
			if (std::string(name) == "UIRect")
				CHECK(info->Requires.empty());
			else
				CHECK(info->Requires == std::vector<std::string> { "UIRect" });
		}
		CHECK(ComponentRegistry::Find<UIImageComponent>() == ComponentRegistry::Find("UIImage"));
		CHECK(ComponentRegistry::Find("UIImage")->DisplayName == "UI Image");
	}

	TEST_CASE("Adding a UI component by name adds the rect it requires first, and only when missing")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Plain");
		const ComponentInfo* image = ComponentRegistry::Find("UIImage");
		REQUIRE(image);
		std::string error;
		REQUIRE_MESSAGE(ComponentAccess::AddComponent(entity, *image, &error), error);
		CHECK(entity.HasComponent<UIImageComponent>());
		CHECK(entity.HasComponent<UIRectComponent>());

		// A rect the entity has keeps its values.
		Entity sized = scene.CreateEntity("Sized");
		sized.AddComponent<UIRectComponent>().Size = glm::vec2(7.0f, 9.0f);
		REQUIRE(ComponentAccess::AddComponent(sized, *ComponentRegistry::Find("UIText"), &error));
		CHECK(sized.HasComponent<UITextComponent>());
		CHECK(sized.GetComponent<UIRectComponent>().Size == glm::vec2(7.0f, 9.0f));

		// Removing the rect is allowed: the image then has no effect, and adding the rect back restores the element.
		REQUIRE(ComponentAccess::RemoveComponent(entity, *ComponentRegistry::Find("UIRect"), &error));
		CHECK(entity.HasComponent<UIImageComponent>());
		CHECK_FALSE(entity.HasComponent<UIRectComponent>());
	}

	TEST_CASE("The rect is added before the component that requires it, so observers find it")
	{
		Scene scene;
		Entity entity = scene.CreateEntity("Plain");
		struct Observer
		{
			Scene* Target = nullptr;
			bool RectPresent = false;
			void OnButton(entt::registry& registry, entt::entity handle) { RectPresent = registry.all_of<UIRectComponent>(handle); }
		} observer;
		scene.GetRegistry().on_construct<UIButtonComponent>().connect<&Observer::OnButton>(observer);
		REQUIRE(ComponentAccess::AddComponent(entity, *ComponentRegistry::Find("UIButton")));
		scene.GetRegistry().on_construct<UIButtonComponent>().disconnect<&Observer::OnButton>(observer);
		CHECK(observer.RectPresent);
	}
}

TEST_SUITE("GameUI.Serialization")
{
	TEST_CASE("UI components round-trip through scene documents with every value")
	{
		Scene scene("UI");
		ReferencingUI ui = CreateReferencingUI(scene);
		UIRectComponent& rect = ui.Slider.GetComponent<UIRectComponent>();
		rect.AnchorMin = { 0.1f, 0.2f };
		rect.AnchorMax = { 0.9f, 0.3f };
		rect.Pivot = { 0.0f, 1.0f };
		rect.Position = { -12.5f, 7.0f };
		rect.Size = { -20.0f, 40.0f };
		rect.FitContent = UIFitContent::Height;
		rect.IgnoreLayout = true;
		UIImageComponent& image = ui.Slider.AddComponent<UIImageComponent>();
		image.Texture = UUID(0x1234);
		image.Color = { 0.1f, 0.2f, 0.3f, 0.4f };
		image.Type = UIImageType::Sliced;
		image.UVRect = { 0.25f, 0.0f, 0.75f, 1.0f };
		image.SliceBorders = { 1.0f, 2.0f, 3.0f, 4.0f };
		image.SliceScale = 0.5f;
		image.FillAmount = 0.25f;
		image.FillDirection = UIFillDirection::BottomToTop;
		image.PreserveAspect = true;
		image.CornerRadius = 6.0f;
		image.BorderWidth = 2.0f;
		image.BorderColor = { 1.0f, 0.0f, 0.0f, 1.0f };
		image.RaycastTarget = false;
		UITextComponent& text = ui.Handle.AddComponent<UITextComponent>();
		text.Text = "Lives: 3\nScore: 9";
		text.Font = UUID(0x5678);
		text.FontSize = 18.0f;
		text.Alignment = TextAlignment::Right;
		text.VerticalAlignment = UITextVerticalAlignment::Bottom;
		text.Wrap = false;
		text.Overflow = UITextOverflow::Ellipsis;
		text.LineSpacing = 1.5f;
		text.OutlineWidth = 0.05f;
		text.RaycastTarget = true;
		UILayoutComponent& layout = ui.Canvas.AddComponent<UILayoutComponent>();
		layout.Type = UILayoutType::Grid;
		layout.Padding = { 1.0f, 2.0f, 3.0f, 4.0f };
		layout.Spacing = { 5.0f, 6.0f };
		layout.ChildAlignment = UIChildAlignment::BottomRight;
		layout.StretchChildren = true;
		layout.CellSize = { 64.0f, 32.0f };
		layout.Columns = 3;
		UICanvasComponent& canvas = ui.Canvas.GetComponent<UICanvasComponent>();
		canvas.ScaleMode = UICanvasScaleMode::ConstantPhysicalSize;
		canvas.ReferenceResolution = { 1280.0f, 720.0f };
		canvas.MatchWidthOrHeight = 1.0f;
		canvas.ScaleFactor = 2.0f;
		canvas.SafeAreaPadding = { 8.0f, 16.0f, 8.0f, 0.0f };
		canvas.SortOrder = -3;
		canvas.PixelSnap = false;
		canvas.ReceivesInput = false;

		const nlohmann::json document = SceneSerializer::Serialize(scene);
		std::string error;
		const Ref<Scene> loaded = CreateRef<Scene>();
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(*loaded, document, &error), error);
		CHECK(SceneSerializer::Serialize(*loaded) == document);
		Tests::CheckSceneCaches(*loaded);

		const Entity slider = loaded->GetEntityByUUID(ui.Slider.GetUUID());
		REQUIRE(slider.IsValid());
		const UIImageComponent& loadedImage = slider.GetComponent<UIImageComponent>();
		CHECK(loadedImage.Type == UIImageType::Sliced);
		CHECK(loadedImage.UVRect == image.UVRect);
		CHECK(loadedImage.FillDirection == UIFillDirection::BottomToTop);
		CHECK(slider.GetComponent<UIRectComponent>().FitContent == UIFitContent::Height);
		CHECK(slider.GetComponent<UISliderComponent>().Fill == ui.Fill.GetUUID());
		const Entity handle = loaded->GetEntityByUUID(ui.Handle.GetUUID());
		CHECK(handle.GetComponent<UITextComponent>().Text == "Lives: 3\nScore: 9");
		CHECK(handle.GetComponent<UITextComponent>().Overflow == UITextOverflow::Ellipsis);
		const Entity loadedCanvas = loaded->GetEntityByUUID(ui.Canvas.GetUUID());
		CHECK(loadedCanvas.GetComponent<UICanvasComponent>().SortOrder == -3);
		CHECK(loadedCanvas.GetComponent<UILayoutComponent>().ChildAlignment == UIChildAlignment::BottomRight);
	}

	TEST_CASE("Out-of-range UI values in documents load clamped")
	{
		Scene scene("UI");
		Entity canvas = scene.CreateEntity("Canvas");
		canvas.AddComponent<UIRectComponent>();
		canvas.AddComponent<UICanvasComponent>();
		nlohmann::json document = SceneSerializer::Serialize(scene);
		nlohmann::json& components = document["Scene"]["Entities"][0]["Components"];
		components["UICanvas"]["ReferenceResolution"] = { 0.0f, 100000.0f };
		components["UICanvas"]["MatchWidthOrHeight"] = 4.0f;

		std::string error;
		std::vector<std::string> warnings;
		const Ref<Scene> loaded = CreateRef<Scene>();
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(*loaded, document, &error, &warnings), error);
		const UICanvasComponent& loadedCanvas = loaded->GetEntityByUUID(canvas.GetUUID()).GetComponent<UICanvasComponent>();
		CHECK(loadedCanvas.ReferenceResolution == glm::vec2(1.0f, 16384.0f));
		CHECK(loadedCanvas.MatchWidthOrHeight == 1.0f);
	}

	TEST_CASE("Entity references of UI components remap in duplicates and prefab instances and stay in copies")
	{
		Ref<Scene> scene = CreateRef<Scene>("UI");
		ReferencingUI ui = CreateReferencingUI(*scene);

		// A duplicate's references point at its own parts; references outside it stay.
		const Entity duplicate = scene->DuplicateEntity(ui.Canvas);
		REQUIRE(duplicate.IsValid());
		const std::vector<Entity> parts = duplicate.GetChildren();
		REQUIRE(parts.size() == 3);
		const Entity slider = parts[0];
		const std::vector<Entity> sliderParts = slider.GetChildren();
		REQUIRE(sliderParts.size() == 2);
		CHECK(slider.GetComponent<UISliderComponent>().Fill == sliderParts[0].GetUUID());
		CHECK(slider.GetComponent<UISliderComponent>().Handle == sliderParts[1].GetUUID());
		CHECK(slider.GetComponent<UISliderComponent>().EventTarget == ui.Outside.GetUUID());
		CHECK(duplicate.GetComponent<UICanvasComponent>().FirstSelected == parts[1].GetUUID());
		CHECK(parts[1].GetComponent<UISelectableComponent>().NavigateDown == parts[2].GetUUID());
		CHECK(parts[1].GetComponent<UISelectableComponent>().NavigateUp == slider.GetUUID());
		CHECK(parts[2].GetComponent<UIToggleComponent>().Group == duplicate.GetUUID());
		Tests::CheckSceneCaches(*scene);

		// Prefab instances do the same.
		const Ref<Prefab> prefab = Prefab::CreateFromEntities(*scene, { ui.Canvas });
		REQUIRE(prefab);
		Scene target("Target");
		std::string error;
		const std::vector<Entity> roots = prefab->Instantiate(target, Entity(), &error);
		REQUIRE_MESSAGE(roots.size() == 1, error);
		const std::vector<Entity> instanceParts = roots[0].GetChildren();
		REQUIRE(instanceParts.size() == 3);
		CHECK(roots[0].GetComponent<UICanvasComponent>().FirstSelected == instanceParts[1].GetUUID());
		CHECK(instanceParts[0].GetComponent<UISliderComponent>().Fill == instanceParts[0].GetChildren().at(0).GetUUID());
		CHECK(instanceParts[2].GetComponent<UIToggleComponent>().Group == roots[0].GetUUID());
		CHECK(instanceParts[2].GetComponent<UIToggleComponent>().IsOn);

		// Play mode's copy keeps every entity's ID, so its references are the same.
		const Ref<Scene> copy = Scene::Copy(scene);
		const Entity copiedButton = copy->GetEntityByUUID(ui.Button.GetUUID());
		REQUIRE(copiedButton.IsValid());
		CHECK(copiedButton.GetComponent<UISelectableComponent>().NavigateDown == ui.Toggle.GetUUID());
		CHECK(copiedButton.GetComponent<UIButtonComponent>().EventTarget == ui.Outside.GetUUID());
		Tests::CheckSceneCaches(*copy);
	}
}
