#pragma once

#include <doctest/doctest.h>

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/Log.h"
#include "Strata/Project/Project.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scripting/ScriptEngine.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The feature test: the project in StrataTests/FeatureTest (a scene with every component, assets of every type and a
// script module exercising the whole script API) and the helpers its runners share. The headless runner
// (FeatureTests.cpp) and the editor runner (Editor/EditorFeatureTests.cpp, which also plays the exported game) play the
// scripted scenario of PlayFeatureScene; the GPU runner (GPUFeatureTests.cpp) only renders the scene, without scripts.
// See AGENTS.md, "Testing".
namespace Strata::Tests
{

	// Frames of a feature run (c_FeatureFrameTime seconds each) and the frame after which the script module is hot
	// reloaded. The feature scripts' scenarios (bodies falling and settling, OnReload) fit into this run.
	constexpr int32_t c_FeatureFrames = 200;
	constexpr int32_t c_FeatureReloadFrame = 100;
	constexpr float c_FeatureFrameTime = 1.0f / 60.0f;

	// StrataTests/FeatureTest in the source tree. Tests never write there: they work on copies.
	std::filesystem::path GetFeatureProjectSourceDirectory();
	// The feature test's script module, built next to the test executable.
	std::filesystem::path GetFeatureScriptModule();
	// Copies the project file and Assets/ of the feature project into `directory` (REQUIREs success) and returns the
	// copied project file.
	std::filesystem::path CopyFeatureProject(const std::filesystem::path& directory);
	// Loads every asset of the manager except the built-in ones (REQUIREs success), as a loading screen would.
	void LoadAllAssets(AssetManagerBase& manager);

	// A copy of the feature project in a temporary directory, opened and imported. While it lives, its
	// EditorAssetManager is the active asset manager and the project the active project.
	class FeatureProject
	{
	public:
		FeatureProject();
		~FeatureProject();

		FeatureProject(const FeatureProject&) = delete;
		FeatureProject& operator=(const FeatureProject&) = delete;

		const std::filesystem::path& GetDirectory() const { return m_Directory; }
		const Ref<Project>& GetProject() const { return m_Project; }
		const Ref<EditorAssetManager>& GetAssetManager() const { return m_AssetManager; }

		// The project's start scene, loaded through the asset manager (REQUIREs that it loads without warnings).
		Ref<Scene> LoadStartScene() const;
	private:
		std::filesystem::path m_Directory;
		Ref<Project> m_Project;
		Ref<EditorAssetManager> m_AssetManager;
		Ref<AssetManagerBase> m_PreviousAssetManager;
		Ref<Project> m_PreviousProject;
	};

	// Log entries (of every logger) written since construction; REQUIREs that the log buffer dropped none of them.
	class LogCapture
	{
	public:
		LogCapture();
		std::vector<LogEntry> GetEntries() const;
	private:
		uint64_t m_Start = 0;
	};

	// Lowers the script logger to Trace while it lives, so that the scripts' Trace and Info messages are captured too.
	class ScopedScriptLogLevel
	{
	public:
		ScopedScriptLogLevel();
		~ScopedScriptLogLevel();

		ScopedScriptLogLevel(const ScopedScriptLogLevel&) = delete;
		ScopedScriptLogLevel& operator=(const ScopedScriptLogLevel&) = delete;
	private:
		spdlog::level::level_enum m_Previous;
	};

	// Plays the started feature scene for c_FeatureFrames frames as the feature scripts expect: input is simulated around
	// InputFeatures.PressFrame and the module is hot reloaded after c_FeatureReloadFrame. `advanceFrame` runs one frame of
	// the application (its asset manager and scene update); the global input state is reset before and after.
	void PlayFeatureScene(Scene& scene, ScriptEngine& engine, const std::function<void()>& advanceFrame);

	// After play, before stopping: every feature script (a class with the fields Checks, Failure and Completed) completed
	// its scenario without a failed check, and the engine side agrees with what the scripts observed.
	void CheckFeatureResults(Scene& scene, const ScriptEngine& engine);

	// After CheckFeatureResults, while the scene still runs: plays GameFeatures' QuitFrame, the frame after the scenario, in
	// which it asks for scene loads and then to quit, and returns the exit code it quits with. The caller checks that the
	// owner of the scene honored the quit (the scene may have stopped then).
	int32_t PlayFeatureQuitFrame(Scene& scene, ScriptEngine& engine, const std::function<void()>& advanceFrame);
	// After stopping: the scene's journal ("<Class>.<Event>@<Entity>" entries the scripts record) shows that every class
	// of the module ran, that every script callback the engine offers was called, and the destruction and reload events.
	void CheckFeatureJournal(Scene& scene, const ScriptEngine& engine);

	// The log of `runs` feature runs: every message the feature scripts log on purpose once per run, and no other warning
	// or error except `tolerated` ones (substrings; they may appear any number of times).
	void CheckFeatureLog(const std::vector<LogEntry>& entries, int32_t runs = 1, std::span<const std::string_view> tolerated = {});

}
