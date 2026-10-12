#pragma once

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/Base.h>
#include <Strata/Core/UUID.h>
#include <Strata/Renderer/CookedTexture.h>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Strata
{

	class EditorAssetManager;
	class Scene;
	class SceneRenderer;
	class TextureReadback;

	// The pixels of a thumbnail: RGBA8 display colors (sRGB-encoded, alpha straight), tightly packed rows, top row first.
	struct ThumbnailImage
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::vector<uint8_t> Pixels;
	};

	enum class ThumbnailState : uint8_t
	{
		None = 0,    // Not asked for (or forgotten again)
		Pending,     // Being read, decoded or rendered
		Ready,       // Its texture can be drawn
		Unavailable  // This asset gets none (its type has none, it failed, or there is no GPU): draw the type's icon
	};

	struct ThumbnailCacheStats
	{
		uint64_t TexturesDecoded = 0; // Made from a texture's mip tail
		uint64_t ScenesRendered = 0;  // Made by rendering a material, mesh, model or prefab
		uint64_t PreviewsRendered = 0; // Large previews (RequestPreview) rendered
		uint64_t LoadedFromDisk = 0;  // Read from the cache directory
		uint64_t FilesWritten = 0;
		uint64_t Failed = 0;          // Could not be made: their assets show the type's icon
		uint64_t Uploads = 0;         // GPU textures made
		uint64_t StoredBytesRead = 0; // Of textures' stored forms (headers and mip tails: never a whole chain)
		uint64_t MipWaitFrames = 0;   // Frames rendered images waited for finer mip levels of the textures they drew
		size_t Entries = 0;
		size_t GPUTextures = 0;
		size_t Pending = 0;
	};

	struct ThumbnailCacheSpecification
	{
		// Where the thumbnails are kept as PNG files with an index (<project>/.strata/Thumbnails); empty: in memory only.
		std::filesystem::path Directory;
		uint32_t MaxGPUTextures = 512;   // Beyond this, the thumbnails asked for longest ago are forgotten
		uint32_t MaxUploadsPerFrame = 8; // GPU textures made per frame
		uint32_t MaxJobs = 4;            // Reads and decodes running at once
		// A rendered thumbnail whose assets still load (or fail to) after this many frames is given up: the asset shows its
		// type's icon until it changes.
		uint32_t MaxSceneFrames = 600;
		// Textures arrive as their mip tail and stream in the levels a view drew them at (TextureStreamer): a rendered image
		// waits at most this many frames for those levels, then shows what is resident (a texture budget may keep them coarser).
		uint32_t MaxMipWaitFrames = 120;
		// After a source of a thumbnail changed (a reimport, a saved edit), it is made again once it stayed unchanged this many
		// frames; the old one shows meanwhile.
		uint32_t SettleFrames = 20;
		// Thumbnails nobody asked for in this many frames leave the queue (asked for again, they are queued again).
		uint32_t RequestFrames = 3;
	};

	// Thumbnails of a project's assets for the Content Browser and the Asset tab: textures from the levels of their mip tail
	// that fit c_Size (read by byte range, so a thumbnail never loads a texture's chain), materials on a sphere and meshes,
	// models and prefabs framed, rendered under the preview lighting by an offscreen SceneRenderer one at a time and read back
	// without stalling (TextureReadback). They are kept as PNG files in the cache directory, keyed by the content stamps of the
	// asset and everything it depends on (EditorAssetManager::GetContentStamp), so a reimport or a saved edit makes them again
	// and an unchanged project reads them back. Assets of other types, and editors without a GPU, get none. The Asset tab's
	// large preview of the asset it shows (RequestPreview) is rendered the same way at its own size, in memory only.
	//
	// Main thread. Reads, decodes and encodes run as jobs; the destructor waits for the reads under way, which use the asset
	// manager (they are small: a header and a mip tail; reads that have not started never do), so the cache must go before
	// the manager does.
	class ThumbnailCache
	{
	public:
		static constexpr uint32_t c_Size = 128;
		// Large previews (RequestPreview) are rendered at the size asked for, rounded up to a multiple of c_PreviewStep (a panel
		// that is resized a little keeps its preview) and at most c_MaxPreviewSize on a side.
		static constexpr uint32_t c_PreviewStep = 32;
		static constexpr uint32_t c_MaxPreviewSize = 512;
		// Part of the cache files' key: bump it when thumbnails would look different (rendering or decoding changed), so that
		// cached ones are made again.
		static constexpr uint32_t c_Version = 1;

		ThumbnailCache(const Ref<EditorAssetManager>& assets, ThumbnailCacheSpecification specification);
		~ThumbnailCache();

		ThumbnailCache(const ThumbnailCache&) = delete;
		ThumbnailCache& operator=(const ThumbnailCache&) = delete;

		// Whether assets of a type get thumbnails (textures, materials, meshes, models and prefabs).
		static bool HasThumbnail(AssetType type);

		// Once per frame: takes finished reads and decodes, renders the current scene thumbnail, uploads finished images,
		// starts the work asked for, notices changed sources and forgets thumbnails nobody asked for in a long time.
		void Update();

		// The thumbnail of an asset to draw (an RGBA8 texture of display colors, at most c_Size on a side, drawn as it is);
		// null until it is ready and for assets that get none. Asks for it: call it every frame it is shown, for what is on
		// screen only.
		nvrhi::ITexture* Request(AssetHandle handle);
		// A large preview of a material, mesh, model or prefab, rendered at `size` pixels on a side (rounded, see
		// c_PreviewStep) for the Asset tab: one asset at a time, kept in memory only, and rendered before the thumbnails that
		// wait. Until it is ready (and for other assets, or when it fails) it is the asset's thumbnail (Request). Call it every
		// frame it is shown; it is rendered again after its sources changed, like thumbnails.
		nvrhi::ITexture* RequestPreview(AssetHandle handle, uint32_t size);
		ThumbnailState GetState(AssetHandle handle) const;
		// Forgets every thumbnail, with its file: each is made again when it is next asked for (tools, and perf runs that
		// measure making them). Work under way finishes unused.
		void Clear();
		// Whether work is queued or running (tests and perf runs wait for it to end).
		bool IsBusy() const;
		ThumbnailCacheStats GetStats() const;

		// The thumbnail of a cooked texture from the largest level the view holds that fits c_Size on a side (a view of the
		// header and mip tail, ParseCookedTexturePrefix, is enough): block-compressed levels are decoded; one channel shows
		// as gray, two (normal maps) with Z reconstructed, HDR tone mapped and encoded. nullopt with the reason otherwise.
		static std::optional<ThumbnailImage> MakeTextureThumbnail(const CookedTextureView& view, std::string* outError = nullptr);
	private:
		struct Entry
		{
			ThumbnailState State = ThumbnailState::None;
			AssetType Type = AssetType::None;
			uint64_t Key = 0;                 // Of the thumbnail shown (or being made)
			std::vector<AssetHandle> Sources; // The asset and what it depends on: a change of one may change it
			uint64_t LastRequest = 0;         // Frame it was last asked for
			uint64_t DirtySince = 0;          // Frame a source changed since it was made (0: none)
			bool Queued = false;
			bool Working = false;             // A read, decode or render is under way
			Ref<ThumbnailImage> Image;        // Until it is uploaded
			nvrhi::TextureHandle Texture;
		};

		// The job results the main thread takes in Update (shared with the jobs, which never touch the cache).
		struct SharedState;

		// The large preview (RequestPreview): one asset at a time.
		struct Preview
		{
			AssetHandle Handle = UUID::Null();
			AssetType Type = AssetType::None;
			uint32_t Size = 0;                // Asked for (rounded)
			uint64_t Key = 0;                 // Of the sources the texture (or the render under way) was made from
			std::vector<AssetHandle> Sources;
			uint64_t LastRequest = 0;
			uint64_t DirtySince = 0;          // Frame a source changed since it was made (0: none)
			bool Wanted = false;              // To be rendered (another asset or size, or changed sources)
			bool Working = false;
			Ref<ThumbnailImage> Image;        // Until it is uploaded
			nvrhi::TextureHandle Texture;
		};

		// What a scene thumbnail being rendered needs across frames.
		struct SceneJob
		{
			AssetHandle Handle = UUID::Null();
			AssetType Type = AssetType::None;
			uint64_t Key = 0;
			uint32_t Size = c_Size;  // Of the image
			bool IsPreview = false;  // The large preview, not a thumbnail
			AssetGroupRequest Loads;
			Ref<Scene> World; // The asset, alone under the preview lighting
			bool Built = false;
			uint32_t Frames = 0;
			uint32_t QuietFrames = 0;
			uint32_t MipWaitFrames = 0; // Frames it waited for its textures' levels (ThumbnailCacheSpecification::MaxMipWaitFrames)
			Scope<TextureReadback> Readback;
		};

		void ApplyResults();
		void StartPreview();
		void ResetPreview();
		bool IsSceneJobCurrent(const SceneJob& job) const;
		void StartQueued();
		bool Start(AssetHandle handle, Entry& entry);
		void StartTextureJob(AssetHandle handle, uint64_t key);
		void StartDiskJob(AssetHandle handle, uint64_t key);
		void UpdateSceneJob();
		bool BuildScene(SceneJob& job, std::string* outError);
		// Whether a texture the renderer drew in its last frame is coarser than the level the views want of it (its finer
		// levels are still streaming in).
		bool HasTexturesBelowWanted() const;
		void FinishScene(bool success, Ref<ThumbnailImage> image);
		void Finish(AssetHandle handle, uint64_t key, Ref<ThumbnailImage> image, bool fromDisk);
		void Fail(AssetHandle handle, uint64_t key);
		void Upload();
		void WatchSources();
		void Forget();
		void WriteIndex();
		uint64_t ComputeKey(AssetHandle handle, std::vector<AssetHandle>& outSources) const;
		void SetSources(AssetHandle handle, Entry& entry, std::vector<AssetHandle> sources);
		std::filesystem::path GetFile(AssetHandle handle) const;
	private:
		Ref<EditorAssetManager> m_Assets;
		ThumbnailCacheSpecification m_Specification;
		Ref<SharedState> m_Shared;
		std::unordered_map<AssetHandle, Entry> m_Entries;
		std::unordered_map<AssetHandle, std::vector<AssetHandle>> m_Dependents; // Source -> entries that show it
		std::deque<AssetHandle> m_Queue;
		std::deque<AssetHandle> m_Uploads;
		std::optional<SceneJob> m_SceneJob;
		Preview m_Preview;
		Scope<SceneRenderer> m_Renderer;
		nvrhi::CommandListHandle m_UploadCommands;
		// The disk index: the key of each thumbnail file (read once, written after changes).
		std::unordered_map<AssetHandle, uint64_t> m_Index;
		bool m_IndexLoaded = false;
		bool m_IndexDirty = false;
		uint64_t m_IndexWrittenFrame = 0;
		uint64_t m_Frame = 1;
		uint64_t m_ContentVersion = 0;
		uint32_t m_JobsRunning = 0;
		ThumbnailCacheStats m_Stats;
	};

}
