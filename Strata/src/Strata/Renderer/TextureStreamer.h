#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Asset/AssetManagerExtension.h"
#include "Strata/Asset/AssetResidency.h"
#include "Strata/Core/Base.h"
#include "Strata/Math/AABB.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <limits>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class AssetManagerBase;
	class Texture;
	struct SceneCamera;
	struct VisibleTextureUse;

	// How a view wants the textures it drew (TextureStreamer::ReportView).
	struct TextureStreamingView
	{
		float ViewportHeight = 0.0f; // Pixels: VisibleTextureUse::PixelSize is measured in them
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

		// The level a texture with `mipCount` levels of `size` texels (its larger side) needs where one unit of its
		// coordinates spans `pixels` on screen with the coordinates tiled `uvScale` times (MaterialProperties::UVTiling):
		// floor(log2(size * uvScale / pixels)), clamped to the levels it has. A texture drawn as many pixels as it has
		// texels, or more, needs level 0; one drawn no larger than a pixel the coarsest.
		static uint32_t ComputeWantedLevel(uint32_t size, uint32_t mipCount, float uvScale, float pixels);

		// What a view's projection makes of distances: the pixels one world unit spans at distance 1 (perspective: the view's
		// height over twice the tangent of half its vertical field of view) or anywhere (orthographic: over its height).
		struct ViewScale
		{
			glm::vec3 Position = glm::vec3(0.0f);
			float UnitPixels = 0.0f;
			float Near = 0.0f;
			bool Orthographic = false;
		};
		static ViewScale GetViewScale(const SceneCamera& camera, float viewportHeight);
		// The pixels one world unit spans at the point of `bounds` nearest to the camera (at least the near plane away), on a
		// surface facing it: the finest detail anything inside the bounds shows. A surface seen at a grazing angle spans as
		// much across the view direction, which is what anisotropic filtering samples by.
		static float GetWorldPixels(const ViewScale& view, const AABB& bounds);
		// The pixels one unit of a submesh's texture coordinates spans where a world unit spans `worldPixels`, for the
		// submesh's Submesh::UVDensity drawn with `axisScale` (the lengths of its world transform's axes: a surface facing X
		// stretches with the scales along Y and Z): of the surfaces facing each axis, those with the fewest texture
		// coordinates per world unit, which show each texel largest. Coordinates that do not change over the surfaces sample
		// level 0 (no derivatives): the largest float.
		static float GetUVPixels(float worldPixels, const glm::vec3& uvDensity, const glm::vec3& axisScale);

		// The rank of a read that raises a texture `missingLevels` levels where it covers `coverage` of a view's height: Low
		// priority (after the loads of whole assets) scored by missing levels times coverage, or Normal for a view that just
		// jumped (TextureStreamingView::CameraCut), so the new view's detail goes before what older views asked for.
		struct Rank
		{
			AssetPriority Priority = AssetPriority::Low;
			float Score = 0.0f;

			// Served before `other` (a higher priority, or a higher score within the same priority).
			bool Outranks(const Rank& other) const
			{
				return Priority != other.Priority ? static_cast<uint8_t>(Priority) < static_cast<uint8_t>(other.Priority) : Score > other.Score;
			}
		};
		static Rank RankRaise(uint32_t missingLevels, float coverage, bool cameraCut);

		// A view jumped between two frames (TextureStreamingView::CameraCut): it moved more than c_CameraCutDistance world
		// units or turned more than 45 degrees (`previousForward` and `forward` are unit vectors).
		static constexpr float c_CameraCutDistance = 10.0f;
		static constexpr float c_CameraCutCosine = 0.7071f;
		static bool IsCameraCut(const glm::vec3& previousPosition, const glm::vec3& previousForward, const glm::vec3& position, const glm::vec3& forward);

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
	// each frame the renderers report the textures they drew, with how finely each material showed them (ReportView,
	// SceneRenderer does: VisibleTextureUse, resolved by the frame, so reports look nothing up), from which the streamer
	// derives the level each texture needs (TextureStreamingPolicy), reads the missing levels' cooked bytes by range through
	// the manager's streaming queue (AssetManagerBase::RequestRange, ranked by missing levels times coverage, a camera cut's
	// first; queued reads rise with their texture's rank) and raises the texture with them within the frame's upload
	// budgets (Texture::RaiseLevels: a new GPU texture, its resident levels copied on the GPU, the new ones uploaded, the
	// bindless slot swapped). The GPU texture budget's room (less what the reads under way will add) goes to the textures
	// below their level in rank order: one whose level does not fit takes the finest levels of textures needed less
	// (MakeRoom), else gets the finest level the room holds. Levels no longer needed for c_DropDelayFrames frames are
	// dropped, down to the tail, and under GPU texture pressure (ReleaseMemory) the least needed textures lose their finest
	// levels, as many as the budget takes, before the manager evicts whole assets; a texture in view keeps its tail, so
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
		void ReportView(const TextureStreamingView& view, std::span<const VisibleTextureUse> textures);
		// Asks for a texture at `level` in this frame, like a view would (a preview at full resolution: 0; a sky light's
		// environment map). Ignored for textures that are not loaded.
		void RequestLevel(AssetHandle texture, uint32_t level, float coverage = 1.0f);
		// The texture with what a first load holds (its mip tail, the whole texture for small ones), for thumbnails and
		// previews: requests the load like GetAsset and never asks for finer levels (what views or RequestLevel need still
		// raises it). Null until it is loaded.
		static Ref<Texture> GetPreview(AssetManagerBase& manager, AssetHandle texture);

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
			TextureStreamingPolicy::Rank ReadRank; // What the read is queued with (raised as the views need it more)
			bool Changing = false;        // A level change under way (Texture::ContinueLevelChange)
			bool ChangeForBudget = false; // The change drops levels for a budget (ReleaseMemory, or room for a raise)
			uint32_t ChangeFrom = 0;      // The resident level when the change started
			uint64_t RetryFrame = 0;      // No read before this frame (after a failure)
		};

		// A texture that may lose its finest levels to a budget (ReleaseMemory) or to the raise of a texture needed more
		// (MakeRoom): anything but what a view of the latest frame needs whole.
		struct DropCandidate
		{
			AssetHandle Handle;
			bool Needed = false;      // The latest frame needed it
			float Coverage = 0.0f;    // Its largest coverage in that frame
			uint64_t LastNeededFrame = 0;
		};
		// A texture below its target, ranked for the room in the budget (OnUpdate).
		struct Raise
		{
			AssetHandle Handle;
			Ref<Texture> Target;
			uint32_t Level = 0;       // The level it should have
			uint32_t Resident = 0;
			float Need = 0.0f;        // Its coverage; infinite for what a view needs whole
			TextureStreamingPolicy::Rank Rank;
		};

		// The loaded texture (looked up without requesting it), or null.
		Ref<Texture> PeekTexture(AssetHandle handle) const;
		// The context of the next call, with what the frame's earlier calls of the streamer used of `context`'s budgets.
		AssetFinalizeContext MakeCallContext(const AssetFinalizeContext& context) const;
		// Continues a texture's change; returns whether it finished (done or failed).
		bool ContinueChange(AssetHandle handle, Texture& texture, Streamed& streamed, const AssetFinalizeContext& context);
		// Starts and finishes a drop (a GPU copy), reporting the memory change; returns the GPU texture bytes it freed.
		uint64_t Drop(AssetHandle handle, Texture& texture, uint32_t level, bool forBudget, const AssetFinalizeContext& context);
		// The GPU texture bytes free in the manager's budget.
		uint64_t GetTextureRoom() const;
		// The textures that may lose levels, least needed first: those the latest frame did not need (longest unneeded
		// first), then those it needed, smallest on screen first.
		std::vector<DropCandidate> GetDropCandidates() const;
		// Frees about `bytes` of a texture's GPU memory for a budget: a change under way is cancelled (its new texture comes on
		// top of the resident one) and its read abandoned, then its finest levels go, only as many as `bytes` takes, never its
		// tail. Returns the GPU texture bytes freed.
		uint64_t ReleaseLevels(AssetHandle handle, uint64_t bytes, const AssetFinalizeContext& context);
		// What ReleaseLevels could free of a texture (its levels above the tail, a change under way) with the room its read
		// holds, without changing anything.
		uint64_t GetReleasableBytes(AssetHandle handle) const;
		// What MakeRoom could gain for a texture needed as much as `need`, without changing anything (`drops`: those the frame
		// made so far).
		uint64_t GetRoomFromLessNeeded(AssetHandle raising, float need, const std::vector<DropCandidate>& candidates, uint32_t drops) const;
		// Room for the raise of a texture needed as much as `need` that misses `bytes`: textures needed less give up their
		// finest levels (and their reads), least needed first, within the frame's drops. A texture never takes from one
		// needed as much or more, so two textures never take levels from each other in turns. Returns the room gained.
		uint64_t MakeRoom(AssetHandle raising, uint64_t bytes, float need, std::vector<DropCandidate>& candidates, uint32_t& drops,
			const AssetFinalizeContext& context);
	private:
		AssetManagerBase& m_Manager;
		TextureStreamingPolicy m_Policy;
		std::unordered_map<AssetHandle, Streamed> m_Streamed; // Textures with a read or a change under way, or a retry pending
		uint64_t m_Frame = 0;
		// What the frame had used of the budgets when the streamer's update began (MakeCallContext).
		uint64_t m_StartUploadedBytes = 0;
		float m_StartUploadMs = 0.0f;

		mutable std::mutex m_StatsMutex; // AddStats may be called on any thread
		AssetDetailStats m_Stats;
	};

}
