#include "Editor/ThumbnailCache.h"

#include "Editor/DisplayImage.h"
#include "Editor/EditorCamera.h"
#include "Editor/SceneBounds.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Hash.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Profiling.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/BlockCompression.h>
#include <Strata/Renderer/ImageReader.h>
#include <Strata/Renderer/ImageWriter.h>
#include <Strata/Renderer/Renderer.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Renderer/TextureReadback.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/Scene.h>

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace Strata
{

	namespace
	{

		constexpr const char* c_IndexFile = "Index.json";
		// The start of a cooked texture read first: its header, mip table and name always fit.
		constexpr uint64_t c_TextureHeaderBytes = 4096;
		// The most a texture thumbnail reads: a texture without a level that fits c_Size has its smallest level read whole.
		constexpr uint64_t c_MaxTextureReadBytes = 64ull * 1024 * 1024;
		// Frames a thumbnail nobody asked for keeps its texture before it may be forgotten.
		constexpr uint64_t c_ForgetFrames = 600;
		// Frames between writes of a changed index.
		constexpr uint64_t c_IndexWriteFrames = 120;
		// The view of rendered thumbnails: from the front left, a little above.
		constexpr float c_ViewYaw = -35.0f;
		constexpr float c_ViewPitch = -25.0f;
		// Frames rendered with nothing loading before the image is read back (the environment and shadows settle).
		constexpr uint32_t c_QuietFrames = 2;
		// A material's sphere leaves this much room around it (its radius times this is the view's half height).
		constexpr float c_SphereMargin = 1.08f;

		uint8_t EncodeSRGB(float linear)
		{
			const float clamped = std::clamp(linear, 0.0f, 1.0f);
			const float encoded = clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
			return static_cast<uint8_t>(std::lround(encoded * 255.0f));
		}

		// HDR radiance shown on a display: Reinhard tone mapping, then sRGB.
		uint8_t ToneMap(float value)
		{
			const float positive = std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
			return EncodeSRGB(positive / (1.0f + positive));
		}

		std::string ToHex(uint64_t value)
		{
			return fmt::format("{:016X}", value);
		}

		std::optional<uint64_t> FromHex(const std::string& text)
		{
			if (text.empty() || text.size() > 16)
				return std::nullopt;
			uint64_t value = 0;
			for (char character : text)
			{
				uint64_t digit = 0;
				if (character >= '0' && character <= '9')
					digit = static_cast<uint64_t>(character - '0');
				else if (character >= 'A' && character <= 'F')
					digit = static_cast<uint64_t>(character - 'A' + 10);
				else if (character >= 'a' && character <= 'f')
					digit = static_cast<uint64_t>(character - 'a' + 10);
				else
					return std::nullopt;
				value = value << 4 | digit;
			}
			return value;
		}

		// The source texels [first, end) an output texel of a side scaled from `source` to `target` texels averages: whole
		// texels within the side, at least one.
		std::pair<uint64_t, uint64_t> GetBoxRange(uint32_t index, uint32_t source, uint32_t target)
		{
			const uint64_t first = static_cast<uint64_t>(index) * source / target;
			const uint64_t end = std::max(first + 1, static_cast<uint64_t>(index + 1) * source / target);
			return { first, std::min<uint64_t>(end, source) };
		}

		// Box-filters an RGBA8 image down to fit `size` on a side (images that fit already stay). Each output texel averages
		// the source texels its box covers within the image, so a side shorter than the reduction (a 4096x1 strip) keeps
		// its texels instead of reading past them.
		ThumbnailImage FitImage(ThumbnailImage image, uint32_t size)
		{
			const uint32_t factor = (std::max(image.Width, image.Height) + size - 1) / size;
			if (factor <= 1)
				return image;
			ThumbnailImage fitted;
			fitted.Width = std::max(1u, image.Width / factor);
			fitted.Height = std::max(1u, image.Height / factor);
			fitted.Pixels.resize(static_cast<size_t>(fitted.Width) * fitted.Height * 4);
			for (uint32_t y = 0; y < fitted.Height; y++)
			{
				const auto [rowFirst, rowEnd] = GetBoxRange(y, image.Height, fitted.Height);
				for (uint32_t x = 0; x < fitted.Width; x++)
				{
					const auto [columnFirst, columnEnd] = GetBoxRange(x, image.Width, fitted.Width);
					uint64_t sum[4] = {};
					for (uint64_t sourceY = rowFirst; sourceY < rowEnd; sourceY++)
					{
						const uint8_t* row = image.Pixels.data() + static_cast<size_t>((sourceY * image.Width + columnFirst) * 4);
						for (uint64_t offset = 0; offset < (columnEnd - columnFirst) * 4; offset++)
							sum[offset % 4] += row[offset];
					}
					const uint64_t count = (rowEnd - rowFirst) * (columnEnd - columnFirst);
					uint8_t* out = fitted.Pixels.data() + (static_cast<size_t>(y) * fitted.Width + x) * 4;
					for (int channel = 0; channel < 4; channel++)
						out[channel] = static_cast<uint8_t>((sum[channel] + count / 2) / count);
				}
			}
			return fitted;
		}

	}

	struct ThumbnailCache::SharedState
	{
		struct Result
		{
			AssetHandle Handle = UUID::Null();
			uint64_t Key = 0;
			Ref<ThumbnailImage> Image; // Null when it failed
			bool FromDisk = false;
			uint64_t StoredBytesRead = 0;
			std::string Error;
		};
		struct Written
		{
			AssetHandle Handle = UUID::Null();
			uint64_t Key = 0;
			bool Success = false;
		};

		std::mutex Mutex;
		std::condition_variable Condition;
		std::vector<Result> Results;
		std::vector<Written> Writes;
		std::optional<std::unordered_map<AssetHandle, uint64_t>> LoadedIndex;
		// Jobs reading through the asset manager now: the destructor waits for them. A job counts itself once it starts, and
		// only while the cache exists (Closed), so the destructor never waits for jobs still queued behind other I/O.
		uint32_t ReadsRunning = 0;
		bool Closed = false;
	};

	ThumbnailCache::ThumbnailCache(const Ref<EditorAssetManager>& assets, ThumbnailCacheSpecification specification)
		: m_Assets(assets), m_Specification(std::move(specification)), m_Shared(CreateRef<SharedState>())
	{
		m_ContentVersion = m_Assets->GetContentVersion();
		if (m_Specification.Directory.empty())
		{
			m_IndexLoaded = true;
			return;
		}
		// The index of the files there, read on an I/O thread (a missing or unreadable one is an empty index).
		JobSystem::SubmitIO([shared = m_Shared, file = m_Specification.Directory / c_IndexFile]()
		{
			std::unordered_map<AssetHandle, uint64_t> index;
			if (const std::optional<std::string> text = FileSystem::ReadText(file))
			{
				const std::optional<nlohmann::json> json = JsonUtils::Parse(*text);
				const nlohmann::json* keys = json && json->is_object() && JsonUtils::GetUInt(*json, "Version", 0) == c_Version ? JsonUtils::Find(*json, "Keys")
					: nullptr;
				if (keys && keys->is_object())
				{
					for (const auto& [handleText, keyText] : keys->items())
					{
						const std::optional<UUID> handle = UUIDFromJson(nlohmann::json(handleText));
						const std::optional<uint64_t> key = keyText.is_string() ? FromHex(keyText.get<std::string>()) : std::nullopt;
						if (handle && key)
							index[*handle] = *key;
					}
				}
			}
			std::scoped_lock<std::mutex> lock(shared->Mutex);
			shared->LoadedIndex = std::move(index);
		}, JobPriority::Low);
	}

	ThumbnailCache::~ThumbnailCache()
	{
		// Reads use the asset manager, which must outlive them: the ones under way are short (a texture's header and mip tail),
		// and the ones that have not started never read.
		{
			std::unique_lock<std::mutex> lock(m_Shared->Mutex);
			m_Shared->Closed = true;
			m_Shared->Condition.wait(lock, [this]() { return m_Shared->ReadsRunning == 0; });
		}
		if (m_IndexDirty)
			WriteIndex();
		std::vector<nvrhi::ResourceHandle> textures;
		for (auto& [handle, entry] : m_Entries)
		{
			if (entry.Texture)
				textures.push_back(entry.Texture);
		}
		if (m_Preview.Texture)
			textures.push_back(m_Preview.Texture);
		if (!textures.empty())
			Renderer::ReleaseDeferred(std::move(textures));
	}

	bool ThumbnailCache::HasThumbnail(AssetType type)
	{
		return type == AssetType::Texture || type == AssetType::Material || type == AssetType::Mesh || type == AssetType::Model || type == AssetType::Prefab;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Requests
	////////////////////////////////////////////////////////////////////////////////

	nvrhi::ITexture* ThumbnailCache::Request(AssetHandle handle)
	{
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end())
		{
			Entry entry;
			entry.Type = m_Assets->GetAssetType(handle);
			const bool supported = HasThumbnail(entry.Type) && !BuiltinAssets::IsBuiltin(handle) && Renderer::IsInitialized();
			entry.State = supported ? ThumbnailState::None : ThumbnailState::Unavailable;
			it = m_Entries.emplace(handle, std::move(entry)).first;
		}
		Entry& entry = it->second;
		entry.LastRequest = m_Frame;
		if (entry.State == ThumbnailState::None && !entry.Queued)
		{
			entry.State = ThumbnailState::Pending;
			entry.Queued = true;
			m_Queue.push_back(handle);
		}
		return entry.Texture.Get();
	}

	nvrhi::ITexture* ThumbnailCache::RequestPreview(AssetHandle handle, uint32_t size)
	{
		// The thumbnail is asked for too: it shows until the preview is ready, and it says whether the asset gets one at all.
		nvrhi::ITexture* thumbnail = Request(handle);
		const Entry& entry = m_Entries.at(handle);
		if (entry.State == ThumbnailState::Unavailable || entry.Type == AssetType::Texture)
			return thumbnail;
		const uint32_t rounded = (std::max(size, 1u) + c_PreviewStep - 1) / c_PreviewStep * c_PreviewStep;
		const uint32_t edge = std::clamp(rounded, c_Size, c_MaxPreviewSize);
		if (handle != m_Preview.Handle)
		{
			ResetPreview();
			m_Preview.Handle = handle;
			m_Preview.Type = entry.Type;
		}
		if (edge != m_Preview.Size)
		{
			// Another size: rendered again (the preview at the old size shows meanwhile).
			m_Preview.Size = edge;
			m_Preview.Wanted = true;
		}
		m_Preview.LastRequest = m_Frame;
		return m_Preview.Texture ? m_Preview.Texture.Get() : thumbnail;
	}

	void ThumbnailCache::ResetPreview()
	{
		if (m_Preview.Texture)
			Renderer::ReleaseDeferred({ nvrhi::ResourceHandle(m_Preview.Texture) });
		// A render under way for it ends unused (UpdateSceneJob no longer finds it current).
		m_Preview = Preview();
	}

	ThumbnailState ThumbnailCache::GetState(AssetHandle handle) const
	{
		auto it = m_Entries.find(handle);
		return it != m_Entries.end() ? it->second.State : ThumbnailState::None;
	}

	void ThumbnailCache::Clear()
	{
		if (m_SceneJob)
		{
			m_SceneJob->Loads.Cancel();
			m_SceneJob.reset();
		}
		ResetPreview();
		std::vector<nvrhi::ResourceHandle> textures;
		for (auto& [handle, entry] : m_Entries)
		{
			if (entry.Texture)
				textures.push_back(entry.Texture);
		}
		if (!textures.empty())
			Renderer::ReleaseDeferred(std::move(textures));
		m_Entries.clear();
		m_Dependents.clear();
		m_Queue.clear();
		m_Uploads.clear();
		if (m_Index.empty())
			return;
		std::vector<std::filesystem::path> files;
		for (const auto& [handle, key] : m_Index)
			files.push_back(GetFile(handle));
		m_Index.clear();
		m_IndexDirty = true;
		JobSystem::SubmitIO([files = std::move(files)]()
		{
			for (const std::filesystem::path& file : files)
				FileSystem::Remove(file);
		}, JobPriority::Low);
	}

	bool ThumbnailCache::IsBusy() const
	{
		const bool previewPending = m_Preview.Handle.IsValid() && (m_Preview.Working || m_Preview.Image
			|| (m_Preview.Wanted && m_Frame - m_Preview.LastRequest <= m_Specification.RequestFrames));
		return !m_Queue.empty() || !m_Uploads.empty() || m_SceneJob.has_value() || m_JobsRunning > 0 || !m_IndexLoaded || previewPending;
	}

	ThumbnailCacheStats ThumbnailCache::GetStats() const
	{
		ThumbnailCacheStats stats = m_Stats;
		stats.Entries = m_Entries.size();
		stats.GPUTextures = 0;
		stats.Pending = 0;
		for (const auto& [handle, entry] : m_Entries)
		{
			stats.GPUTextures += entry.Texture ? 1 : 0;
			stats.Pending += entry.State == ThumbnailState::Pending ? 1 : 0;
		}
		return stats;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Frame
	////////////////////////////////////////////////////////////////////////////////

	void ThumbnailCache::Update()
	{
		ST_PROFILE_FUNCTION();
		m_Frame++;
		ApplyResults();
		WatchSources();
		UpdateSceneJob();
		// The preview the Asset tab shows goes before the thumbnails that wait.
		StartPreview();
		StartQueued();
		Upload();
		if (m_Frame % 60 == 0)
			Forget();
		if (m_IndexDirty && m_Frame - m_IndexWrittenFrame >= c_IndexWriteFrames)
			WriteIndex();
	}

	void ThumbnailCache::ApplyResults()
	{
		std::vector<SharedState::Result> results;
		std::vector<SharedState::Written> writes;
		std::optional<std::unordered_map<AssetHandle, uint64_t>> index;
		{
			std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
			results.swap(m_Shared->Results);
			writes.swap(m_Shared->Writes);
			index.swap(m_Shared->LoadedIndex);
		}
		if (index)
		{
			m_Index = std::move(*index);
			m_IndexLoaded = true;
		}
		for (SharedState::Result& result : results)
		{
			m_JobsRunning--;
			m_Stats.StoredBytesRead += result.StoredBytesRead;
			auto it = m_Entries.find(result.Handle);
			if (it == m_Entries.end() || it->second.Key != result.Key)
				continue; // Forgotten, or made again for newer sources meanwhile
			Entry& entry = it->second;
			entry.Working = false;
			if (result.Image)
			{
				Finish(result.Handle, result.Key, std::move(result.Image), result.FromDisk);
			}
			else if (result.FromDisk)
			{
				// A cache file that cannot be read is made again.
				m_Index.erase(result.Handle);
				m_IndexDirty = true;
				entry.Queued = true;
				m_Queue.push_front(result.Handle);
			}
			else
			{
				ST_WARN("No thumbnail for {}: {}", result.Handle.ToString(), result.Error);
				Fail(result.Handle, result.Key);
			}
		}
		for (const SharedState::Written& written : writes)
		{
			m_JobsRunning--;
			if (!written.Success)
				continue;
			m_Stats.FilesWritten++;
			m_Index[written.Handle] = written.Key;
			m_IndexDirty = true;
		}
	}

	void ThumbnailCache::WatchSources()
	{
		const uint64_t version = m_Assets->GetContentVersion();
		if (version == m_ContentVersion)
			return;
		std::vector<AssetHandle> changed;
		const bool known = m_Assets->GetContentChanges(m_ContentVersion, changed);
		m_ContentVersion = version;
		// Content changes include every load: only a change of what a thumbnail was made from (its key) makes it again.
		std::unordered_set<AssetHandle> affected;
		if (known)
		{
			for (AssetHandle source : changed)
			{
				if (auto dependents = m_Dependents.find(source); dependents != m_Dependents.end())
					affected.insert(dependents->second.begin(), dependents->second.end());
			}
		}
		else
		{
			for (const auto& [handle, entry] : m_Entries)
				affected.insert(handle);
		}
		for (AssetHandle handle : affected)
		{
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end() || it->second.Key == 0 || it->second.DirtySince != 0)
				continue;
			std::vector<AssetHandle> sources;
			if (ComputeKey(handle, sources) != it->second.Key)
				it->second.DirtySince = m_Frame;
		}

		// The preview the same way: made again once its changed sources settled.
		if (m_Preview.Handle.IsValid() && m_Preview.Key != 0 && m_Preview.DirtySince == 0)
		{
			const bool touched = !known || std::any_of(changed.begin(), changed.end(), [this](AssetHandle source)
			{
				return std::find(m_Preview.Sources.begin(), m_Preview.Sources.end(), source) != m_Preview.Sources.end();
			});
			std::vector<AssetHandle> sources;
			if (touched && ComputeKey(m_Preview.Handle, sources) != m_Preview.Key)
				m_Preview.DirtySince = m_Frame;
		}
	}

	void ThumbnailCache::StartPreview()
	{
		Preview& preview = m_Preview;
		// Not shown: rendered when it is asked for again.
		if (!preview.Handle.IsValid() || m_Frame - preview.LastRequest > m_Specification.RequestFrames)
			return;
		if (preview.DirtySince != 0 && m_Frame - preview.DirtySince >= m_Specification.SettleFrames)
		{
			preview.DirtySince = 0;
			preview.Wanted = true;
		}
		if (!preview.Wanted || preview.Working || m_SceneJob || !m_Assets->GetMetadata(preview.Handle))
			return;
		std::vector<AssetHandle> sources;
		const uint64_t key = ComputeKey(preview.Handle, sources);
		// What an import is still making would make the preview stale at once: wait for it.
		for (AssetHandle source : sources)
		{
			if (m_Assets->IsImportPending(source))
				return;
		}
		preview.Wanted = false;
		preview.Key = key;
		preview.Sources = std::move(sources);
		// An asset whose import failed has its type's icon for a thumbnail, which stands in for the preview too.
		if (!m_Assets->GetImportInfo(preview.Handle).Error.empty())
			return;
		preview.Working = true;
		SceneJob job;
		job.Handle = preview.Handle;
		job.Type = preview.Type;
		job.Key = key;
		job.Size = preview.Size;
		job.IsPreview = true;
		job.Loads = m_Assets->RequestLoadWithDependencies(preview.Handle, AssetPriority::Normal);
		m_SceneJob = std::move(job);
	}

	bool ThumbnailCache::IsSceneJobCurrent(const SceneJob& job) const
	{
		if (job.IsPreview)
			return m_Preview.Handle == job.Handle && m_Preview.Working && m_Preview.Key == job.Key && m_Preview.Size == job.Size;
		auto it = m_Entries.find(job.Handle);
		return it != m_Entries.end() && it->second.Key == job.Key;
	}

	void ThumbnailCache::StartQueued()
	{
		// Changed sources: once they settled, thumbnails on screen are made again (the old one shows meanwhile).
		for (auto& [handle, entry] : m_Entries)
		{
			if (entry.DirtySince != 0 && !entry.Queued && !entry.Working && m_Frame - entry.DirtySince >= m_Specification.SettleFrames
				&& m_Frame - entry.LastRequest <= m_Specification.RequestFrames)
			{
				// DirtySince stays until the work starts: an entry that leaves the screen first is made again when it is back.
				entry.Queued = true;
				if (entry.State != ThumbnailState::Ready)
					entry.State = ThumbnailState::Pending;
				m_Queue.push_back(handle);
			}
		}

		std::deque<AssetHandle> waiting;
		while (!m_Queue.empty())
		{
			const AssetHandle handle = m_Queue.front();
			m_Queue.pop_front();
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end() || !it->second.Queued)
				continue;
			Entry& entry = it->second;
			// Off screen again: forgotten until asked for again.
			if (m_Frame - entry.LastRequest > m_Specification.RequestFrames)
			{
				entry.Queued = false;
				if (entry.State == ThumbnailState::Pending)
					entry.State = entry.Texture ? ThumbnailState::Ready : ThumbnailState::None;
				continue;
			}
			if (m_JobsRunning >= m_Specification.MaxJobs || !Start(handle, entry))
				waiting.push_back(handle);
		}
		m_Queue.swap(waiting);
	}

	bool ThumbnailCache::Start(AssetHandle handle, Entry& entry)
	{
		if (!m_IndexLoaded)
			return false;
		const std::optional<AssetMetadata> metadata = m_Assets->GetMetadata(handle);
		if (!metadata)
		{
			entry.Queued = false;
			entry.DirtySince = 0;
			entry.State = ThumbnailState::Unavailable;
			return true;
		}
		std::vector<AssetHandle> sources;
		const uint64_t key = ComputeKey(handle, sources);
		// What an import is still making would make the thumbnail stale at once: wait for it.
		for (AssetHandle source : sources)
		{
			if (m_Assets->IsImportPending(source))
				return false;
		}
		const AssetImportInfo import = m_Assets->GetImportInfo(handle);
		if (!import.Error.empty())
		{
			SetSources(handle, entry, std::move(sources));
			entry.Key = key;
			entry.Queued = false;
			entry.DirtySince = 0;
			Fail(handle, key);
			return true;
		}
		const bool isScene = entry.Type != AssetType::Texture;
		const bool cached = m_Index.contains(handle) && m_Index.at(handle) == key;
		if (isScene && !cached && m_SceneJob)
			return false; // One rendered at a time
		SetSources(handle, entry, std::move(sources));
		entry.Key = key;
		entry.Queued = false;
		entry.Working = true;
		entry.DirtySince = 0;
		if (entry.State != ThumbnailState::Ready)
			entry.State = ThumbnailState::Pending;
		if (cached)
		{
			StartDiskJob(handle, key);
		}
		else if (!isScene)
		{
			StartTextureJob(handle, key);
		}
		else
		{
			SceneJob job;
			job.Handle = handle;
			job.Type = entry.Type;
			job.Key = key;
			job.Loads = m_Assets->RequestLoadWithDependencies(handle, AssetPriority::Low);
			m_SceneJob = std::move(job);
		}
		return true;
	}

	void ThumbnailCache::StartTextureJob(AssetHandle handle, uint64_t key)
	{
		const std::optional<AssetMetadata> metadata = m_Assets->GetMetadata(handle);
		const uint64_t storedSize = metadata ? metadata->StoredSize : 0;
		m_JobsRunning++;
		JobSystem::SubmitIO([shared = m_Shared, assets = m_Assets.get(), handle, key, storedSize]()
		{
			{
				std::scoped_lock<std::mutex> lock(shared->Mutex);
				if (shared->Closed)
					return; // The cache went with its project; the asset manager may be gone too
				shared->ReadsRunning++;
			}
			SharedState::Result result;
			result.Handle = handle;
			result.Key = key;
			auto read = [&](uint64_t offset, uint64_t size, std::vector<uint8_t>& data)
			{
				result.StoredBytesRead += size;
				return assets->ReadStoredRange(handle, offset, size, data, &result.Error);
			};

			// The header first, which tells how many first bytes hold the levels that fit c_Size (stored smallest first, right
			// after the header) or, for a texture without such a level, the smallest levels; then those bytes, unless the first
			// read held them. Version 1 data (levels largest first) and a header longer than the first read are read whole.
			std::vector<uint8_t> data;
			std::optional<CookedTextureView> view;
			if (storedSize == 0)
			{
				result.Error = "The texture's stored size is unknown";
			}
			else if (read(0, std::min(storedSize, c_TextureHeaderBytes), data))
			{
				std::optional<uint64_t> end;
				for (uint32_t dimension = c_Size; !end && dimension <= c_MaxCookedTextureSize; dimension *= 2)
					end = GetCookedTexturePrefixSize(data, storedSize, dimension);
				if (end && *end > c_MaxTextureReadBytes)
				{
					result.Error = "The texture has no level small enough to read for a thumbnail";
				}
				else if (end)
				{
					if (*end <= data.size() || read(0, *end, data))
						view = ParseCookedTexturePrefix(std::span<const uint8_t>(data).first(static_cast<size_t>(*end)), storedSize, &result.Error);
				}
				else if (storedSize <= c_MaxTextureReadBytes)
				{
					if (read(0, storedSize, data))
						view = ParseCookedTexture(data, &result.Error);
				}
				else
				{
					result.Error = "The texture cannot be read in parts (an old cooked form) and is too large to read whole";
				}
			}
			if (view)
			{
				if (std::optional<ThumbnailImage> image = MakeTextureThumbnail(*view, &result.Error))
					result.Image = CreateRef<ThumbnailImage>(std::move(*image));
			}

			std::scoped_lock<std::mutex> lock(shared->Mutex);
			shared->Results.push_back(std::move(result));
			shared->ReadsRunning--;
			shared->Condition.notify_all();
		}, JobPriority::Low);
	}

	void ThumbnailCache::StartDiskJob(AssetHandle handle, uint64_t key)
	{
		m_JobsRunning++;
		JobSystem::SubmitIO([shared = m_Shared, handle, key, file = GetFile(handle)]()
		{
			SharedState::Result result;
			result.Handle = handle;
			result.Key = key;
			result.FromDisk = true;
			if (const std::optional<std::vector<uint8_t>> png = FileSystem::ReadBytes(file))
			{
				std::optional<ReadbackImage> decoded = ImageReader::DecodeRGBA8(*png, &result.Error);
				if (decoded && decoded->Width <= c_Size && decoded->Height <= c_Size)
					result.Image = CreateRef<ThumbnailImage>(ThumbnailImage { decoded->Width, decoded->Height, std::move(decoded->Pixels) });
			}
			std::scoped_lock<std::mutex> lock(shared->Mutex);
			shared->Results.push_back(std::move(result));
		}, JobPriority::Low);
	}

	void ThumbnailCache::Finish(AssetHandle handle, uint64_t key, Ref<ThumbnailImage> image, bool fromDisk)
	{
		Entry& entry = m_Entries.at(handle);
		entry.State = ThumbnailState::Ready;
		entry.Image = image;
		m_Uploads.push_back(handle);
		if (fromDisk)
		{
			m_Stats.LoadedFromDisk++;
			return;
		}
		if (entry.Type == AssetType::Texture)
			m_Stats.TexturesDecoded++;
		else
			m_Stats.ScenesRendered++;
		if (m_Specification.Directory.empty())
			return;
		// Kept on disk: encoded and written on an I/O thread, recorded in the index once written.
		m_JobsRunning++;
		JobSystem::SubmitIO([shared = m_Shared, handle, key, image, file = GetFile(handle)]()
		{
			ReadbackImage pixels;
			pixels.Width = image->Width;
			pixels.Height = image->Height;
			pixels.Format = nvrhi::Format::RGBA8_UNORM;
			pixels.BytesPerPixel = 4;
			pixels.Pixels = image->Pixels;
			std::string error;
			const std::optional<std::vector<uint8_t>> png = ImageWriter::EncodePNG(pixels, false, &error);
			const bool success = png && FileSystem::WriteBytes(file, *png);
			if (!success)
				ST_WARN("The thumbnail of {} could not be saved: {}", handle.ToString(), png ? FileSystem::ToUTF8(file) : error);
			std::scoped_lock<std::mutex> lock(shared->Mutex);
			shared->Writes.push_back({ handle, key, success });
		}, JobPriority::Low);
	}

	void ThumbnailCache::Fail(AssetHandle handle, uint64_t key)
	{
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end() || it->second.Key != key)
			return;
		Entry& entry = it->second;
		entry.Working = false;
		m_Stats.Failed++;
		// A thumbnail shown already stays (the change that broke it may be undone); otherwise the type's icon.
		if (!entry.Texture)
			entry.State = ThumbnailState::Unavailable;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Rendered thumbnails
	////////////////////////////////////////////////////////////////////////////////

	void ThumbnailCache::UpdateSceneJob()
	{
		if (!m_SceneJob)
			return;
		SceneJob& job = *m_SceneJob;
		if (!IsSceneJobCurrent(job))
		{
			// Forgotten, or wanted for newer sources or (a preview) at another size: started again from the queue, or by
			// StartPreview.
			if (job.IsPreview && m_Preview.Handle == job.Handle && m_Preview.Working)
			{
				m_Preview.Working = false;
				m_Preview.Wanted = true;
			}
			job.Loads.Cancel();
			m_SceneJob.reset();
			return;
		}

		if (job.Readback)
		{
			if (!job.Readback->IsReady())
				return;
			ReadbackImage pixels;
			if (!job.Readback->GetResult(pixels) || pixels.BytesPerPixel != 4)
			{
				FinishScene(false, nullptr);
				return;
			}
			Ref<ThumbnailImage> image = CreateRef<ThumbnailImage>();
			image->Width = pixels.Width;
			image->Height = pixels.Height;
			image->Pixels = std::move(pixels.Pixels);
			// The sky fills the image: opaque.
			for (size_t pixel = 3; pixel < image->Pixels.size(); pixel += 4)
				image->Pixels[pixel] = 255;
			FinishScene(true, std::move(image));
			return;
		}

		if (++job.Frames > m_Specification.MaxSceneFrames)
		{
			ST_WARN("No thumbnail for {}: its assets were still loading after {} frames", job.Handle.ToString(), m_Specification.MaxSceneFrames);
			FinishScene(false, nullptr);
			return;
		}
		if (!job.Built)
		{
			if (!job.Loads.IsComplete())
				return;
			std::string error;
			if (!BuildScene(job, &error))
			{
				if (!error.empty())
				{
					ST_WARN("No thumbnail for {}: {}", job.Handle.ToString(), error);
					FinishScene(false, nullptr);
				}
				return;
			}
			job.Built = true;
		}

		std::vector<AssetHandle> loadingMeshes;
		const AABB bounds = SceneBounds::GetSceneBounds(*job.World, &loadingMeshes);
		if (!bounds.IsValid())
		{
			FinishScene(false, nullptr);
			return;
		}
		EditorCamera camera;
		camera.SetOrientation(c_ViewYaw, c_ViewPitch);
		const float radius = std::max(glm::length(bounds.GetExtents()), 1.0e-3f);
		camera.SetClipPlanes(std::max(radius * 0.01f, EditorCamera::c_MinNear), std::max(radius * 100.0f, 1.0f));
		if (job.Type == AssetType::Material)
		{
			// The sphere fills the view (its box's corners, which FitBounds frames, lie well outside it).
			const float sphereRadius = bounds.GetExtents().x;
			camera.SetTarget(bounds.GetCenter());
			camera.SetDistance(sphereRadius * c_SphereMargin / std::sin(glm::radians(camera.GetFOV()) * 0.5f));
		}
		else
		{
			camera.FitBounds(bounds, 1.0f);
		}

		if (!m_Renderer)
		{
			SceneRendererSpecification specification;
			specification.DebugName = "Thumbnails";
			specification.EntityIDs = false;
			specification.ShadowMapSize = 1024;
			specification.ShadowCascades = 1;
			specification.PrefetchDistance = 0.0f;
			m_Renderer = CreateScope<SceneRenderer>(specification);
		}
		// Thumbnails and the preview share the renderer: its targets follow the image's size (they change only when a preview
		// starts or ends).
		m_Renderer->SetViewportSize(job.Size, job.Size);
		SceneRenderOptions options;
		options.PreviewEnvironment = true;
		options.DrawScreenSpaceText = false;
		if (!m_Renderer->Render(*job.World, camera.GetSceneCamera(1.0f), nullptr, options))
		{
			FinishScene(false, nullptr);
			return;
		}
		const SceneRendererStats& stats = m_Renderer->GetStats();
		if (stats.PendingAssets > 0 || stats.PendingTextGlyphs > 0 || !loadingMeshes.empty())
		{
			job.QuietFrames = 0;
			return;
		}
		if (++job.QuietFrames < c_QuietFrames)
			return;
		std::string error;
		job.Readback = TextureReadback::Create(m_Renderer->GetOutputTexture(), {}, &error);
		if (!job.Readback)
		{
			ST_WARN("No thumbnail for {}: {}", job.Handle.ToString(), error);
			FinishScene(false, nullptr);
		}
	}

	bool ThumbnailCache::BuildScene(SceneJob& job, std::string* outError)
	{
		job.World = CreateRef<Scene>("Thumbnail");
		switch (job.Type)
		{
			case AssetType::Material:
			{
				Entity sphere = job.World->CreateEntity("Sphere");
				sphere.AddComponent<MeshRendererComponent>(MeshRendererComponent { BuiltinAssets::SphereMesh, job.Handle, true });
				return true;
			}
			case AssetType::Mesh:
			{
				Entity mesh = job.World->CreateEntity("Mesh");
				mesh.AddComponent<MeshRendererComponent>(MeshRendererComponent { job.Handle, UUID::Null(), true });
				return true;
			}
			case AssetType::Model:
			case AssetType::Prefab:
			{
				const Ref<EntityTemplate> entityTemplate = std::dynamic_pointer_cast<EntityTemplate>(m_Assets->GetAsset(job.Handle, AssetPriority::Low));
				if (!entityTemplate)
				{
					if (m_Assets->GetAssetState(job.Handle) == AssetState::Failed && outError)
						*outError = "it could not be loaded";
					return false;
				}
				std::string error;
				entityTemplate->Instantiate(*job.World, {}, &error);
				if (!error.empty())
				{
					if (outError)
						*outError = error;
					return false;
				}
				return true;
			}
			default:
				if (outError)
					*outError = fmt::format("{} assets get no rendered thumbnail", AssetTypeToString(job.Type));
				return false;
		}
	}

	void ThumbnailCache::FinishScene(bool success, Ref<ThumbnailImage> image)
	{
		const AssetHandle handle = m_SceneJob->Handle;
		const uint64_t key = m_SceneJob->Key;
		const bool preview = m_SceneJob->IsPreview;
		m_SceneJob->Loads.Cancel();
		m_SceneJob.reset();
		if (preview)
		{
			// Only current jobs finish (UpdateSceneJob); one that failed leaves the thumbnail in its place until a source changes.
			m_Preview.Working = false;
			if (success)
			{
				m_Preview.Image = std::move(image);
				m_Stats.PreviewsRendered++;
			}
			else
			{
				m_Stats.Failed++;
			}
			return;
		}
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end() || it->second.Key != key)
			return;
		it->second.Working = false;
		if (success)
			Finish(handle, key, std::move(image), false);
		else
			Fail(handle, key);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Textures, sources and the index
	////////////////////////////////////////////////////////////////////////////////

	void ThumbnailCache::Upload()
	{
		if ((m_Uploads.empty() && !m_Preview.Image) || !Renderer::IsInitialized())
			return;
		nvrhi::IDevice* device = Renderer::GetDevice();
		if (!m_UploadCommands)
			m_UploadCommands = device->createCommandList();
		std::vector<nvrhi::ResourceHandle> replaced;
		bool open = false;
		auto begin = [this, &open]()
		{
			if (!open)
			{
				m_UploadCommands->open();
				open = true;
			}
		};
		if (const Ref<ThumbnailImage> image = std::move(m_Preview.Image))
		{
			begin();
			// A preview that cannot be uploaded leaves the thumbnail in its place.
			if (nvrhi::TextureHandle texture = DisplayImage::Upload(m_UploadCommands, image->Width, image->Height, image->Pixels, "Preview"))
			{
				if (m_Preview.Texture)
					replaced.push_back(m_Preview.Texture);
				m_Preview.Texture = texture;
				m_Stats.Uploads++;
			}
		}
		for (uint32_t count = 0; count < m_Specification.MaxUploadsPerFrame && !m_Uploads.empty(); count++)
		{
			const AssetHandle handle = m_Uploads.front();
			m_Uploads.pop_front();
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end() || !it->second.Image)
				continue;
			Entry& entry = it->second;
			const Ref<ThumbnailImage> image = std::move(entry.Image);
			begin();
			nvrhi::TextureHandle texture = DisplayImage::Upload(m_UploadCommands, image->Width, image->Height, image->Pixels, "Thumbnail");
			if (!texture)
			{
				Fail(handle, entry.Key);
				continue;
			}
			if (entry.Texture)
				replaced.push_back(entry.Texture);
			entry.Texture = texture;
			m_Stats.Uploads++;
		}
		if (open)
		{
			m_UploadCommands->close();
			device->executeCommandList(m_UploadCommands);
		}
		if (!replaced.empty())
			Renderer::ReleaseDeferred(std::move(replaced));
	}

	void ThumbnailCache::Forget()
	{
		// Thumbnails nobody asked for in a while go, the least recently asked for first, once there are too many textures.
		std::vector<std::pair<uint64_t, AssetHandle>> idle;
		size_t textures = 0;
		for (const auto& [handle, entry] : m_Entries)
		{
			textures += entry.Texture ? 1 : 0;
			if (!entry.Working && !entry.Queued && m_Frame - entry.LastRequest > c_ForgetFrames)
				idle.emplace_back(entry.LastRequest, handle);
		}
		std::sort(idle.begin(), idle.end());
		std::vector<nvrhi::ResourceHandle> released;
		for (const auto& [lastRequest, handle] : idle)
		{
			Entry& entry = m_Entries.at(handle);
			if (entry.Texture && textures <= m_Specification.MaxGPUTextures)
				continue;
			if (entry.Texture)
			{
				released.push_back(entry.Texture);
				textures--;
			}
			SetSources(handle, entry, {});
			m_Entries.erase(handle);
		}
		if (!released.empty())
			Renderer::ReleaseDeferred(std::move(released));
		// The preview goes when it has not been shown for a while.
		if (m_Preview.Handle.IsValid() && !m_Preview.Working && m_Frame - m_Preview.LastRequest > c_ForgetFrames)
			ResetPreview();
		// Files of assets the project no longer has go too, once no import can still bring their assets.
		if (m_IndexLoaded && !m_Assets->HasPendingImports())
		{
			std::vector<std::filesystem::path> stale;
			for (auto it = m_Index.begin(); it != m_Index.end();)
			{
				if (m_Assets->IsHandleValid(it->first))
				{
					++it;
					continue;
				}
				stale.push_back(GetFile(it->first));
				it = m_Index.erase(it);
				m_IndexDirty = true;
			}
			if (!stale.empty())
			{
				JobSystem::SubmitIO([stale = std::move(stale)]()
				{
					for (const std::filesystem::path& file : stale)
						FileSystem::Remove(file);
				}, JobPriority::Low);
			}
		}
	}

	void ThumbnailCache::WriteIndex()
	{
		m_IndexDirty = false;
		m_IndexWrittenFrame = m_Frame;
		if (m_Specification.Directory.empty())
			return;
		nlohmann::json keys = nlohmann::json::object();
		for (const auto& [handle, key] : m_Index)
			keys[UUIDToJson(handle).get<std::string>()] = ToHex(key);
		std::string text = JsonUtils::Dump(nlohmann::json { { "Version", c_Version }, { "Keys", std::move(keys) } }, 1, '\t') + "\n";
		JobSystem::SubmitIO([text = std::move(text), file = m_Specification.Directory / c_IndexFile]()
		{
			if (!FileSystem::WriteText(file, text))
				ST_WARN("The thumbnail index could not be saved to '{}'", FileSystem::ToUTF8(file));
		}, JobPriority::Low);
	}

	uint64_t ThumbnailCache::ComputeKey(AssetHandle handle, std::vector<AssetHandle>& outSources) const
	{
		outSources = m_Assets->GetDependencyClosure(handle);
		uint64_t key = Hash::FNV1a(fmt::format("Thumbnail {} {}", c_Version, c_Size));
		for (AssetHandle source : outSources)
			key = Hash::FNV1a(fmt::format("{}:{:016X};", source.ToString(), m_Assets->GetContentStamp(source)), key);
		return key != 0 ? key : 1;
	}

	void ThumbnailCache::SetSources(AssetHandle handle, Entry& entry, std::vector<AssetHandle> sources)
	{
		for (AssetHandle source : entry.Sources)
		{
			auto it = m_Dependents.find(source);
			if (it == m_Dependents.end())
				continue;
			std::erase(it->second, handle);
			if (it->second.empty())
				m_Dependents.erase(it);
		}
		entry.Sources = std::move(sources);
		for (AssetHandle source : entry.Sources)
			m_Dependents[source].push_back(handle);
	}

	std::filesystem::path ThumbnailCache::GetFile(AssetHandle handle) const
	{
		return m_Specification.Directory / (UUIDToJson(handle).get<std::string>() + ".png");
	}

	////////////////////////////////////////////////////////////////////////////////
	// Texture thumbnails
	////////////////////////////////////////////////////////////////////////////////

	std::optional<ThumbnailImage> ThumbnailCache::MakeTextureThumbnail(const CookedTextureView& view, std::string* outError)
	{
		auto fail = [outError](std::string message) -> std::optional<ThumbnailImage>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};

		// The largest level that fits, else the smallest one the view holds (made smaller below).
		int chosen = -1;
		for (uint32_t level = 0; level < view.Layout.MipCount; level++)
		{
			const CookedTextureLevel& entry = view.Layout.Levels[level];
			if (view.GetLevelData(level).empty())
				continue;
			chosen = static_cast<int>(level);
			if (entry.Width <= c_Size && entry.Height <= c_Size)
				break;
		}
		if (chosen < 0)
			return fail("The texture data holds none of its levels");
		const CookedTextureLevel& level = view.Layout.Levels[static_cast<uint32_t>(chosen)];
		std::span<const uint8_t> data = view.GetLevelData(static_cast<uint32_t>(chosen));

		const TextureFormat format = GetDecodedFormat(view.Format);
		std::vector<uint8_t> decoded;
		if (IsBlockCompressedFormat(view.Format))
		{
			decoded.resize(static_cast<size_t>(GetMipByteSize(format, level.Width, level.Height)));
			if (!BlockCompression::DecodeLevel(view.Format, level.Width, level.Height, data, decoded))
				return fail(fmt::format("The {} level could not be decoded", TextureFormatToString(view.Format)));
			data = decoded;
		}

		ThumbnailImage image;
		image.Width = level.Width;
		image.Height = level.Height;
		const size_t texels = static_cast<size_t>(level.Width) * level.Height;
		image.Pixels.resize(texels * 4);
		if (data.size() < static_cast<size_t>(GetMipByteSize(format, level.Width, level.Height)))
			return fail("The level is shorter than its size");
		for (size_t texel = 0; texel < texels; texel++)
		{
			uint8_t* out = image.Pixels.data() + texel * 4;
			switch (format)
			{
				case TextureFormat::RGBA8:
				case TextureFormat::RGBA8SRGB:
					std::memcpy(out, data.data() + texel * 4, 4);
					break;
				case TextureFormat::R8:
					out[0] = out[1] = out[2] = data[texel];
					out[3] = 255;
					break;
				case TextureFormat::RG8:
				{
					// A normal map's X and Y, with Z reconstructed, shown as normal maps are (n * 0.5 + 0.5).
					const float x = static_cast<float>(data[texel * 2]) / 127.5f - 1.0f;
					const float y = static_cast<float>(data[texel * 2 + 1]) / 127.5f - 1.0f;
					const float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
					out[0] = data[texel * 2];
					out[1] = data[texel * 2 + 1];
					out[2] = static_cast<uint8_t>(std::lround((z * 0.5f + 0.5f) * 255.0f));
					out[3] = 255;
					break;
				}
				case TextureFormat::RGBA16F:
				{
					uint16_t halves[4];
					std::memcpy(halves, data.data() + texel * 8, sizeof(halves));
					for (int channel = 0; channel < 3; channel++)
						out[channel] = ToneMap(glm::unpackHalf1x16(halves[channel]));
					out[3] = static_cast<uint8_t>(std::lround(std::clamp(glm::unpackHalf1x16(halves[3]), 0.0f, 1.0f) * 255.0f));
					break;
				}
				case TextureFormat::RGBA32F:
				{
					float values[4];
					std::memcpy(values, data.data() + texel * 16, sizeof(values));
					for (int channel = 0; channel < 3; channel++)
						out[channel] = ToneMap(values[channel]);
					out[3] = static_cast<uint8_t>(std::lround(std::clamp(std::isfinite(values[3]) ? values[3] : 1.0f, 0.0f, 1.0f) * 255.0f));
					break;
				}
				default:
					return fail(fmt::format("{} textures get no thumbnail", TextureFormatToString(view.Format)));
			}
		}
		return FitImage(std::move(image), c_Size);
	}

}
