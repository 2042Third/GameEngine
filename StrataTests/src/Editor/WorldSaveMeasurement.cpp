#include "Editor/WorldSaveMeasurement.h"

#include "Editor/DocumentSaver.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Process.h>
#include <Strata/Reflection/PropertyJson.h>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace Strata::Tests
{

	namespace
	{

		constexpr double c_Megabyte = 1024.0 * 1024.0;
		// How long the world's open and the save may take before the measurement gives up (the 1M world opens in seconds).
		constexpr std::chrono::minutes c_StepTimeout { 10 };
		// The helper process's limit: the project's open, the world's and the save.
		constexpr std::chrono::minutes c_HelperTimeout { 25 };

		double MillisecondsSince(std::chrono::steady_clock::time_point start)
		{
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		}

		// Samples the process's private memory every millisecond on a thread of its own until it is destroyed: the largest
		// value seen. A spike shorter than the interval is missed; the kernel's peak, where it keeps one, covers that.
		class PrivateMemorySampler
		{
		public:
			PrivateMemorySampler()
			{
				m_Thread = std::thread([this]()
				{
					while (!m_Stop.load())
					{
						if (const std::optional<ProcessMemoryInfo> memory = Platform::GetProcessMemory())
						{
							uint64_t largest = m_Largest.load();
							while (memory->PrivateBytes > largest && !m_Largest.compare_exchange_weak(largest, memory->PrivateBytes))
							{
							}
						}
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
					}
				});
			}

			~PrivateMemorySampler()
			{
				m_Stop.store(true);
				m_Thread.join();
			}

			PrivateMemorySampler(const PrivateMemorySampler&) = delete;
			PrivateMemorySampler& operator=(const PrivateMemorySampler&) = delete;

			uint64_t GetLargest() const { return m_Largest.load(); }
		private:
			std::atomic<bool> m_Stop = false;
			std::atomic<uint64_t> m_Largest = 0;
			std::thread m_Thread;
		};

		// Runs frames (with a millisecond between them, for the work of other threads) until the condition holds.
		bool FramesUntil(HarnessEditor& editor, const std::function<bool()>& condition, std::chrono::steady_clock::duration timeout)
		{
			const auto deadline = std::chrono::steady_clock::now() + timeout;
			while (!condition())
			{
				if (std::chrono::steady_clock::now() > deadline)
					return false;
				editor.Frames(1);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return true;
		}

		nlohmann::json ToJson(const WorldSaveMeasurement& measurement)
		{
			return {
				{ "Entities", measurement.Entities },
				{ "Bytes", measurement.Bytes },
				{ "OpenSeconds", measurement.OpenSeconds },
				{ "SaveFrameMs", measurement.SaveFrameMs },
				{ "MaxFrameMs", measurement.MaxFrameMs },
				{ "Frames", measurement.Frames },
				{ "Seconds", measurement.Seconds },
				{ "PrivateMBBefore", measurement.PrivateMBBefore },
				{ "PeakPrivateIncreaseMB", measurement.PeakPrivateIncreaseMB } };
		}

		std::optional<WorldSaveMeasurement> FromJson(const nlohmann::json& json)
		{
			if (!json.is_object())
				return std::nullopt;
			WorldSaveMeasurement measurement;
			const auto read = [&json](const char* key, auto& value)
			{
				const auto entry = json.find(key);
				if (entry == json.end() || !entry->is_number())
					return false;
				value = entry->get<std::remove_reference_t<decltype(value)>>();
				return true;
			};
			if (!read("Entities", measurement.Entities) || !read("Bytes", measurement.Bytes) || !read("OpenSeconds", measurement.OpenSeconds)
				|| !read("SaveFrameMs", measurement.SaveFrameMs) || !read("MaxFrameMs", measurement.MaxFrameMs) || !read("Frames", measurement.Frames)
				|| !read("Seconds", measurement.Seconds) || !read("PrivateMBBefore", measurement.PrivateMBBefore)
				|| !read("PeakPrivateIncreaseMB", measurement.PeakPrivateIncreaseMB))
			{
				return std::nullopt;
			}
			return measurement;
		}

	}

	std::optional<WorldSaveMeasurement> MeasureWorldSave(const std::filesystem::path& projectDirectory, AssetHandle scene, std::string& error)
	{
		EditorOptions options;
		options.ProjectPath = projectDirectory;
		HarnessEditor editor({}, options);
		EditorContext& context = editor.Context();
		EditorCommandRunner& runner = editor.Layer->GetCommandRunner();
		const EditorCommandRegistry& commands = editor.Layer->GetCommands();
		editor.Frames(2);
		if (!context.HasProject())
		{
			error = "The project did not open";
			return std::nullopt;
		}
		if (!FramesUntil(editor, [&]() { return !context.IsSceneLoading(); }, c_StepTimeout))
		{
			error = "The project's start scene did not open";
			return std::nullopt;
		}

		// The world, opened as the person opens it.
		WorldSaveMeasurement measurement;
		std::optional<EditorCommandResult> opened;
		const auto openStart = std::chrono::steady_clock::now();
		runner.Run(context, commands, "scene.open", { { "scene", UUIDToJson(scene) } }, [&opened](const EditorCommandResult& result) { opened = result; });
		if (!FramesUntil(editor, [&]() { return opened.has_value(); }, c_StepTimeout) || !opened->Success)
		{
			error = opened ? "scene.open failed: " + opened->Error : std::string("scene.open did not finish");
			return std::nullopt;
		}
		measurement.OpenSeconds = MillisecondsSince(openStart) / 1000.0;

		// One edit, then frames until the editor is idle again (the open's leftovers released, the journal written).
		const EditorCommandResult edited = commands.Execute(context, "entity.create", { { "name", "Saved Edit" } });
		if (!edited.Success)
		{
			error = "entity.create failed: " + edited.Error;
			return std::nullopt;
		}
		const auto settle = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		FramesUntil(editor, [&]() { return std::chrono::steady_clock::now() > settle; }, std::chrono::seconds(10));
		context.GetRecovery().Wait();

		const std::optional<ProcessMemoryInfo> before = Platform::GetProcessMemory();
		if (!before)
		{
			error = "The process's memory is unknown";
			return std::nullopt;
		}
		std::optional<uint64_t> peakPrivate;
		{
			const PrivateMemorySampler sampler;
			// The save starts inside a frame, between the editor's update and its UI, as Ctrl+S does.
			std::optional<EditorCommandResult> started;
			const auto saveStart = std::chrono::steady_clock::now();
			editor.FrameWith([&]()
			{
				runner.Run(context, commands, "scene.save", { { "wait", false } }, [&started](const EditorCommandResult& result) { started = result; },
					CommandOrigin { CommandSource::UI });
			});
			measurement.SaveFrameMs = MillisecondsSince(saveStart);
			measurement.MaxFrameMs = measurement.SaveFrameMs;
			measurement.Frames = 1;
			if (!started || !started->Success || !started->Value.contains("saveId"))
			{
				error = started ? "scene.save failed: " + started->Error : std::string("scene.save did not answer at once");
				return std::nullopt;
			}
			const uint64_t id = started->Value["saveId"].get<uint64_t>();
			const DocumentSaver& saver = context.GetDocumentSaver();
			const auto deadline = std::chrono::steady_clock::now() + c_StepTimeout;
			std::optional<DocumentSaveStatus> status = saver.GetStatus(id);
			while (status && !status->IsFinished())
			{
				if (std::chrono::steady_clock::now() > deadline)
				{
					error = "The save did not finish";
					return std::nullopt;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				const auto frameStart = std::chrono::steady_clock::now();
				editor.Frames(1);
				measurement.MaxFrameMs = std::max(measurement.MaxFrameMs, MillisecondsSince(frameStart));
				measurement.Frames++;
				status = saver.GetStatus(id);
			}
			measurement.Seconds = MillisecondsSince(saveStart) / 1000.0;
			if (!status || status->State != DocumentSaveState::Done || status->Items.empty())
			{
				error = status ? "The save failed: " + status->Error : std::string("The save's status is gone");
				return std::nullopt;
			}
			measurement.Entities = status->Items.front().Entities;
			measurement.Bytes = status->Items.front().Bytes;
			peakPrivate = sampler.GetLargest();
		}
		if (context.IsSceneModified())
		{
			error = "The scene is still modified after its save";
			return std::nullopt;
		}

		// The kernel's peak, where it keeps one, holds what the sampler missed when it rose during the save.
		if (const std::optional<ProcessMemoryInfo> after = Platform::GetProcessMemory(); after && after->PeakPrivateBytes > before->PeakPrivateBytes)
			peakPrivate = std::max(*peakPrivate, after->PeakPrivateBytes);
		measurement.PrivateMBBefore = static_cast<double>(before->PrivateBytes) / c_Megabyte;
		measurement.PeakPrivateIncreaseMB = static_cast<double>(*peakPrivate > before->PrivateBytes ? *peakPrivate - before->PrivateBytes : 0) / c_Megabyte;
		return measurement;
	}

	std::optional<WorldSaveMeasurement> MeasureWorldSaveInHelper(const std::filesystem::path& projectDirectory, AssetHandle scene, std::string& error)
	{
		const std::filesystem::path resultFile = CreateTemporaryDirectory("WorldSaveResult") / "Result.json";
		ProcessSpecification specification;
		specification.Executable = GetTestExecutablePath();
		specification.Arguments = { "--strata-test-helper=save-world", FileSystem::ToUTF8(projectDirectory), scene.ToString(), FileSystem::ToUTF8(resultFile) };
		const Process::RunResult run = Process::Run(specification, c_HelperTimeout);
		if (!run.Started || run.TimedOut || run.ExitCode != 0)
		{
			error = fmt::format("The save-world helper {} (exit code {}): {}{}", run.TimedOut ? "timed out" : "failed", run.ExitCode, run.Error, run.Output);
			return std::nullopt;
		}
		const std::optional<std::string> text = FileSystem::ReadText(resultFile);
		const std::optional<nlohmann::json> json = text ? JsonUtils::Parse(*text, &error) : std::nullopt;
		std::optional<WorldSaveMeasurement> measurement = json ? FromJson(*json) : std::nullopt;
		if (!measurement)
			error = "The save-world helper's result is unreadable: " + error;
		return measurement;
	}

	std::optional<int> RunWorldSaveHelper(std::string_view mode, int argc, char** argv)
	{
		if (mode != "save-world")
			return std::nullopt;
		const std::optional<UUID> scene = argc == 5 ? UUID::FromString(argv[3]) : std::nullopt;
		if (!scene)
		{
			std::fprintf(stderr, "Usage: --strata-test-helper=save-world <project directory> <scene handle> <result file>\n");
			return 2;
		}
		LogSpecification logSpecification;
		logSpecification.Level = LogLevel::Warn;
		Log::Init(logSpecification);
		// A measurement runs at full speed with the application's pools, like the editor it stands for.
		Platform::DisableExecutionSpeedThrottling();
		JobSystem::Init();
		std::string error;
		const std::optional<WorldSaveMeasurement> measurement = MeasureWorldSave(FileSystem::FromUTF8(argv[2]), *scene, error);
		JobSystem::Shutdown();
		Log::Shutdown();
		if (!measurement)
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		if (!FileSystem::WriteText(FileSystem::FromUTF8(argv[4]), JsonUtils::Dump(ToJson(*measurement))))
		{
			std::fprintf(stderr, "Could not write '%s'\n", argv[4]);
			return 1;
		}
		return 0;
	}

}
