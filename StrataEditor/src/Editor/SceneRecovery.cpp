#include "Editor/SceneRecovery.h"

#include <Strata/Core/Crypto.h>
#include <Strata/Core/FileReader.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/SceneSerializer.h>

#include <algorithm>
#include <exception>
#include <fstream>
#include <span>
#include <system_error>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		constexpr const char* c_Format = "SceneRecovery";
		constexpr uint64_t c_FormatVersion = 1;
		// The header is the file's first line: anything longer is not one.
		constexpr size_t c_MaxHeaderBytes = 64 * 1024;
		// What HashFile reads at a time.
		constexpr uint64_t c_HashChunkBytes = 1024 * 1024;

		int64_t GetUnixMilliseconds()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		}

		std::string HashBytes(std::span<const uint8_t> bytes)
		{
			return Crypto::ToHex(Sha256::Hash(bytes));
		}

		// Hashed a piece at a time: a scene file can be hundreds of megabytes (17 MB per 100,000 entities), which the hash never
		// needs at once, and every scene the editor opens is hashed (on an I/O thread, while frames go on).
		std::optional<std::string> HashFile(const std::filesystem::path& path)
		{
			const Scope<FileReader> reader = FileReader::Open(path);
			if (!reader)
				return std::nullopt;
			Sha256 hash;
			std::vector<uint8_t> chunk(static_cast<size_t>(std::min<uint64_t>(reader->GetSize(), c_HashChunkBytes)));
			for (uint64_t offset = 0; offset < reader->GetSize(); offset += chunk.size())
			{
				const std::span<uint8_t> piece = std::span<uint8_t>(chunk).first(static_cast<size_t>(std::min<uint64_t>(chunk.size(), reader->GetSize() - offset)));
				if (!reader->ReadAt(offset, piece))
					return std::nullopt;
				hash.Update(std::span<const uint8_t>(piece));
			}
			return Crypto::ToHex(hash.Finalize());
		}

		double SecondsBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
		{
			return std::chrono::duration<double>(to - from).count();
		}

		// Whether applying `states` over `prior` creates, removes or moves an entity (also into another cell of a world). Steps
		// that do not can be folded into the step before them in a journal: positions recorded there stay what they were.
		bool IsStructural(const std::vector<EntityState>& states, const std::vector<EntityState>* prior)
		{
			if (!prior || prior->size() != states.size())
				return true;
			for (size_t index = 0; index < states.size(); index++)
			{
				const EntityState& after = states[index];
				const EntityState& before = (*prior)[index];
				if (after.ID != before.ID || after.Exists != before.Exists)
					return true;
				if (after.Exists && (after.Parent != before.Parent || after.SiblingIndex != before.SiblingIndex || after.Cell != before.Cell))
					return true;
			}
			return false;
		}

		void AppendState(std::string& out, const EntityState& state)
		{
			out += "{\"ID\":";
			out += JsonUtils::Dump(UUIDToJson(state.ID));
			out += ",\"Exists\":";
			out += state.Exists ? "true" : "false";
			if (state.Exists)
			{
				out += ",\"Components\":";
				out += JsonUtils::Dump(state.Components);
				out += ",\"Parent\":";
				out += state.Parent.IsValid() ? JsonUtils::Dump(UUIDToJson(state.Parent)) : std::string("null");
				out += ",\"SiblingIndex\":";
				out += std::to_string(state.SiblingIndex);
			}
			if (state.Cell.IsValid())
			{
				out += ",\"Cell\":";
				out += JsonUtils::Dump(UUIDToJson(state.Cell));
			}
			out += '}';
		}

		std::optional<EntityState> ParseState(nlohmann::json& json, std::string& outError)
		{
			if (!json.is_object())
			{
				outError = "an entity state is not an object";
				return std::nullopt;
			}
			EntityState state;
			const nlohmann::json* id = JsonUtils::Find(json, "ID");
			const std::optional<UUID> uuid = id ? UUIDFromJson(*id) : std::nullopt;
			const nlohmann::json* exists = JsonUtils::Find(json, "Exists");
			if (!uuid || !uuid->IsValid() || !exists || !exists->is_boolean())
			{
				outError = "an entity state lacks a valid 'ID' or 'Exists'";
				return std::nullopt;
			}
			state.ID = *uuid;
			state.Exists = exists->get<bool>();
			// Journals of a world's edits name the cells of their entities (also of those that do not exist: the cell they left).
			if (const nlohmann::json* cell = JsonUtils::Find(json, "Cell"))
			{
				const std::optional<UUID> cellId = UUIDFromJson(*cell);
				if (!cellId)
				{
					outError = fmt::format("the state of entity {} has an invalid 'Cell'", state.ID.ToString());
					return std::nullopt;
				}
				state.Cell = *cellId;
			}
			if (!state.Exists)
				return state;
			const auto components = json.find("Components");
			const nlohmann::json* parent = JsonUtils::Find(json, "Parent");
			const nlohmann::json* sibling = JsonUtils::Find(json, "SiblingIndex");
			const std::optional<UUID> parentId = parent && !parent->is_null() ? UUIDFromJson(*parent) : std::optional<UUID>(UUID::Null());
			if (components == json.end() || !components->is_object() || !parentId || !sibling || !sibling->is_number_unsigned())
			{
				outError = fmt::format("the state of entity {} lacks valid 'Components', 'Parent' or 'SiblingIndex'", state.ID.ToString());
				return std::nullopt;
			}
			state.Components = std::move(*components);
			state.Parent = *parentId;
			state.SiblingIndex = static_cast<size_t>(sibling->get<uint64_t>());
			return state;
		}

		// The file's header (its first line), or nullopt when the file cannot be read or does not start with one.
		std::optional<nlohmann::json> ReadHeader(const std::filesystem::path& path)
		{
			std::ifstream stream(path, std::ios::binary);
			if (!stream)
				return std::nullopt;
			std::string line;
			char character = 0;
			while (stream.get(character) && character != '\n')
			{
				if (line.size() >= c_MaxHeaderBytes)
					return std::nullopt;
				line += character;
			}
			std::optional<nlohmann::json> header = JsonUtils::Parse(line);
			if (!header || !header->is_object())
				return std::nullopt;
			const nlohmann::json* strata = JsonUtils::Find(*header, "Strata");
			if (!strata || JsonUtils::GetString(*strata, "Format") != c_Format || JsonUtils::GetUInt(*strata, "Version", 0) != c_FormatVersion)
				return std::nullopt;
			return header;
		}

		RecoveryEntry ReadEntry(const nlohmann::json& header, std::string id)
		{
			RecoveryEntry entry;
			entry.Id = std::move(id);
			if (const nlohmann::json* scene = JsonUtils::Find(header, "Scene"); scene && !scene->is_null())
				entry.Scene = UUIDFromJson(*scene).value_or(UUID::Null());
			entry.SceneName = JsonUtils::GetString(header, "SceneName", "Untitled");
			entry.ScenePath = JsonUtils::GetString(header, "ScenePath");
			entry.Time = JsonUtils::GetInt(header, "Time", 0);
			entry.Discarded = JsonUtils::GetBool(header, "Discarded", false);
			entry.EntityCount = JsonUtils::GetUInt(header, "Entities", 0);
			entry.Complete = JsonUtils::GetBool(header, "Complete", false);
			return entry;
		}

		std::optional<std::string> ReadBaseHash(const nlohmann::json& header)
		{
			const nlohmann::json* hash = JsonUtils::Find(header, "BaseHash");
			return hash && hash->is_string() ? std::optional<std::string>(hash->get<std::string>()) : std::nullopt;
		}

		// Whether a scene file is the one the recovery data applies to; the problem otherwise.
		std::string CheckSceneFile(const std::filesystem::path& sceneFile, const std::string& scenePath, const std::optional<std::string>& baseHash)
		{
			const std::optional<std::string> current = HashFile(sceneFile);
			if (!current)
				return fmt::format("The scene file '{}' is missing", scenePath);
			if (!baseHash)
				return "It does not record which version of the scene file it applies to";
			if (*current != *baseHash)
				return fmt::format("The scene file '{}' changed after these changes were made (it was saved since, or changed outside this editor)", scenePath);
			return {};
		}

		bool WriteFileReplacing(const std::filesystem::path& path, const std::string& text, std::string& outError)
		{
			if (!FileSystem::CreateDirectories(path.parent_path()))
			{
				outError = fmt::format("cannot create '{}'", FileSystem::ToUTF8(path.parent_path()));
				return false;
			}
			// A crash while writing leaves the previous version: the new one replaces it only once it is complete.
			std::filesystem::path temporary = path;
			temporary += ".tmp";
			if (!FileSystem::WriteText(temporary, text))
			{
				outError = fmt::format("cannot write '{}'", FileSystem::ToUTF8(temporary));
				return false;
			}
			if (!FileSystem::Rename(temporary, path))
			{
				FileSystem::Remove(temporary);
				outError = fmt::format("cannot replace '{}'", FileSystem::ToUTF8(path));
				return false;
			}
			return true;
		}

		// Keeps the newest `keep` files of a directory.
		void PruneOldestFiles(const std::filesystem::path& directory, size_t keep)
		{
			std::vector<std::pair<int64_t, std::filesystem::path>> files;
			std::error_code error;
			for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
			{
				if (it->is_regular_file(error))
					files.emplace_back(FileSystem::GetLastWriteTime(it->path()).value_or(0), it->path());
			}
			if (files.size() <= keep)
				return;
			std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			for (size_t index = keep; index < files.size(); index++)
				FileSystem::Remove(files[index].second);
		}

		// Moves a recovery file (by its id) to Recovery/Discarded, where it is not offered, and keeps the newest files there.
		bool MoveToDiscarded(const std::filesystem::path& directory, const std::string& id)
		{
			const std::filesystem::path discarded = directory / SceneRecovery::c_DiscardedDirectoryName;
			const std::filesystem::path file = directory / FileSystem::FromUTF8(id + SceneRecovery::c_FileExtension);
			if (!FileSystem::CreateDirectories(discarded) || !FileSystem::Rename(file, FileSystem::GetUniquePath(discarded / file.filename())))
			{
				ST_WARN("Recovery: '{}' could not be moved to '{}'", FileSystem::ToUTF8(file), FileSystem::ToUTF8(discarded));
				return false;
			}
			PruneOldestFiles(discarded, SceneRecovery::c_MaxDiscardedFiles);
			return true;
		}

		// Of the changes discarded on purpose, the newest `keep` stay on offer (those that still apply first); the others
		// move to Recovery/Discarded. What a crash left stays whatever its number.
		void RetireDiscards(const std::filesystem::path& directory, std::vector<RecoveryEntry>& entries, size_t keep)
		{
			std::vector<const RecoveryEntry*> discards;
			for (const RecoveryEntry& entry : entries)
			{
				if (entry.Discarded)
					discards.push_back(&entry);
			}
			if (discards.size() <= keep)
				return;
			std::stable_sort(discards.begin(), discards.end(), [](const RecoveryEntry* a, const RecoveryEntry* b)
			{
				if (a->Applicable != b->Applicable)
					return a->Applicable;
				return a->Time > b->Time;
			});
			std::unordered_set<std::string> retired;
			for (size_t index = keep; index < discards.size(); index++)
			{
				if (MoveToDiscarded(directory, discards[index]->Id))
					retired.insert(discards[index]->Id);
			}
			std::erase_if(entries, [&retired](const RecoveryEntry& entry) { return retired.contains(entry.Id); });
		}

		// Every entity of a scene, in hierarchy order: a complete journal's first step.
		Ref<const std::vector<EntityState>> CaptureScene(const Scene& scene)
		{
			std::vector<UUID> ids;
			ids.reserve(scene.GetEntityCount());
			for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
				ids.push_back(entity.GetUUID());
			return CreateRef<const std::vector<EntityState>>(SceneEdit::CaptureEntities(scene, ids));
		}

		std::string MakeJournalStem(AssetHandle scene)
		{
			// The session part keeps every journal of a scene apart: one discarded earlier is never overwritten.
			return fmt::format("{}-{}", scene.IsValid() ? scene.ToString() : std::string("Untitled"), UUID().ToString());
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// SceneRecoveryAction
	////////////////////////////////////////////////////////////////////////////////

	SceneRecoveryAction::SceneRecoveryAction(Scene& scene, std::vector<EntityState> before, std::vector<EntityState> after,
		const SceneSettings& settingsBefore, const SceneSettings& settingsAfter)
		: m_Scene(scene), m_SettingsBefore(settingsBefore), m_SettingsAfter(settingsAfter)
	{
		SceneEdit::LinkCells(before, after);
		m_Before = CreateRef<const std::vector<EntityState>>(std::move(before));
		m_After = CreateRef<const std::vector<EntityState>>(std::move(after));
	}

	const std::string& SceneRecoveryAction::GetName() const
	{
		static const std::string s_Name = "Recover Unsaved Changes";
		return s_Name;
	}

	bool SceneRecoveryAction::Execute(std::string* outError)
	{
		if (!SceneEdit::CheckCells(m_Scene, *m_After, outError))
			return false;
		SceneEdit::ApplyEntities(m_Scene, *m_After);
		m_Scene.GetSettings() = m_SettingsAfter;
		return true;
	}

	bool SceneRecoveryAction::Undo(std::string* outError)
	{
		if (!SceneEdit::CheckCells(m_Scene, *m_Before, outError))
			return false;
		SceneEdit::ApplyEntities(m_Scene, *m_Before);
		m_Scene.GetSettings() = m_SettingsBefore;
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// SceneRecovery
	////////////////////////////////////////////////////////////////////////////////

	SceneRecovery::SceneRecovery()
		: m_LastWriteTime(CreateRef<std::atomic<int64_t>>(0))
	{
		m_ProcessId = Platform::GetProcessID();
		m_ProcessStartTime = Platform::GetProcessStartTime(m_ProcessId).value_or(0);
		m_LastChange = m_LastWrite = std::chrono::steady_clock::now();
	}

	SceneRecovery::~SceneRecovery()
	{
		Wait();
	}

	void SceneRecovery::SetProject(const std::filesystem::path& recoveryDirectory, const std::filesystem::path& assetDirectory)
	{
		m_Directory = recoveryDirectory;
		m_AssetDirectory = assetDirectory;
		m_Scan.reset();
		m_RescanDue = false;
		m_Entries.clear();
		m_EntriesVersion++;
		m_DiscardedHere.clear();
		if (!IsActive())
		{
			m_PendingSteps.clear();
			m_Changed = false;
		}
	}

	nlohmann::json SceneRecovery::SerializeSettings(const SceneSettings& settings)
	{
		// The scene file's own form, through the scene serializer, so settings added later are journaled without changes here.
		Scene scene;
		scene.GetSettings() = settings;
		const nlohmann::json document = SceneSerializer::Serialize(scene);
		const nlohmann::json* sceneJson = JsonUtils::Find(document, "Scene");
		const nlohmann::json* serialized = sceneJson ? JsonUtils::Find(*sceneJson, "Settings") : nullptr;
		return serialized ? *serialized : nlohmann::json::object();
	}

	bool SceneRecovery::DeserializeSettings(const nlohmann::json& json, SceneSettings& settings)
	{
		if (!json.is_object())
			return false;
		Scene scene;
		scene.GetSettings() = settings;
		const nlohmann::json document = {
			{ "Strata", { { "Format", "Scene" }, { "Version", SceneSerializer::c_FormatVersion } } },
			{ "Scene", { { "Name", "Recovery" }, { "Settings", json } } } };
		std::vector<std::string> warnings;
		if (!SceneSerializer::Deserialize(scene, document, nullptr, &warnings))
			return false;
		settings = scene.GetSettings();
		return true;
	}

	DiscardOutcome SceneRecovery::EndScene(const Scene& scene, bool modified, std::string* outReason)
	{
		DiscardOutcome outcome = modified ? DiscardOutcome::Lost : DiscardOutcome::Unmodified;
		if (IsActive() && m_Journal)
		{
			// Written again even when nothing changed since the last write: marked discarded, it is offered from now on.
			if (modified && Write(scene, true, true))
			{
				outcome = DiscardOutcome::Kept;
				m_RescanDue = true;
				m_DiscardedHere.insert(m_Journal->Stem);
			}
			else if (modified)
			{
				ST_WARN("The unsaved changes of '{}' were discarded without recovery data: {}", scene.GetName(), m_Skipped);
				if (outReason)
					*outReason = fmt::format("they could not be journaled: {}", m_Skipped);
			}
			else if (m_Changed)
			{
				Write(scene, false, false);
			}
		}
		else if (modified && outReason)
		{
			*outReason = IsActive() ? "the scene's changes were not journaled" : "no project is open to keep them in";
		}
		m_Journal.reset();
		m_PendingSteps.clear();
		m_Changed = false;
		return outcome;
	}

	void SceneRecovery::BeginScene(const Scene& scene, AssetHandle handle, const std::filesystem::path& sceneFile, const std::string& scenePath)
	{
		m_Journal = CreateRef<Journal>();
		m_Journal->Stem = MakeJournalStem(handle);
		m_Journal->Scene = handle;
		m_Journal->ScenePath = handle.IsValid() ? scenePath : std::string();
		m_Journal->SceneFile = handle.IsValid() ? sceneFile : std::filesystem::path();
		m_ScenePath = m_Journal->ScenePath;
		m_PendingSteps.clear();
		m_Changed = false;
		m_NeedsSnapshot = false;
		m_Unjournaled = false;
		m_Skipped.clear();
		m_Writes = 0;
		m_LastChange = m_LastWrite = std::chrono::steady_clock::now();
		if (!handle.IsValid())
		{
			// No file to apply changes to: the journal starts from what the scene holds now (a template's few entities, or
			// nothing), captured once, and the edits follow as steps.
			m_Journal->Complete = true;
			if (scene.GetEntityCount() > c_MaxSnapshotEntities)
			{
				m_Unjournaled = true;
				m_Skipped = fmt::format("it was never saved and started with {} entities, more than the {} a snapshot may hold without holding up a "
					"frame", scene.GetEntityCount(), c_MaxSnapshotEntities);
			}
			else
			{
				m_Journal->Base = CaptureScene(scene);
			}
			return;
		}
		if (sceneFile.empty())
			return;
		// The version of the scene file the changes will apply to, hashed right away: the file could change on disk later. A
		// scene moved before this runs (it waits behind a discard's write, or a script moves the scene right after opening it)
		// is no longer here: the move's operation notes it at the new place (OnSceneMoved), and a write of a journal that
		// still has none says so.
		Enqueue([journal = m_Journal]()
		{
			journal->BaseHash = HashFile(journal->SceneFile);
		});
	}

	void SceneRecovery::OnSaved(AssetHandle handle, const std::filesystem::path& sceneFile, const std::string& scenePath, std::string document)
	{
		// The changes are on disk: the previous journal's file goes, and a new journal follows the saved document.
		if (m_Journal)
		{
			Enqueue([journal = m_Journal, directory = m_Directory]()
			{
				if (journal->Written && !directory.empty())
					FileSystem::Remove(directory / FileSystem::FromUTF8(journal->Stem + c_FileExtension));
				journal->Written = false;
			});
		}
		m_Journal = CreateRef<Journal>();
		m_Journal->Stem = MakeJournalStem(handle);
		m_Journal->Scene = handle;
		m_Journal->ScenePath = scenePath;
		m_Journal->SceneFile = sceneFile;
		m_ScenePath = scenePath;
		m_PendingSteps.clear();
		m_Changed = false;
		m_NeedsSnapshot = false;
		m_Unjournaled = false;
		m_Skipped.clear();
		m_LastChange = m_LastWrite = std::chrono::steady_clock::now();
		Enqueue([journal = m_Journal, saved = std::move(document)]()
		{
			journal->BaseHash = HashBytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(saved.data()), saved.size()));
		});
	}

	void SceneRecovery::OnSceneMoved(const std::filesystem::path& sceneFile, const std::string& scenePath)
	{
		if (!m_Journal || !m_Journal->Scene.IsValid() || scenePath.empty() || scenePath == m_ScenePath)
			return;
		m_ScenePath = scenePath;
		// The journal's fields belong to the file operations once they run: changed by one, in order with the writes.
		Enqueue([journal = m_Journal, sceneFile, scenePath]()
		{
			journal->ScenePath = scenePath;
			journal->SceneFile = sceneFile;
			// Moved before BeginScene's hash ran, which found nothing at the old place: a move keeps the file's content, so its
			// version is noted here.
			if (!journal->BaseHash)
				journal->BaseHash = HashFile(sceneFile);
		});
		// Written again soon, so that a journal left behind names the scene's new place (scans find a moved scene by its
		// handle anyway).
		m_Changed = true;
		m_LastChange = std::chrono::steady_clock::now();
	}

	void SceneRecovery::OnApplied(const EditorAction& action, bool undone, bool merged)
	{
		if (!IsActive() || !m_Journal)
			return;
		switch (action.GetSceneChange())
		{
			case SceneChangeKind::None:
				return;
			case SceneChangeKind::Entities:
				// A journal that takes a snapshot when it is written needs no steps.
				if (!m_NeedsSnapshot && !m_Unjournaled)
				{
					Ref<const std::vector<EntityState>> states = action.GetEntityStates(undone);
					if (!states)
					{
						m_NeedsSnapshot = true;
					}
					else if (merged && !m_PendingSteps.empty() && m_PendingSteps.back().Action == &action)
					{
						// A drag merges a frame at a time into its step: the step's latest states replace those of the frame
						// before, which would otherwise wait for the write with every other frame's.
						m_PendingSteps.back().States = std::move(states);
						m_PendingSteps.back().Prior = action.GetEntityStates(!undone);
					}
					else
					{
						m_PendingSteps.push_back({ std::move(states), action.GetEntityStates(!undone), &action });
					}
				}
				break;
			case SceneChangeKind::Settings:
				break; // Every write records the settings
			case SceneChangeKind::Other:
				m_NeedsSnapshot = true;
				break;
		}
		m_Changed = true;
		m_LastChange = std::chrono::steady_clock::now();
	}

	void SceneRecovery::Update(const Scene& scene, bool modified)
	{
		PumpOperations();
		if (m_Scan && m_Scan->Done.load(std::memory_order_acquire))
			FinishScan();
		// Changes discarded since the last scan are offered at once: a scan queued now runs after their write.
		if (m_RescanDue && IsActive() && !m_Scan)
		{
			m_RescanDue = false;
			Scan();
		}
		if (!IsActive() || !m_Journal || !m_Changed)
			return;
		const auto now = std::chrono::steady_clock::now();
		if (SecondsBetween(m_LastChange, now) >= c_QuietSeconds || SecondsBetween(m_LastWrite, now) >= c_MaxIntervalSeconds)
			Write(scene, modified, false);
	}

	bool SceneRecovery::Flush(const Scene& scene, bool modified)
	{
		if (!IsActive() || !m_Journal || !m_Changed)
			return false;
		return Write(scene, modified, false);
	}

	bool SceneRecovery::Write(const Scene& scene, bool modified, bool discarded)
	{
		m_Changed = false;
		m_LastWrite = std::chrono::steady_clock::now();
		const Ref<Journal> journal = m_Journal;
		if (!modified)
		{
			// The scene is what it started from (its file, or a new scene's first state), undone back to: nothing to recover.
			m_PendingSteps.clear();
			Enqueue([journal, directory = m_Directory]()
			{
				if (journal->Written)
					FileSystem::Remove(directory / FileSystem::FromUTF8(journal->Stem + c_FileExtension));
				journal->Written = false;
				journal->Segments.clear();
			});
			return true;
		}
		if (m_Unjournaled)
		{
			m_PendingSteps.clear();
			return false;
		}

		Ref<const std::vector<EntityState>> snapshot;
		if (m_NeedsSnapshot)
		{
			// A step only a snapshot captures (EditorAction::GetSceneChange is Other): the journal holds the whole scene from now on.
			const size_t count = scene.GetEntityCount();
			m_PendingSteps.clear();
			if (count > c_MaxSnapshotEntities)
			{
				m_Unjournaled = true;
				m_Skipped = fmt::format("an edit changed it in a way only a snapshot captures, and it has {} entities, more than the {} a snapshot "
					"may hold without holding up a frame", count, c_MaxSnapshotEntities);
				return false;
			}
			snapshot = CaptureScene(scene);
		}
		m_Writes++;

		std::vector<AppliedStep> steps = std::move(m_PendingSteps);
		m_PendingSteps.clear();
		// The journal's text is worked out by a background worker, then written by the file operation queued after it.
		Ref<std::string> text = CreateRef<std::string>();
		Enqueue([journal, text, steps = std::move(steps), snapshot, settings = SerializeSettings(scene.GetSettings()), sceneName = scene.GetName(),
			discarded, processId = m_ProcessId, processStartTime = m_ProcessStartTime]()
		{
			Journal& target = *journal;
			if (snapshot)
			{
				target.Base = snapshot;
				target.Segments.clear();
				target.Complete = true;
			}
			else
			{
				for (const AppliedStep& step : steps)
				{
					const std::vector<EntityState>& states = *step.States;
					// Steps that move no entity fold into the one before; those that do are replayed as they came.
					if (target.Segments.empty() || IsStructural(states, step.Prior.get()))
						target.Segments.emplace_back();
					Segment& segment = target.Segments.back();
					for (size_t index = 0; index < states.size(); index++)
					{
						const auto [entry, added] = segment.States.insert_or_assign(states[index].ID, std::make_pair(step.States, index));
						if (added)
							segment.Order.push_back(states[index].ID);
					}
				}
			}

			// A complete journal's first step is what the scene started from.
			uint64_t entityCount = 0;
			std::string body = "{\"Settings\":" + JsonUtils::Dump(settings) + ",\"Steps\":[";
			if (target.Base)
			{
				body += '[';
				for (size_t index = 0; index < target.Base->size(); index++)
				{
					if (index > 0)
						body += ',';
					AppendState(body, (*target.Base)[index]);
				}
				body += ']';
				entityCount += target.Base->size();
			}
			for (size_t segmentIndex = 0; segmentIndex < target.Segments.size(); segmentIndex++)
			{
				const Segment& segment = target.Segments[segmentIndex];
				body += segmentIndex > 0 || target.Base ? ",[" : "[";
				for (size_t index = 0; index < segment.Order.size(); index++)
				{
					const auto& [states, position] = segment.States.at(segment.Order[index]);
					if (index > 0)
						body += ',';
					AppendState(body, (*states)[position]);
				}
				body += ']';
				entityCount += segment.Order.size();
			}
			body += "]}\n";

			// Changes that apply to a scene file need its version (a complete journal starts from an empty scene instead).
			if (!target.Complete && target.Scene.IsValid() && !target.BaseHash && !target.BaseHashReported)
			{
				ST_WARN("Recovery: cannot read the scene file '{}' to note its version, so the unsaved changes of '{}' cannot be recovered",
					FileSystem::ToUTF8(target.SceneFile), sceneName);
				target.BaseHashReported = true;
			}
			const nlohmann::json header = {
				{ "Strata", { { "Format", c_Format }, { "Version", c_FormatVersion } } },
				{ "Scene", target.Scene.IsValid() ? UUIDToJson(target.Scene) : nlohmann::json(nullptr) },
				{ "SceneName", sceneName },
				{ "ScenePath", target.ScenePath },
				{ "BaseHash", target.BaseHash ? nlohmann::json(*target.BaseHash) : nlohmann::json(nullptr) },
				{ "Owner", { { "ProcessId", processId }, { "ProcessStartTime", processStartTime } } },
				{ "Discarded", discarded },
				{ "Time", GetUnixMilliseconds() },
				{ "Entities", entityCount },
				{ "Steps", target.Segments.size() + (target.Base ? 1 : 0) },
				{ "Complete", target.Complete } };
			*text = JsonUtils::Dump(header) + "\n" + body;
		}, true);
		Enqueue([journal, text, directory = m_Directory, sceneName = scene.GetName(), lastWriteTime = m_LastWriteTime]()
		{
			// Empty when working it out failed (reported then).
			if (text->empty())
				return;
			std::string error;
			const bool written = WriteFileReplacing(directory / FileSystem::FromUTF8(journal->Stem + c_FileExtension), *text, error);
			// A large journal's text goes with its write.
			std::string().swap(*text);
			if (!written)
			{
				ST_WARN("Recovery: the unsaved changes of '{}' could not be written: {}", sceneName, error);
				return;
			}
			journal->Written = true;
			lastWriteTime->store(GetUnixMilliseconds(), std::memory_order_release);
		});
		return true;
	}

	void SceneRecovery::Enqueue(std::function<void()> operation, bool compute)
	{
		m_Operations.push_back(Operation { std::move(operation), compute });
		PumpOperations();
	}

	void SceneRecovery::PumpOperations()
	{
		// One at a time, in order; without a job system the operation runs at once.
		while (!m_Operations.empty() && (!m_Running.IsValid() || m_Running.IsComplete()))
		{
			Operation operation = std::move(m_Operations.front());
			m_Operations.pop_front();
			std::function<void()> job = [function = std::move(operation.Function)]()
			{
				// Operations use third-party code (JSON) that may throw: contained, so the thread goes on.
				try
				{
					function();
				}
				catch (const std::exception& exception)
				{
					ST_ERROR("Recovery: a file operation failed: {}", exception.what());
				}
			};
			m_Running = operation.Compute ? JobSystem::Submit(std::move(job), JobPriority::Background) : JobSystem::SubmitIO(std::move(job));
		}
	}

	void SceneRecovery::Wait()
	{
		while (!m_Operations.empty() || (m_Running.IsValid() && !m_Running.IsComplete()))
		{
			if (m_Running.IsValid())
				m_Running.Wait();
			PumpOperations();
		}
		if (m_Scan && m_Scan->Done.load(std::memory_order_acquire))
			FinishScan();
	}

	bool SceneRecovery::IsBusy() const
	{
		return !m_Operations.empty() || (m_Running.IsValid() && !m_Running.IsComplete());
	}

	void SceneRecovery::Scan()
	{
		if (!IsActive())
		{
			m_Scan.reset();
			m_Entries.clear();
			m_EntriesVersion++;
			return;
		}
		Ref<ScanResult> scan = CreateRef<ScanResult>();
		scan->Directory = m_Directory;
		scan->AssetDirectory = m_AssetDirectory;
		if (m_SceneLocator)
			scan->Scenes = m_SceneLocator();
		m_Scan = scan;
		// Queued after every write asked for so far, discards included.
		m_RescanDue = false;
		Enqueue([scan, processId = m_ProcessId, processStartTime = m_ProcessStartTime]()
		{
			std::error_code error;
			for (std::filesystem::directory_iterator it(scan->Directory, error), end; !error && it != end; it.increment(error))
			{
				std::error_code fileError;
				if (!it->is_regular_file(fileError) || FileSystem::ToUTF8(it->path().extension()) != c_FileExtension)
					continue;
				const std::optional<nlohmann::json> header = ReadHeader(it->path());
				if (!header)
				{
					ST_WARN("Recovery: '{}' is not a recovery file; it is ignored", FileSystem::ToUTF8(it->path()));
					continue;
				}
				RecoveryEntry entry = ReadEntry(*header, FileSystem::ToUTF8(it->path().stem()));
				// The journal of an editor that is still editing the scene (this one, or another on the same project) is its own
				// business until it discards it or goes away.
				if (!entry.Discarded)
				{
					const nlohmann::json* owner = JsonUtils::Find(*header, "Owner");
					const uint32_t ownerId = owner ? static_cast<uint32_t>(JsonUtils::GetUInt(*owner, "ProcessId", 0)) : 0;
					const uint64_t ownerStart = owner ? JsonUtils::GetUInt(*owner, "ProcessStartTime", 0) : 0;
					// A process is the owner only with the owner's start time too: process ids are reused, also this one's (an
					// editor that crashed earlier may have had it).
					const bool alive = ownerId != 0 && (ownerId == processId ? ownerStart == processStartTime
						: Platform::IsProcessAlive(ownerId) && Platform::GetProcessStartTime(ownerId).value_or(0) == ownerStart);
					if (alive)
						continue;
				}
				if (entry.Scene.IsValid())
				{
					// The scene by its handle: it may have moved since the changes were written (its file is the same).
					if (const auto scene = scan->Scenes.find(entry.Scene); scene != scan->Scenes.end())
						entry.ScenePath = scene->second;
					if (entry.ScenePath.empty())
						entry.Problem = "It does not name the scene file it applies to";
					else
						entry.Problem = CheckSceneFile(scan->AssetDirectory / FileSystem::FromUTF8(entry.ScenePath), entry.ScenePath, ReadBaseHash(*header));
					entry.Applicable = entry.Problem.empty();
				}
				scan->Entries.push_back(std::move(entry));
			}
			RetireDiscards(scan->Directory, scan->Entries, c_MaxOfferedDiscards);
			std::sort(scan->Entries.begin(), scan->Entries.end(), [](const RecoveryEntry& a, const RecoveryEntry& b) { return a.Time > b.Time; });
			scan->Done.store(true, std::memory_order_release);
		});
	}

	void SceneRecovery::FinishScan()
	{
		m_Entries = std::move(m_Scan->Entries);
		// A scene that moved while the scan ran may have been looked for at its old place: read again.
		if (m_SceneLocator && m_SceneLocator() != m_Scan->Scenes)
			m_RescanDue = true;
		m_Scan.reset();
		if (m_Validator)
		{
			for (RecoveryEntry& entry : m_Entries)
			{
				if (!entry.Applicable)
					continue;
				std::string problem = m_Validator(entry);
				if (!problem.empty())
				{
					entry.Applicable = false;
					entry.Problem = std::move(problem);
				}
			}
		}
		m_EntriesVersion++;
	}

	const RecoveryEntry* SceneRecovery::FindEntry(const std::string& id) const
	{
		const auto it = std::find_if(m_Entries.begin(), m_Entries.end(), [&id](const RecoveryEntry& entry) { return entry.Id == id; });
		return it != m_Entries.end() ? &*it : nullptr;
	}

	bool SceneRecovery::Discard(const std::string& id, std::string* outError)
	{
		const auto it = std::find_if(m_Entries.begin(), m_Entries.end(), [&id](const RecoveryEntry& entry) { return entry.Id == id; });
		if (it == m_Entries.end() || !IsActive())
		{
			if (outError)
				*outError = fmt::format("There is no recovery data '{}' (scene.recoveryStatus lists it)", id);
			return false;
		}
		m_Entries.erase(it);
		m_EntriesVersion++;
		Enqueue([directory = m_Directory, id]() { MoveToDiscarded(directory, id); });
		return true;
	}

	Ref<SceneRecovery::Load> SceneRecovery::Read(const std::string& id, const std::filesystem::path& sceneFile)
	{
		Ref<Load> load = CreateRef<Load>();
		load->Id = id;
		load->File = m_Directory / FileSystem::FromUTF8(id + c_FileExtension);
		load->SceneFile = sceneFile;
		Enqueue([load]()
		{
			auto finish = [&load](std::string error)
			{
				load->Error = std::move(error);
				load->Done.store(true, std::memory_order_release);
			};
			const std::optional<std::string> text = FileSystem::ReadText(load->File);
			if (!text)
				return finish(fmt::format("Cannot read '{}'", FileSystem::ToUTF8(load->File)));
			const size_t lineEnd = text->find('\n');
			const std::optional<nlohmann::json> header = lineEnd != std::string::npos ? JsonUtils::Parse(std::string_view(*text).substr(0, lineEnd)) : std::nullopt;
			const nlohmann::json* strata = header ? JsonUtils::Find(*header, "Strata") : nullptr;
			if (!strata || JsonUtils::GetString(*strata, "Format") != c_Format || JsonUtils::GetUInt(*strata, "Version", 0) != c_FormatVersion)
				return finish(fmt::format("'{}' is not a recovery file of this editor version", FileSystem::ToUTF8(load->File)));
			std::string parseError;
			std::optional<nlohmann::json> body = JsonUtils::Parse(std::string_view(*text).substr(lineEnd + 1), &parseError);
			if (!body || !body->is_object())
				return finish(fmt::format("'{}' is damaged: {}", FileSystem::ToUTF8(load->File), parseError.empty() ? "no data" : parseError));

			RecoveryData data;
			data.Entry = ReadEntry(*header, load->Id);
			data.BaseHash = ReadBaseHash(*header);
			if (const nlohmann::json* settings = JsonUtils::Find(*body, "Settings"))
				data.Settings = *settings;
			const auto steps = body->find("Steps");
			if (steps == body->end() || !steps->is_array())
				return finish(fmt::format("'{}' is damaged: it has no steps", FileSystem::ToUTF8(load->File)));
			std::unordered_set<UUID> seen;
			for (nlohmann::json& step : *steps)
			{
				if (!step.is_array())
					return finish(fmt::format("'{}' is damaged: a step is not a list of entity states", FileSystem::ToUTF8(load->File)));
				std::vector<EntityState>& states = data.Steps.emplace_back();
				seen.clear();
				for (nlohmann::json& stateJson : step)
				{
					std::string error;
					std::optional<EntityState> state = ParseState(stateJson, error);
					if (!state)
						return finish(fmt::format("'{}' is damaged: {}", FileSystem::ToUTF8(load->File), error));
					if (!seen.insert(state->ID).second)
						return finish(fmt::format("'{}' is damaged: entity {} appears twice in a step", FileSystem::ToUTF8(load->File), state->ID.ToString()));
					states.push_back(std::move(*state));
				}
			}
			if (data.Entry.Scene.IsValid())
			{
				std::string problem = load->SceneFile.empty() ? fmt::format("The scene {} is not in the project", data.Entry.Scene.ToString())
					: CheckSceneFile(load->SceneFile, data.Entry.ScenePath, data.BaseHash);
				if (!problem.empty())
					return finish(std::move(problem));
			}
			load->Data = std::move(data);
			finish({});
		});
		return load;
	}

	void SceneRecovery::Replace(const Scene& scene, bool modified, const std::string& id)
	{
		// The journal reaches the disk first: only then does the applied file go.
		if (!Flush(scene, modified))
			return;
		m_Entries.erase(std::remove_if(m_Entries.begin(), m_Entries.end(), [&id](const RecoveryEntry& entry) { return entry.Id == id; }), m_Entries.end());
		m_EntriesVersion++;
		Enqueue([file = m_Directory / FileSystem::FromUTF8(id + c_FileExtension), journal = m_Journal]()
		{
			if (journal->Written)
				FileSystem::Remove(file);
			else
				ST_WARN("Recovery: '{}' stays, as the recovered changes could not be journaled", FileSystem::ToUTF8(file));
		});
	}

	RecoveryJournalStatus SceneRecovery::GetStatus() const
	{
		RecoveryJournalStatus status;
		status.Active = IsActive() && m_Journal;
		if (m_Journal)
			status.File = m_Journal->Stem + c_FileExtension;
		status.Writes = m_Writes;
		status.LastWrite = m_LastWriteTime->load(std::memory_order_acquire);
		status.Pending = m_Changed;
		status.PendingSteps = m_PendingSteps.size();
		status.Busy = IsBusy();
		if (!m_Skipped.empty())
		{
			status.Skipped = fmt::format("its changes are not journaled: {}; save it to protect them (scene.save, or scene.saveAs for a scene never "
				"saved)", m_Skipped);
		}
		return status;
	}

	nlohmann::json SceneRecovery::DescribeStatus() const
	{
		const RecoveryJournalStatus status = GetStatus();
		const size_t applicable = static_cast<size_t>(std::count_if(m_Entries.begin(), m_Entries.end(), [](const RecoveryEntry& entry) { return entry.Applicable; }));
		nlohmann::json journal = {
			{ "active", status.Active },
			{ "file", status.File },
			{ "writes", status.Writes },
			{ "lastWrite", status.LastWrite },
			{ "pending", status.Pending },
			{ "pendingSteps", status.PendingSteps },
			{ "busy", status.Busy } };
		if (!status.Skipped.empty())
			journal["skipped"] = status.Skipped;
		return { { "scanning", IsScanning() }, { "entries", m_Entries.size() }, { "applicable", applicable }, { "journal", std::move(journal) } };
	}

	nlohmann::json SceneRecovery::DescribeEntry(const RecoveryEntry& entry)
	{
		nlohmann::json description = {
			{ "id", entry.Id },
			{ "scene", entry.Scene.IsValid() ? UUIDToJson(entry.Scene) : nlohmann::json(nullptr) },
			{ "sceneName", entry.SceneName },
			{ "scenePath", entry.ScenePath.empty() ? nlohmann::json(nullptr) : nlohmann::json(entry.ScenePath) },
			{ "time", entry.Time },
			{ "discarded", entry.Discarded },
			{ "entityCount", entry.EntityCount },
			{ "complete", entry.Complete },
			{ "applicable", entry.Applicable } };
		if (!entry.Problem.empty())
			description["problem"] = entry.Problem;
		return description;
	}

}
