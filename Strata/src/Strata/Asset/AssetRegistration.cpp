#include "stpch.h"
#include "Strata/Asset/AssetRegistration.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Reflection/PropertyJson.h"

#include <algorithm>
#include <chrono>
#include <system_error>

namespace Strata
{

	namespace
	{

		bool IsHiddenName(const std::filesystem::path& name)
		{
			const std::string text = FileSystem::ToUTF8(name);
			return !text.empty() && text[0] == '.';
		}

	}

	const char* AssetScanPhaseToString(AssetScanPhase phase)
	{
		switch (phase)
		{
			case AssetScanPhase::Walking:     return "Walking";
			case AssetScanPhase::Registering: return "Registering";
			case AssetScanPhase::Applying:    return "Applying";
		}
		return "Unknown";
	}

	RegistrationSlice::RegistrationSlice(const AssetRegistrationOptions& options, bool unlimited)
		: m_Options(options), m_Unlimited(unlimited)
	{
		m_Start = Now();
	}

	uint64_t RegistrationSlice::Now() const
	{
		if (m_Options.Clock)
			return m_Options.Clock();
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	bool RegistrationSlice::HasTime()
	{
		if (m_Unlimited)
			return true;
		if (m_Ended)
			return false;
		m_Entries++;
		const uint64_t every = std::max<uint32_t>(m_Options.EntriesPerCheck, 1);
		if (m_Entries % every == 0 && Now() - m_Start >= m_Options.SliceMicroseconds)
			m_Ended = true;
		return !m_Ended;
	}

	AssetMeta ReadAssetMeta(const std::filesystem::path& metaPath)
	{
		AssetMeta contents;
		const std::optional<std::string> text = FileSystem::ReadText(metaPath);
		if (!text)
		{
			// Missing, or present and unreadable.
			if (FileSystem::Exists(metaPath))
			{
				contents.Status = AssetMetaStatus::Invalid;
				contents.Error = "cannot be read";
			}
			return contents;
		}
		contents.Status = AssetMetaStatus::Invalid;
		std::optional<nlohmann::json> meta = JsonUtils::Parse(*text, &contents.Error);
		if (!meta || !meta->is_object())
		{
			if (contents.Error.empty())
				contents.Error = "not a JSON object";
			return contents;
		}
		const nlohmann::json* handle = JsonUtils::Find(*meta, "Handle");
		const std::optional<UUID> parsedHandle = handle ? UUIDFromJson(*handle) : std::nullopt;
		if (!parsedHandle || !parsedHandle->IsValid())
		{
			contents.Error = "missing or invalid \"Handle\"";
			return contents;
		}
		contents.Status = AssetMetaStatus::Valid;
		contents.Handle = *parsedHandle;
		if (const nlohmann::json* settings = JsonUtils::Find(*meta, "ImportSettings"); settings && settings->is_object())
			contents.Settings = *settings;
		return contents;
	}

	bool WriteAssetMeta(const std::filesystem::path& metaPath, AssetHandle handle, AssetType type, const nlohmann::json& settings)
	{
		nlohmann::json meta = nlohmann::json::object();
		meta["Strata"] = { { "Format", "AssetMeta" }, { "Version", c_AssetMetaVersion } };
		meta["Handle"] = UUIDToJson(handle);
		meta["Type"] = AssetTypeToString(type);
		meta["ImportSettings"] = settings;
		return FileSystem::WriteText(metaPath, JsonUtils::Dump(meta, 1, '\t') + "\n");
	}

	AssetDirectoryListing WalkAssetDirectory(const std::filesystem::path& directory, const std::atomic<bool>* cancel, std::atomic<uint64_t>* found)
	{
		AssetDirectoryListing listing;
		std::error_code error;
		std::filesystem::recursive_directory_iterator iterator(directory, std::filesystem::directory_options::skip_permission_denied, error);
		const std::filesystem::recursive_directory_iterator end;
		while (!error && iterator != end)
		{
			if (cancel && cancel->load(std::memory_order_relaxed))
			{
				listing.Error = "cancelled";
				break;
			}
			const std::filesystem::directory_entry& entry = *iterator;
			std::error_code typeError;
			if (IsHiddenName(entry.path().filename()))
			{
				if (entry.is_directory(typeError))
					iterator.disable_recursion_pending();
			}
			else if (entry.is_directory(typeError))
			{
				listing.Folders.push_back(FileSystem::ToUTF8(FileSystem::GetRelativePath(entry.path(), directory)));
			}
			else if (entry.is_regular_file(typeError) && StringUtils::ToLower(FileSystem::ToUTF8(entry.path().extension())) != ".meta")
			{
				listing.Files.push_back(entry.path().lexically_normal());
				if (found)
					found->fetch_add(1, std::memory_order_relaxed);
			}
			iterator.increment(error);
		}
		if (error)
			listing.Error = error.message();
		std::sort(listing.Files.begin(), listing.Files.end());
		return listing;
	}

	std::vector<DuplicateHandle> FindDuplicateHandles(std::span<const AssetHandle> handles, std::span<const std::string> paths,
		const std::unordered_map<AssetHandle, std::string>& recordedPaths)
	{
		std::unordered_map<AssetHandle, std::vector<size_t>> filesByHandle;
		for (size_t index = 0; index < handles.size(); index++)
		{
			if (handles[index].IsValid())
				filesByHandle[handles[index]].push_back(index);
		}
		std::vector<DuplicateHandle> duplicates;
		for (const auto& [handle, files] : filesByHandle)
		{
			if (files.size() < 2)
				continue;
			const auto recorded = recordedPaths.find(handle);
			size_t winner = files.front();
			bool fromRecord = false;
			if (recorded != recordedPaths.end())
			{
				for (const size_t file : files)
				{
					if (paths[file] == recorded->second)
					{
						winner = file;
						fromRecord = true;
						break;
					}
				}
			}
			if (!fromRecord)
			{
				for (const size_t file : files)
				{
					if (paths[file].size() < paths[winner].size())
						winner = file;
				}
			}
			for (const size_t file : files)
			{
				if (file != winner)
					duplicates.push_back(DuplicateHandle { file, winner });
			}
		}
		std::sort(duplicates.begin(), duplicates.end(), [](const DuplicateHandle& a, const DuplicateHandle& b) { return a.File < b.File; });
		return duplicates;
	}

}
