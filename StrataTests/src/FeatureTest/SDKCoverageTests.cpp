#include <doctest/doctest.h>

#include "FeatureTest/FeatureTestUtils.h"
#include "FeatureTest/SDKReader.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;
using namespace Strata::Tests::SDKReader;

// Checks mechanically that the feature scripts use the whole public script SDK: every public function of the SDK
// headers outside namespace Detail (called on its class, see SDKReader.h for what counts as a use), every public SDK
// macro and every script field type. The run of the feature test verifies the behavior.
namespace
{

	std::vector<std::filesystem::path> ListFiles(const std::filesystem::path& directory)
	{
		std::vector<std::filesystem::path> files;
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
		{
			const std::string extension = FileSystem::ToUTF8(it->path().extension());
			if (it->is_regular_file() && (extension == ".h" || extension == ".hpp" || extension == ".cpp"))
				files.push_back(it->path());
		}
		std::sort(files.begin(), files.end());
		return files;
	}

	SDKSurface ReadSDKSurface()
	{
		SDKSurface surface;
		const std::filesystem::path sdkDirectory = FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "StrataScriptCore" / "Include" / "StrataScript";
		const std::vector<std::filesystem::path> headers = ListFiles(sdkDirectory);
		REQUIRE(headers.size() >= 8);
		for (const std::filesystem::path& header : headers)
		{
			// The C ABI is covered by the host function call counters instead.
			if (header.filename() == "ScriptABI.h")
				continue;
			const std::optional<std::string> text = FileSystem::ReadText(header);
			REQUIRE(text.has_value());
			surface.AddHeader(*text);
		}
		return surface;
	}

	std::vector<ScriptSource> ReadFeatureScripts()
	{
		std::vector<ScriptSource> sources;
		for (const std::filesystem::path& path : ListFiles(GetFeatureProjectSourceDirectory() / "Scripts"))
		{
			const std::optional<std::string> text = FileSystem::ReadText(path);
			REQUIRE(text.has_value());
			sources.push_back({ FileSystem::ToUTF8(path.filename()), *text });
		}
		REQUIRE_FALSE(sources.empty());
		return sources;
	}

	std::set<std::string> GetQualifiedNames(const std::vector<SDKFunction>& functions)
	{
		std::set<std::string> names;
		for (const SDKFunction& function : functions)
			names.insert(function.GetQualifiedName());
		return names;
	}

	const SDKFunction* FindFreeFunction(const SDKSurface& surface, std::string_view qualifiedName)
	{
		for (const SDKFunction& function : surface.Functions)
		{
			if (function.Class.empty() && function.GetQualifiedName() == qualifiedName)
				return &function;
		}
		return nullptr;
	}

	// "File:Line" of the accesses of `name` whose receiver type the reader could not determine.
	std::string DescribeUntypedAccesses(const ScriptUsage& usage, std::string_view name)
	{
		std::string text;
		for (const UntypedAccess& access : usage.UntypedAccesses)
		{
			if (access.Name != name)
				continue;
			text += (text.empty() ? "" : ", ") + access.Source + ":" + std::to_string(access.Line);
		}
		return text.empty() ? std::string("none") : text;
	}

	// Every declaration form the reader has to understand. The public functions are named after their form.
	constexpr std::string_view c_DeclarationSample = R"sample(
#pragma once
#define ST_SCRIPT_SAMPLE(Value) (Value)
#define ST_SCRIPT_DETAIL_SAMPLE(Value) (Value)

namespace Strata
{
	class Sample
	{
	public:
		Sample();
		~Sample();
		bool operator==(const Sample& other) const = default;
		explicit operator bool() const;

		static constexpr uint64_t c_Thousand = 1'000; // A digit separator, not the start of a character literal
		void AfterDigitSeparator();
		char Quote() const { return '\''; }
		template<typename T = float> T DefaultedTemplate() const;
		template<typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0> void Constrained(T value);
		[[deprecated("use Other")]] void Deprecated();
		[[nodiscard]] static int StaticFunction();
		virtual void VirtualFunction() {}
		std::vector<Entity> VectorResult() const;
		decltype(auto) DeducedResult();
		auto TrailingResult() -> Entity;
		const char* RawString() const { return R"text(a "quoted" ; } string)text"; }
		/* void Commented(); */
		int Value = 2;
	private:
		void Private();
	};

	namespace Key
	{
		const char* GetKeyName(int key);
	}

	namespace Detail
	{
		void Internal();

		template<typename T>
		inline constexpr bool c_IsFieldType = std::is_same_v<T, bool> || std::is_same_v<T, glm::vec2> || std::is_same_v<T, Strata::Entity>
			|| std::is_same_v<T, std::string>;
	}

	void Free(int value = 1);
}
)sample";

	// An SDK and scripts using parts of it in every way the reader understands, and in ways that do not count.
	constexpr std::string_view c_UsageSDK = R"sample(
namespace Strata
{
	class Entity
	{
	public:
		bool IsValid() const;
		void Destroy();
		Component GetComponent(std::string_view name) const;
		std::vector<Entity> GetChildren() const;
		template<typename T> std::optional<T> GetProperty(std::string_view name) const;
		template<typename T> T* GetScript() const;
	};

	class Component
	{
	public:
		void Destroy();
		int Value() const;
		bool Expect(bool condition) const;
		Entity GetEntity() const;
	};

	class Script
	{
	public:
		virtual void OnCreate() {}
		virtual void OnUpdate(float deltaTime) {}
		Entity GetEntity() const;
		void Unused();
	};

	class Scene
	{
	public:
		static Entity Find(std::string_view name);
		static void Clear();
	};

	template<typename T>
	class ScriptClassBuilder
	{
	public:
		template<auto Member> ScriptClassBuilder& Field(std::string_view name);
	};

	namespace Key
	{
		const char* GetKeyName(int key);
		const char* GetOtherName(int key);
	}

	void Global();
	void GlobalUnused();
}

#define ST_SCRIPT_CLASS(Type) static void StrataScriptDescribe##Type(::Strata::ScriptClassBuilder<Type>& stScriptBuilder)
#define ST_SCRIPT_FIELD(Member) stScriptBuilder.template Field<&Member>(#Member)
)sample";

	constexpr std::string_view c_UsageScripts = R"sample(
using namespace Strata;

namespace Sample
{
	class Base : public Strata::Script
	{
	public:
		int Value = 0;               // Named like Component::Value: declarations are no use
		void Expect(bool condition); // Named like Component::Expect
	};
}

class Player : public Sample::Base
{
public:
	Entity Target;
	std::string Label;

	void OnCreate() override
	{
		Target.Destroy();
		Expect(GetEntity().IsValid());
		const std::vector<Entity> children = Target.GetChildren();
		children[0].GetComponent("Camera").GetEntity();
		auto found = Scene::Find("Name");
		found.GetProperty<Entity>("Parent").value_or(Entity()).GetScript<Player>();
		Key::GetKeyName(1);
		Global();
		Scene scene;
		scene.Clear();      // A static function called on an instance
		unknown.Clear();    // Receiver of unknown type
		Mystery().Unused(); // Receiver of unknown type
		Value = 1;
	}

	void OnUpdate(float deltaTime); // No "override"
};

ST_SCRIPT_CLASS(Player)
{
	ST_SCRIPT_FIELD(Target);
	ST_SCRIPT_FIELD(Value);
}
)sample";

}

TEST_SUITE("FeatureTest")
{
	TEST_CASE("The SDK reader understands the declaration forms of the SDK headers")
	{
		SDKSurface surface;
		surface.AddHeader(c_DeclarationSample);
		CHECK(GetQualifiedNames(surface.Functions) == std::set<std::string> { "Sample::AfterDigitSeparator", "Sample::Quote", "Sample::DefaultedTemplate",
			"Sample::Constrained", "Sample::Deprecated", "Sample::StaticFunction", "Sample::VirtualFunction", "Sample::VectorResult",
			"Sample::DeducedResult", "Sample::TrailingResult", "Sample::RawString", "Key::GetKeyName", "Free" });
		CHECK(surface.Classes == std::set<std::string> { "Sample" });
		CHECK(surface.PublicMacros == std::set<std::string> { "ST_SCRIPT_SAMPLE" });
		CHECK(surface.FieldTypes == std::vector<std::string> { "bool", "glm::vec2", "Entity", "string" });

		const SDKFunction* defaulted = surface.FindFunction("Sample", "DefaultedTemplate");
		REQUIRE(defaulted);
		CHECK(defaulted->TemplateParameters == std::vector<std::string> { "T" });
		CHECK(defaulted->ReturnType == TypeRef { TypeRef::Kind::Value, "T" });
		const SDKFunction* constrained = surface.FindFunction("Sample", "Constrained");
		REQUIRE(constrained);
		CHECK(constrained->TemplateParameters == std::vector<std::string> { "T" });
		const SDKFunction* staticFunction = surface.FindFunction("Sample", "StaticFunction");
		REQUIRE(staticFunction);
		CHECK(staticFunction->Static);
		CHECK_FALSE(staticFunction->Virtual);
		const SDKFunction* virtualFunction = surface.FindFunction("Sample", "VirtualFunction");
		REQUIRE(virtualFunction);
		CHECK(virtualFunction->Virtual);
		CHECK_FALSE(virtualFunction->Static);
		const SDKFunction* vectorResult = surface.FindFunction("Sample", "VectorResult");
		REQUIRE(vectorResult);
		CHECK(vectorResult->ReturnType == TypeRef { TypeRef::Kind::Vector, "Entity" });
		const SDKFunction* trailingResult = surface.FindFunction("Sample", "TrailingResult");
		REQUIRE(trailingResult);
		CHECK(trailingResult->ReturnType == TypeRef { TypeRef::Kind::Value, "Entity" });
		const SDKFunction* keyName = FindFreeFunction(surface, "Key::GetKeyName");
		REQUIRE(keyName);
		CHECK(keyName->Namespace == "Key");
	}

	TEST_CASE("The SDK reader counts calls on the right class only")
	{
		SDKSurface surface;
		surface.AddHeader(c_UsageSDK);
		const ScriptUsage usage = AnalyzeScripts(surface, { { "Player.cpp", std::string(c_UsageScripts) } });

		// Used: on typed receivers, qualified, inherited, overridden, through a macro.
		CHECK(usage.Functions == std::set<std::string> { "Entity::Destroy", "Script::OnCreate", "Script::GetEntity", "Entity::IsValid", "Entity::GetChildren",
			"Entity::GetComponent", "Component::GetEntity", "Scene::Find", "Entity::GetProperty", "Entity::GetScript", "Key::GetKeyName", "Global",
			"ScriptClassBuilder::Field" });
		// Not used: same names on other classes, declarations, an override without "override", a static function called on
		// an instance, calls on receivers of unknown type and functions that are not called at all.
		for (const char* unused : { "Component::Destroy", "Component::Value", "Component::Expect", "Script::OnUpdate", "Script::Unused", "Scene::Clear",
				 "Key::GetOtherName", "GlobalUnused" })
		{
			INFO("Function ", unused);
			CHECK_FALSE(usage.Functions.contains(unused));
		}

		std::set<std::string> untyped;
		for (const UntypedAccess& access : usage.UntypedAccesses)
			untyped.insert(access.Name + "@" + std::to_string(access.Line));
		CHECK(untyped == std::set<std::string> { "Clear@32", "Unused@33" });

		REQUIRE(usage.Registrations.size() == 1);
		CHECK(usage.Registrations[0].ClassName == "Player");
		CHECK(usage.Registrations[0].Fields == std::vector<std::string> { "Target", "Value" });
		const ScriptField* inherited = usage.FindField("Player", "Value");
		REQUIRE(inherited);
		CHECK(inherited->TypeSpelling == "int");
		const ScriptField* label = usage.FindField("Player", "Label");
		REQUIRE(label);
		CHECK(label->TypeSpelling == "string");
		CHECK(usage.FindField("Player", "Missing") == nullptr);
	}

	TEST_CASE("The SDK reader finds the script SDK's public API")
	{
		const SDKSurface surface = ReadSDKSurface();
		// Guards against a reader regression that would make the coverage checks below vacuous.
		CHECK(surface.Functions.size() >= 80);
		for (const char* className : { "Entity", "Component", "TransformComponent", "AssetHandle", "Assets", "Scene", "Script", "ScriptClassBuilder",
				 "Input", "Log", "Time", "RigidBody", "Physics", "AudioSource", "Audio", "Game", "Random", "Timer", "KeyRepeat" })
		{
			INFO("Class ", className);
			CHECK(surface.Classes.contains(className));
		}
		CHECK(surface.FindFunction("Entity", "GetProperty"));
		CHECK(surface.FindFunction("Scene", "Instantiate"));
		CHECK(surface.FindFunction("ScriptClassBuilder", "Field"));
		CHECK(surface.FindFunction("Input", "GetScrollDelta"));
		CHECK(surface.FindFunction("TransformComponent", "SetLocalTransform"));
		const SDKFunction* onReload = surface.FindFunction("Script", "OnReload");
		REQUIRE(onReload);
		CHECK(onReload->Virtual);
		const SDKFunction* instantiate = surface.FindFunction("Scene", "Instantiate");
		REQUIRE(instantiate);
		CHECK(instantiate->Static);
		const SDKFunction* getProperty = surface.FindFunction("Entity", "GetProperty");
		REQUIRE(getProperty);
		CHECK(getProperty->ReturnType == TypeRef { TypeRef::Kind::Optional, "T" });
		const SDKFunction* getChildren = surface.FindFunction("Entity", "GetChildren");
		REQUIRE(getChildren);
		CHECK(getChildren->ReturnType == TypeRef { TypeRef::Kind::Vector, "Entity" });
		// Constructors, operators, private members and Detail are not part of the surface.
		CHECK_FALSE(surface.FindFunction("Entity", "Entity"));
		CHECK_FALSE(surface.FindFunction("TransformComponent", "GetLocal"));
		CHECK_FALSE(surface.FindFunction("Log", "Write"));
		CHECK_FALSE(surface.Classes.contains("ClassRecord"));
		CHECK(surface.PublicMacros == std::set<std::string> { "ST_SCRIPT_CLASS", "ST_SCRIPT_FIELD" });
		for (const char* type : { "bool", "int32_t", "float", "glm::vec2", "glm::vec3", "glm::vec4", "glm::quat", "string", "Entity", "AssetHandle" })
		{
			INFO("Field type ", type);
			CHECK(std::find(surface.FieldTypes.begin(), surface.FieldTypes.end(), type) != surface.FieldTypes.end());
		}
	}

	TEST_CASE("The feature scripts use every function of the script SDK")
	{
		const SDKSurface surface = ReadSDKSurface();
		const ScriptUsage usage = AnalyzeScripts(surface, ReadFeatureScripts());

		for (const std::string& className : surface.Classes)
		{
			INFO("SDK class ", className, " is not used by the feature scripts (StrataTests/FeatureTest/Scripts)");
			CHECK(usage.Identifiers.contains(className));
		}
		for (const SDKFunction& function : surface.Functions)
		{
			const std::string name = function.GetQualifiedName();
			INFO("SDK function ", name, " is not used by the feature scripts (StrataTests/FeatureTest/Scripts). ",
				function.Static || function.Class.empty() ? "Call it qualified (" + name + ")." : "Call it on a receiver whose type is declared (see "
				"StrataTests/src/FeatureTest/SDKReader.h); calls of this name on receivers of unknown type: " + DescribeUntypedAccesses(usage, function.Name));
			CHECK(usage.Functions.contains(name));
		}
		for (const std::string& macro : surface.PublicMacros)
		{
			INFO("SDK macro ", macro, " is not used by the feature scripts (StrataTests/FeatureTest/Scripts)");
			CHECK(usage.Identifiers.contains(macro));
		}
	}

	TEST_CASE("The feature scene overrides a script field of every SDK field type")
	{
		const SDKSurface surface = ReadSDKSurface();
		REQUIRE_FALSE(surface.FieldTypes.empty());
		const ScriptUsage usage = AnalyzeScripts(surface, ReadFeatureScripts());
		FeatureProject project;
		const Ref<Scene> scene = project.LoadStartScene();

		// The C++ types (as the scripts declare them) of the registered fields the scene overrides.
		std::set<std::string> overridden;
		for (const Entity entity : scene->GetEntitiesInHierarchyOrder())
		{
			const ScriptComponent* scripts = entity.TryGetComponent<ScriptComponent>();
			if (!scripts)
				continue;
			for (const ScriptEntry& entry : scripts->Scripts)
			{
				const auto registration = std::find_if(usage.Registrations.begin(), usage.Registrations.end(),
					[&](const ScriptRegistration& candidate) { return candidate.ClassName == entry.ClassName; });
				if (registration == usage.Registrations.end())
					continue;
				// "Game::Player" is declared as class Player.
				const size_t separator = entry.ClassName.rfind("::");
				const std::string className = separator == std::string::npos ? entry.ClassName : entry.ClassName.substr(separator + 2);
				for (const ScriptFieldValue& field : entry.Fields)
				{
					if (std::find(registration->Fields.begin(), registration->Fields.end(), field.Name) == registration->Fields.end())
						continue;
					if (const ScriptField* declared = usage.FindField(className, field.Name))
						overridden.insert(declared->TypeSpelling);
				}
			}
		}

		for (const std::string& type : surface.FieldTypes)
		{
			INFO("Script field type ", type, ": declare a field of this type in a feature script (StrataTests/FeatureTest/Scripts), register it with "
				"ST_SCRIPT_FIELD and override it in StrataTests/FeatureTest/Assets/Scenes/Feature.stscene");
			CHECK(overridden.contains(type));
		}
	}
}
