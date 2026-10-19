#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/GameUI/GameUIComponents.h"
#include "Strata/Input/Input.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/SceneSystem.h"
#include "TestHelpers.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint64_t c_Texture = 0x7E7700000000A001ull;
	constexpr uint64_t c_Mesh = 0x7E7700000000A002ull;

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

	// The scene the UI test scripts expect (StrataTests/Scripts/API/UIScripts.cpp), with a texture and a mesh asset in the
	// active asset manager. Input frames start with every Frame(), on a 1920x1080 input viewport where one canvas unit is a
	// pixel.
	struct UIScriptScene
	{
		Ref<FakeAssetManager> Assets = CreateRef<FakeAssetManager>();
		Scene Target;
		Entity Menu;
		Entity Play;
		Entity Controller;

		UIScriptScene()
		{
			Input::Reset();
			Input::SetViewport(glm::vec2(0.0f), glm::vec2(1920.0f, 1080.0f));
			AssetManager::SetActive(Assets);
			Assets->Add(c_Texture, AssetMemoryUsage {}, 0, AssetType::Texture);
			Assets->Add(c_Mesh, AssetMemoryUsage {}, 0, AssetType::Mesh);

			Menu = Target.CreateEntity("Menu");
			Menu.AddComponent<UIRectComponent>();
			Menu.AddComponent<UICanvasComponent>().ScaleMode = UICanvasScaleMode::ConstantPixelSize;
			Entity title = Add("Title", { 0.0f, 0.0f }, { 300.0f, 60.0f });
			title.AddComponent<UITextComponent>().Text = "Hello";
			Entity logo = Add("Logo", { 400.0f, 0.0f }, { 64.0f, 64.0f });
			logo.AddComponent<UIImageComponent>().Color = { 0.2f, 0.4f, 0.6f, 1.0f };
			Controller = Target.CreateEntity("Controller");
			Play = AddButton("Play", { 100.0f, 100.0f }, { 200.0f, 50.0f });
			Play.GetComponent<UIButtonComponent>().EventTarget = Controller.GetUUID();
			Entity options = Target.CreateEntity("Options");
			Entity sound = AddSelectable("Sound", { 100.0f, 200.0f }, { 40.0f, 40.0f });
			UIToggleComponent& soundToggle = sound.AddComponent<UIToggleComponent>();
			soundToggle.Group = options.GetUUID();
			soundToggle.IsOn = true;
			Entity music = AddSelectable("Music", { 200.0f, 200.0f }, { 40.0f, 40.0f });
			music.AddComponent<UIToggleComponent>().Group = options.GetUUID();
			Entity volume = AddSelectable("Volume", { 100.0f, 300.0f }, { 200.0f, 20.0f });
			UISliderComponent& slider = volume.AddComponent<UISliderComponent>();
			slider.MinValue = 0.0f;
			slider.MaxValue = 10.0f;
			slider.Value = 4.0f;
			slider.WholeNumbers = true;
			AddButton("Locked", { 400.0f, 100.0f }, { 100.0f, 50.0f }).GetComponent<UISelectableComponent>().Interactable = false;
			Entity name = AddSelectable("Name", { 100.0f, 400.0f }, { 300.0f, 40.0f });
			Entity nameText = Target.CreateChildEntity(name, "Name Text");
			nameText.AddComponent<UIRectComponent>(BoxRect({ 8.0f, 8.0f }, { 284.0f, 24.0f }));
			nameText.AddComponent<UITextComponent>().FontSize = 20.0f;
			UIInputFieldComponent& field = name.AddComponent<UIInputFieldComponent>();
			field.TextElement = nameText.GetUUID();
			field.CharacterLimit = 8;
			field.EventTarget = Controller.GetUUID();
			Target.CreateEntity("Plain");
		}

		~UIScriptScene()
		{
			if (Target.IsRunning())
				Target.OnRuntimeStop();
			AssetManager::SetActive(nullptr);
			Input::Reset();
		}

		UIScriptScene(const UIScriptScene&) = delete;
		UIScriptScene& operator=(const UIScriptScene&) = delete;

		Entity Add(const std::string& name, const glm::vec2& min, const glm::vec2& size)
		{
			Entity entity = Target.CreateChildEntity(Menu, name);
			entity.AddComponent<UIRectComponent>(BoxRect(min, size));
			return entity;
		}

		Entity AddSelectable(const std::string& name, const glm::vec2& min, const glm::vec2& size)
		{
			Entity entity = Add(name, min, size);
			entity.AddComponent<UIImageComponent>();
			entity.AddComponent<UISelectableComponent>();
			return entity;
		}

		Entity AddButton(const std::string& name, const glm::vec2& min, const glm::vec2& size)
		{
			Entity button = AddSelectable(name, min, size);
			button.AddComponent<UIButtonComponent>();
			return button;
		}

		void Frame(float deltaTime = 1.0f / 60.0f)
		{
			Input::BeginFrame();
			Target.OnUpdateRuntime(deltaTime);
		}

		void Click(const glm::vec2& position)
		{
			Input::SimulateMouseMove(position);
			Frame();
			Input::SimulateMouseButton(Mouse::ButtonLeft, true);
			Frame();
			Input::SimulateMouseButton(Mouse::ButtonLeft, false);
			Frame();
		}
	};

	// Scenes play without the game UI's system while this lives (a build without the GameUI module).
	class ScopedWithoutUISystem
	{
	public:
		ScopedWithoutUISystem()
		{
			const std::vector<SceneSystemDescriptor>& descriptors = SceneSystemRegistry::GetAll();
			const auto ui = std::find_if(descriptors.begin(), descriptors.end(), [](const SceneSystemDescriptor& descriptor) { return descriptor.Name == "UI"; });
			REQUIRE(ui != descriptors.end());
			m_Descriptor = *ui;
			for (const SceneSystemDescriptor& descriptor : descriptors)
				m_Order.push_back(descriptor.Name);
			REQUIRE(SceneSystemRegistry::Unregister("UI"));
		}

		~ScopedWithoutUISystem()
		{
			// Registered again, its constraint (before Scripting) puts it back where it was.
			CHECK(SceneSystemRegistry::Register(m_Descriptor));
			std::vector<std::string> order;
			for (const SceneSystemDescriptor& descriptor : SceneSystemRegistry::GetAll())
				order.push_back(descriptor.Name);
			CHECK(order == m_Order);
		}

		ScopedWithoutUISystem(const ScopedWithoutUISystem&) = delete;
		ScopedWithoutUISystem& operator=(const ScopedWithoutUISystem&) = delete;
	private:
		SceneSystemDescriptor m_Descriptor;
		std::vector<std::string> m_Order;
	};

	void RunUIScript(const std::string& className, int32_t minimumChecks, int32_t mode = 0)
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		UIScriptScene scene;
		ScriptEntry& entry = AddScriptEntry(scene.Controller, className);
		if (className == "UIControlAPI")
			AddFieldOverride(entry, "Texture", PropertyType::Asset, PropertyValue(UUID(c_Texture)));
		if (className == "UIMisuse")
			AddFieldOverride(entry, "NotTexture", PropertyType::Asset, PropertyValue(UUID(c_Mesh)));
		if (className == "UIUnavailable")
			AddFieldOverride(entry, "Mode", PropertyType::Int, PropertyValue(mode));
		scene.Target.OnRuntimeStart();
		scene.Frame();

		const ScriptSystem& system = GetScriptSystem(scene.Target);
		CHECK(GetField<bool>(system, scene.Controller, className, "Done"));
		CheckScriptChecks(system, scene.Controller, className, minimumChecks);
		CHECK_FALSE(engine->IsFaulted());
	}

}

TEST_SUITE("Scripting.UI")
{
	TEST_CASE("Scripts read and change texts, images, colors, interactability, values and the focus")
	{
		RunUIScript("UIControlAPI", 30);
	}

	TEST_CASE("UI functions refuse misuse harmlessly")
	{
		RunUIScript("UIMisuse", 20);
	}

	TEST_CASE("UI functions do nothing in scenes without the game UI and on older engines")
	{
		{
			ScopedWithoutUISystem withoutUI;
			RunUIScript("UIUnavailable", 3, 0);
		}
		RunUIScript("UIUnavailable", 3, 1);
	}

	TEST_CASE("Widget events reach the widget's scripts, then its event target's, once each and before OnUpdate")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		UIScriptScene scene;
		AddScriptEntry(scene.Play, "UIEventRecorder");
		AddScriptEntry(scene.Play, "UIFrameProbe");
		AddScriptEntry(scene.Controller, "UIEventRecorder");
		Entity volume = scene.Target.FindEntityByName("Volume");
		AddScriptEntry(volume, "UIEventRecorder");
		scene.Target.OnRuntimeStart();
		scene.Frame();
		const ScriptSystem& system = GetScriptSystem(scene.Target);

		scene.Click({ 150.0f, 120.0f });
		CHECK(GetField<std::string>(system, scene.Play, "UIEventRecorder", "Events") == "focus:Play+;click:Play;");
		// The target hears the widget's events too: its clicks and its focus.
		CHECK(GetField<std::string>(system, scene.Controller, "UIEventRecorder", "Events") == "focus:Play+;click:Play;");
		// Each event came before the OnUpdate of its frame.
		CHECK(GetField<int32_t>(system, scene.Play, "UIEventRecorder", "SeenInUpdate") == 2);

		// A slider changed by dragging: its value, once per change; the focus moves from Play to it.
		Input::SimulateMouseMove({ 101.0f, 310.0f });
		scene.Frame();
		Input::SimulateMouseButton(Mouse::ButtonLeft, true);
		scene.Frame();
		Input::SimulateMouseMove({ 300.0f, 310.0f });
		scene.Frame();
		scene.Frame();
		Input::SimulateMouseButton(Mouse::ButtonLeft, false);
		scene.Frame();
		CHECK(GetField<std::string>(system, volume, "UIEventRecorder", "Events") == "focus:Volume+;value:Volume=0;value:Volume=10;");
		CHECK(GetField<float>(system, volume, "UIEventRecorder", "LastValue") == 10.0f);
		CHECK(GetField<std::string>(system, scene.Play, "UIEventRecorder", "Events") == "focus:Play+;click:Play;focus:Play-;");

		// Values scripts or tools set raise nothing; a disabled widget raises nothing.
		volume.GetComponent<UISliderComponent>().Value = 5.0f;
		volume.MarkModified<UISliderComponent>();
		scene.Frame();
		scene.Click({ 450.0f, 120.0f });
		CHECK(GetField<int32_t>(system, volume, "UIEventRecorder", "Count") == 3);
		CHECK(GetField<int32_t>(system, scene.Controller, "UIEventRecorder", "Count") == 3);
		CHECK_FALSE(engine->IsFaulted());
	}

	TEST_CASE("An input field's submit reaches OnUISubmit of the field's and its target's scripts; typing is hidden from the game")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		UIScriptScene scene;
		Entity name = scene.Target.FindEntityByName("Name");
		AddScriptEntry(name, "UIEventRecorder");
		AddScriptEntry(scene.Controller, "UIEventRecorder");
		AddScriptEntry(scene.Controller, "UIFrameProbe");
		scene.Target.OnRuntimeStart();
		scene.Frame();
		const ScriptSystem& system = GetScriptSystem(scene.Target);

		scene.Click({ 200.0f, 420.0f });
		Input::SimulateText("Ada");
		scene.Frame();
		Input::SimulateKey(Key::Enter, true);
		scene.Frame();
		Input::SimulateKey(Key::Enter, false);
		scene.Frame();
		CHECK(GetField<std::string>(system, name, "UIEventRecorder", "Events") == "focus:Name+;submit:Name=Ada;");
		CHECK(GetField<std::string>(system, scene.Controller, "UIEventRecorder", "Events") == "focus:Name+;submit:Name=Ada;");
		CHECK(GetField<std::string>(system, scene.Controller, "UIFrameProbe", "Typed").empty()); // The field had the keyboard
		CHECK_FALSE(engine->IsFaulted());
	}

	TEST_CASE("Unscaled time goes on at time scale 0, and typed text reaches scripts")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		UIScriptScene scene;
		AddScriptEntry(scene.Controller, "UIFrameProbe");
		scene.Target.OnRuntimeStart();
		scene.Target.SetTimeScale(0.0f);
		scene.Frame(0.25f);
		const ScriptSystem& system = GetScriptSystem(scene.Target);
		CHECK(GetField<float>(system, scene.Controller, "UIFrameProbe", "DeltaTime") == 0.0f);
		CHECK(GetField<float>(system, scene.Controller, "UIFrameProbe", "UnscaledDeltaTime") == 0.25f);
		CHECK(GetField<float>(system, scene.Controller, "UIFrameProbe", "RealTime") == 0.25f);
		Input::SimulateText("h\xC3\xA9llo");
		scene.Frame(0.25f);
		CHECK(GetField<float>(system, scene.Controller, "UIFrameProbe", "RealTime") == 0.5f);
		CHECK(GetField<std::string>(system, scene.Controller, "UIFrameProbe", "Typed") == "h\xC3\xA9llo");
		scene.Frame(0.25f);
		CHECK(GetField<std::string>(system, scene.Controller, "UIFrameProbe", "Typed") == "h\xC3\xA9llo"); // Once
		CHECK_FALSE(engine->IsFaulted());
	}

	TEST_CASE("UI callbacks are read only from class descriptors large enough to hold them")
	{
		// Both classes have the UI callbacks, but "First" has the descriptor size of an SDK that predates them.
		ScopedEnvironmentVariable scopedCase("STRATA_TEST_MALFORMED_CASE", "OlderUIClass");
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_MALFORMED));
		const ScriptClassInfo* first = engine->FindClass("First");
		const ScriptClassInfo* second = engine->FindClass("Second");
		REQUIRE(first);
		REQUIRE(second);
		for (ScriptCallback callback : { ScriptCallback::OnUIClick, ScriptCallback::OnUIValueChanged, ScriptCallback::OnUIFocusChanged, ScriptCallback::OnUISubmit })
		{
			INFO("Callback ", ScriptCallbackToString(callback));
			CHECK_FALSE(first->Implements(callback));
			CHECK(second->Implements(callback));
		}

		// A button with both scripts is clicked: only Second hears the focus and the click (every field reads the call count).
		UIScriptScene scene;
		AddScriptEntry(scene.Play, "First");
		AddScriptEntry(scene.Play, "Second");
		scene.Target.OnRuntimeStart();
		scene.Frame();
		scene.Click({ 150.0f, 120.0f });
		CHECK(GetField<int32_t>(GetScriptSystem(scene.Target), scene.Play, "Second", "B") == 2);
		CHECK_FALSE(engine->IsFaulted());
	}

	TEST_CASE("OnUISubmit is read only from class descriptors large enough to hold it")
	{
		// "First" has the descriptor size of an SDK that has the other UI callbacks but predates OnUISubmit.
		ScopedEnvironmentVariable scopedCase("STRATA_TEST_MALFORMED_CASE", "OlderSubmitClass");
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_MALFORMED));
		const ScriptClassInfo* first = engine->FindClass("First");
		const ScriptClassInfo* second = engine->FindClass("Second");
		REQUIRE(first);
		REQUIRE(second);
		CHECK(first->Implements(ScriptCallback::OnUIFocusChanged));
		CHECK_FALSE(first->Implements(ScriptCallback::OnUISubmit));
		CHECK(second->Implements(ScriptCallback::OnUISubmit));
		CHECK_FALSE(engine->IsFaulted());
	}
}
