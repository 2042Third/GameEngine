#pragma once

#include "Editor/EditorCommands.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Scene/Entity.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strata
{

	class EditorAssetManager;
	class EditorContext;
	class Scene;
	struct AssetManagerStats;
	struct FrameProfileSummary;
	struct JobSystemStats;

	// Helpers shared by the built-in editor commands.
	namespace CommandUtils
	{

		// JSON Schema of a parameter object: {"type": "object", "properties": {...}, "required": [...]}.
		nlohmann::json ObjectSchema(std::initializer_list<std::pair<const char*, nlohmann::json>> properties, std::vector<std::string> required = {});
		nlohmann::json StringSchema(std::string description);
		nlohmann::json BoolSchema(std::string description);
		nlohmann::json IntegerSchema(std::string description, int64_t minimum, int64_t maximum);
		nlohmann::json NumberSchema(std::string description);
		nlohmann::json Vec3Schema(std::string description);
		nlohmann::json EntitySchema(std::string description);
		nlohmann::json OptionalEntitySchema(std::string description); // Also accepts null
		nlohmann::json EntityArraySchema(std::string description);
		nlohmann::json AssetSchema(std::string description); // Handle or path relative to the asset directory
		nlohmann::json AnyObjectSchema(std::string description);

		struct FoundAsset
		{
			AssetHandle Handle;
			AssetType Type;
		};
		// An asset by reference: a handle (registered or built-in), a path relative to the asset directory, or
		// "Builtin/<Name>". Works without a project for built-in assets.
		std::optional<FoundAsset> FindAsset(std::string_view reference);

		// Resolves a string parameter with FindAsset; it must be of the expected type (None: any type).
		AssetHandle ResolveAsset(EditorContext& context, CommandArguments& arguments, std::string_view name, AssetType expectedType);

		// {"id", "name", "parent", "children": [...], "components": {...}}
		nlohmann::json DescribeEntity(Entity entity);

		// {"open": false} without a project, else {"open": true, "name", "directory", "assetDirectory", "startScene"}.
		nlohmann::json DescribeProject(const EditorContext& context);

		// Applies component values (as in scene files) to the entity, adding missing components. Validated like the
		// inspector: unknown components or properties, read-only properties, invalid values and references to missing
		// assets or entities fail (values before the failure may have been applied: roll back).
		bool ApplyComponents(Entity entity, const nlohmann::json& components, std::string* outError);

		// The totals of AssetManagerBase::GetStats as asset.stats and editor.status report them (EditorStreamingCommands.cpp).
		nlohmann::json DescribeAssetStats(const AssetManagerStats& stats);

		// Statistics of frame times in milliseconds (at least one), as editor.wait and project.export report them: {count,
		// medianMs (the mean of the two middle ones for an even count), meanMs, p95Ms (nearest rank), maxMs}.
		nlohmann::json DescribeFrameTimes(std::vector<double> frameTimes);

		// The CPU times of the frames that run while a deferred command waits (editor.wait, scene.open, project.export),
		// recorded by its poll function, which runs in the frames it measures. The storage is reserved when the command
		// starts, so recording does not allocate in those frames: an allocation there can wait for the heap while background
		// threads free a scene (the play copy play.stop lets go of), 0.6-2.7 ms in 7 of 10 runs of PerfGPU.Editor's scene
		// opened over a million entities, when the frames take 0.6. A command that outlasts its reserve grows the storage as
		// vectors grow, which allocates in that frame. Copies of the poll function share the storage.
		class FrameTimeRecorder
		{
		public:
			// Records the frames from the editor's recorded frame `firstMeasured` on (EditorContext::GetRecordedFrameCount),
			// with room for `expectedFrames`.
			FrameTimeRecorder(uint64_t firstMeasured, size_t expectedFrames);

			// Records the latest frame the editor recorded, if it is measured and new (call it once per poll).
			void Update(const EditorContext& context);
			bool IsEmpty() const { return m_FrameTimes->empty(); }
			size_t GetCount() const { return m_FrameTimes->size(); }
			size_t GetCapacity() const { return m_FrameTimes->capacity(); }
			// DescribeFrameTimes of the recorded frames (at least one).
			nlohmann::json Describe() const;
		private:
			std::shared_ptr<std::vector<double>> m_FrameTimes;
			uint64_t m_FirstMeasured = 0;
			uint64_t m_LastRecorded = 0;
		};
		// Room for the frames of a scene.open or project.export (256 KiB): the longest opens PerfGPU.Editor measures, of its
		// million-entity scenes, record 10,000-13,000 frames at the rate of an editor with pending commands.
		constexpr size_t c_FrameTimeReserve = 32768;

		// A frame profile as editor.perf reports it (EditorStateCommands.cpp): {"frames", "frame", "framesPerSecond", "stageSum",
		// "untimed", "jobs", "stages": {"<name>": {"avgMs", "p50Ms", "p95Ms", "maxMs", "inFrameTime"}}}, statistics in
		// milliseconds; `jobs` adds the job system's current running and queued jobs.
		nlohmann::json DescribeFrameProfile(const FrameProfileSummary& summary, const JobSystemStats& jobs);

		// The "force" parameter of the commands that replace the edited scene (see CheckUnsavedChanges).
		nlohmann::json ForceSchema();
		// The guard of every command that replaces the edited scene (scene.open, scene.new, project.open, project.create,
		// project.close, project.openSample): while the edited scene has unsaved changes (also while a copy of it plays: the
		// edited scene is what is at stake), the command fails, changing nothing, unless `force` is true. `action` says what the
		// command would do, e.g. "opening the scene 'Level2'". Returns the failure (naming the scene and how to proceed), or
		// nullopt when the command may go on. A forced discard keeps the changes as recovery data (EditorContext::ResetScene).
		std::optional<EditorCommandResult> CheckUnsavedChanges(const EditorContext& context, bool force, std::string_view action);
		// A command handler that runs `handler` once no save is in flight (EditorContext::HasSavesInFlight): at once when none
		// is, otherwise deferred until the saves are published (then it runs as the command's poll and decides as it would
		// have, CheckUnsavedChanges included). The commands that replace the edited scene, close the project or quit the editor
		// use it, so that a save that runs is never lost and `force` never discards one, and scene.save {wait: false} followed
		// by scene.open needs no force.
		EditorCommandHandler AfterSaves(EditorCommandHandler handler);

		// A file a command writes for its client (e.g. a capture). Relative paths resolve against the project directory
		// and fail without a project (the editor's working directory means nothing to a client); absolute paths stay.
		// Network (UNC) and device paths, drive-relative paths and reserved device names are refused, the extension must
		// match (".png"), and an existing file is replaced only with `overwrite`; anything but a regular file never is.
		std::optional<std::filesystem::path> ResolveOutputPath(const EditorContext& context, std::string_view path, std::string_view extension, bool overwrite,
			std::string* outError);

		// A free path "<folder>/<name><extension>" in the asset directory for an asset named after something people named (an
		// entity): characters file names cannot hold become '_', a name that still is no valid file name (empty, a device name
		// such as CON) becomes `fallback`, and " (2)", " (3)"... follow it while a file has the name. Folder: '/'-separated,
		// relative to the asset directory ("" for the directory itself).
		std::string MakeUniqueAssetPath(const EditorAssetManager& assets, std::string_view folder, std::string_view name, std::string_view extension,
			std::string_view fallback);

	}

}
