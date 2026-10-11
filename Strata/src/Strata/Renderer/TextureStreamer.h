#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Asset/AssetManagerExtension.h"
#include "Strata/Asset/AssetResidency.h"
#include "Strata/Core/Base.h"

#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class AssetManagerBase;
	class Texture;
	struct VisibleMaterialUse;

	// How a view wants the textures it drew (TextureStreamer::ReportView).
	struct TextureStreamingView
	{
		float ViewportHeight = 0.0f; // Pixels: VisibleMaterialUse::PixelSize is measured in them
		bool Streaming = true;       // False: every texture it drew at full resolution (SceneRendererSpecification::TextureStreaming)
		bool CameraCut = false;      // The view jumped since its previous frame: its reads go before older ones
	};

	// Which mip level each texture should have and in which order their levels are read: the texture streamer's decisions,
	// without the GPU (unit-tested on their own). Main thread.
	class TextureStreamingPolicy
	{
	public:
		// No need recorded: a texture keeps its tail (TextureStreamer drops it to the tail).
		static constexpr uint32_t c_NoLevel = std::numeric_limits<uint32_t>::max();
		// Detail rises at once and falls only after this many frames without a need of it.
		static constexpr uint32_t c_DropDelayFrames = 120;
		// A texture nobody needed for this many frames is forgotten (its tail stays).
		static constexpr uint32_t c_ForgetFrames = 2 * c_DropDelayFrames;

		// The level a texture with `mipCount` levels of `size` texels (its larger side) needs where it is drawn `pixels` tall
		// with its coordinates tiled `uvScale` times: floor(log2(size * uvScale / pixels)) + bias, clamped to the levels it
		// has. A texture drawn as many pixels as it has texels, or more, needs level 0.
		static uint32_t ComputeWantedLevel(uint32_t size, uint32_t mipCount, float uvScale, float pixels, int32_t bias = 0);

		// The rank of a read that raises a texture `missingLevels` levels where it covers `coverage` of a view's height: Low
		// priority (after the loads of whole assets) scored by missing levels times coverage, or Normal for a view that just
		// jumped (TextureStreamingView::CameraCut), so the new view's detail goes before what older views asked for.
		struct Rank
		{
			AssetPriority Priority = AssetPriority::Low;
			float Score = 0.0f;
		};
		static Rank RankRaise(uint32_t missingLevels, float coverage, bool cameraCut);

		// Records what a use of a texture in a view needs this frame: of several uses and views, the finest level, the largest
		// coverage, whether any view jumped, and whether any needs it whole (a view without streaming, which waits for every
		// level: such a texture keeps them also under memory pressure).
		void AddNeed(AssetHandle texture, uint32_t level, float coverage, bool cameraCut, bool whole = false);
		// Ends the frame: each texture's target follows the frame's needs, finer at once, coarser only once the finer level
		// was not needed for c_DropDelayFrames frames (c_NoLevel when nothing needed it that long). Needs start anew.
		void Update(uint64_t frame);

		struct Target
		{
			uint32_t Level = c_NoLevel;   // The level it should have
			float Coverage = 0.0f;        // The frame's largest coverage (0 when the frame did not need it)
			bool CameraCut = false;       // A view that needed it in the frame had just jumped
			bool Whole = false;           // A view of the frame needs every level (TextureStreamingView::Streaming false)
			uint64_t HeldFrame = 0;       // The latest frame that needed Level or finer
			uint64_t LastNeededFrame = 0; // The latest frame that needed it at all
		};
		const Target* Find(AssetHandle texture) const;
		const std::unordered_map<AssetHandle, Target>& GetTargets() const { return m_Targets; }
		void Forget(AssetHandle texture) { m_Targets.erase(texture); }
	private:
		struct Need
		{
			uint32_t Level = c_NoLevel;
			float Coverage = 0.0f;
			bool CameraCut = false;
			bool Whole = false;
		};
		std::unordered_map<AssetHandle, Need> m_Needs; // The frame's
		std::unordered_map<AssetHandle, Target> m_Targets;
	};

	// Keeps every texture of an asset manager at the mip level the screen needs (one per manager: an AssetManagerExtension
	// the renderer module registers). The manager loads textures partly (their header and mip tail, AssetPrefixFunction);
	// each frame the renderers report the materials they drew (ReportView, SceneRenderer does), from which the streamer
	// derives the level each texture needs (TextureStreamingPolicy), reads the missing levels' cooked bytes by range through
	// the manager's streaming queue (AssetManagerBase::RequestRange, ranked by missing levels times coverage, a camera cut's
	// first) and raises the texture with them within the frame's upload budgets (Texture::RaiseLevels: a new GPU texture,
	// its resident levels copied on the GPU, the new ones uploaded, the bindless slot swapped). Levels no longer needed for
	// c_DropDelayFrames frames are dropped, down to the tail, and under GPU texture pressure (ReleaseMemory) the least
	// needed textures lose their finest levels before the manager evicts whole assets; a texture in view keeps its tail, so
	// nothing that was drawn shows the loading fallback again. Main thread.
	class TextureStreamer : public AssetManagerExtension
	{
	public:
		// At most this many textures drop levels per frame because they are no longer needed (each is a GPU copy).
		static constexpr uint32_t c_MaxDropsPerFrame = 8;
		// After a read fails, the texture's levels are read again this many frames later at the earliest.
		static constexpr uint32_t c_RetryFrames = 60;

		explicit TextureStreamer(AssetManagerBase& manager);
		~TextureStreamer() override;

		TextureStreamer(const TextureStreamer&) = delete;
		TextureStreamer& operator=(const TextureStreamer&) = delete;

		// The streamer of a manager (null for managers made before the renderer module registered it).
		static TextureStreamer* Find(AssetManagerBase& manager);

		// What a view drew in its frame, for the levels its textures need (SceneRenderer reports after each frame it renders).
		void ReportView(const TextureStreamingView& view, std::span<const VisibleMaterialUse> materials);
		// Asks for a texture at `level` in this frame, like a view would (a preview at full resolution: 0; a sky light's
		// environment map). Ignored for textures that are not loaded.
		void RequestLevel(AssetHandle texture, uint32_t level, float coverage = 1.0f);
		// The texture with what a first load holds (its mip tail, the whole texture for small ones), for thumbnails and
		// previews: requests the load like GetAsset and never asks for finer levels (what views or RequestLevel need still
		// raises it). Null until it is loaded.
		static Ref<Texture> GetPreview(AssetManagerBase& manager, AssetHandle texture);

		// Added to every wanted level (positive: coarser, to save memory; negative: sharper).
		void SetLevelBias(int32_t bias) { m_LevelBias = bias; }
		int32_t GetLevelBias() const { return m_LevelBias; }
		const TextureStreamingPolicy& GetPolicy() const { return m_Policy; }

		// AssetManagerExtension
		bool RefinesPartialAssets(const AssetUploadContext* upload) const override;
		void OnUpdate(const AssetFinalizeContext& context) override;
		uint64_t ReleaseMemory(AssetMemoryPool pool, uint64_t bytes, const AssetFinalizeContext& context) override;
		void AddStats(AssetManagerStats& stats) const override;
	private:
		// What the streamer is doing with a texture.
		struct Streamed
		{
			uint64_t Read = 0;            // A range read under way (AssetManagerBase::RequestRange)
			uint32_t ReadLevel = 0;       // The finest level it reads
			uint32_t ReadResident = 0;    // The resident level when it was requested: it reads [ReadLevel, ReadResident)
			bool Changing = false;        // A level change under way (Texture::ContinueLevelChange)
			uint32_t ChangeFrom = 0;      // The resident level when the change started
			uint64_t RetryFrame = 0;      // No read before this frame (after a failure)
		};

		// The loaded texture (looked up without requesting it), or null.
		Ref<Texture> PeekTexture(AssetHandle handle) const;
		// The context of the next call, with what the frame's earlier calls of the streamer used of `context`'s budgets.
		AssetFinalizeContext MakeCallContext(const AssetFinalizeContext& context) const;
		// Continues a texture's change; returns whether it finished (done or failed).
		bool ContinueChange(AssetHandle handle, Texture& texture, Streamed& streamed, const AssetFinalizeContext& context);
		// Starts and finishes a drop (a GPU copy), reporting the memory change; returns the GPU texture bytes it freed.
		uint64_t Drop(AssetHandle handle, Texture& texture, uint32_t level, const AssetFinalizeContext& context);
		// The GPU texture bytes free in the manager's budget.
		uint64_t GetTextureRoom() const;
	private:
		AssetManagerBase& m_Manager;
		TextureStreamingPolicy m_Policy;
		std::unordered_map<AssetHandle, Streamed> m_Streamed; // Textures with a read or a change under way, or a retry pending
		uint64_t m_Frame = 0;
		int32_t m_LevelBias = 0;
		// What the frame had used of the budgets when the streamer's update began (MakeCallContext).
		uint64_t m_StartUploadedBytes = 0;
		float m_StartUploadMs = 0.0f;

		mutable std::mutex m_StatsMutex; // AddStats may be called on any thread
		AssetDetailStats m_Stats;
	};

}
