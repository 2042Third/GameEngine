#include "stpch.h"
#include "Strata/Renderer/TextureStreamer.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/RendererUploadContext.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Renderer/Texture.h"

#include <algorithm>
#include <cmath>

namespace Strata
{

	////////////////////////////////////////////////////////////////////////////////
	// TextureStreamingPolicy
	////////////////////////////////////////////////////////////////////////////////

	uint32_t TextureStreamingPolicy::ComputeWantedLevel(uint32_t size, uint32_t mipCount, float uvScale, float pixels, int32_t bias)
	{
		if (mipCount == 0)
			return 0;
		const int32_t coarsest = static_cast<int32_t>(mipCount) - 1;
		const float texels = static_cast<float>(size) * std::max(std::isfinite(uvScale) ? uvScale : 1.0f, 0.0f);
		// Drawn no larger than a pixel (or not at all): the coarsest level is all it shows.
		if (!(pixels > 1.0f) || !std::isfinite(pixels))
			return static_cast<uint32_t>(std::clamp(coarsest + bias, 0, coarsest));
		if (!(texels > pixels))
			return static_cast<uint32_t>(std::clamp(bias, 0, coarsest));
		const int32_t level = static_cast<int32_t>(std::floor(std::log2(texels / pixels)));
		return static_cast<uint32_t>(std::clamp(level + bias, 0, coarsest));
	}

	TextureStreamingPolicy::Rank TextureStreamingPolicy::RankRaise(uint32_t missingLevels, float coverage, bool cameraCut)
	{
		Rank rank;
		rank.Priority = cameraCut ? AssetPriority::Normal : AssetPriority::Low;
		rank.Score = static_cast<float>(missingLevels) * std::clamp(std::isfinite(coverage) ? coverage : 0.0f, 0.0f, 1.0f);
		return rank;
	}

	void TextureStreamingPolicy::AddNeed(AssetHandle texture, uint32_t level, float coverage, bool cameraCut, bool whole)
	{
		Need& need = m_Needs[texture];
		need.Level = std::min(need.Level, level);
		need.Coverage = std::max(need.Coverage, std::isfinite(coverage) ? coverage : 0.0f);
		need.CameraCut = need.CameraCut || cameraCut;
		need.Whole = need.Whole || whole;
	}

	void TextureStreamingPolicy::Update(uint64_t frame)
	{
		for (auto it = m_Targets.begin(); it != m_Targets.end();)
		{
			Target& target = it->second;
			auto need = m_Needs.find(it->first);
			if (need != m_Needs.end())
			{
				// Finer at once; coarser once the finer level was not needed for a while.
				if (need->second.Level <= target.Level || frame - std::min(target.HeldFrame, frame) >= c_DropDelayFrames)
				{
					target.Level = need->second.Level;
					target.HeldFrame = frame;
				}
				target.Coverage = need->second.Coverage;
				target.CameraCut = need->second.CameraCut;
				target.Whole = need->second.Whole;
				target.LastNeededFrame = frame;
				m_Needs.erase(need);
				++it;
				continue;
			}

			target.Coverage = 0.0f;
			target.CameraCut = false;
			target.Whole = false;
			const uint64_t unneeded = frame - std::min(target.LastNeededFrame, frame);
			if (unneeded >= c_ForgetFrames)
			{
				it = m_Targets.erase(it);
				continue;
			}
			if (frame - std::min(target.HeldFrame, frame) >= c_DropDelayFrames)
				target.Level = c_NoLevel;
			++it;
		}
		// Textures needed for the first time (or again after they were forgotten).
		for (const auto& [texture, need] : m_Needs)
			m_Targets[texture] = Target { need.Level, need.Coverage, need.CameraCut, need.Whole, frame, frame };
		m_Needs.clear();
	}

	const TextureStreamingPolicy::Target* TextureStreamingPolicy::Find(AssetHandle texture) const
	{
		auto it = m_Targets.find(texture);
		return it != m_Targets.end() ? &it->second : nullptr;
	}

	////////////////////////////////////////////////////////////////////////////////
	// TextureStreamer
	////////////////////////////////////////////////////////////////////////////////

	TextureStreamer::TextureStreamer(AssetManagerBase& manager)
		: m_Manager(manager)
	{
	}

	TextureStreamer::~TextureStreamer()
	{
		// Reads under way are abandoned with the manager (it waits for their jobs); textures keep their resident levels.
		for (const auto& [handle, streamed] : m_Streamed)
		{
			if (streamed.Read != 0)
				m_Manager.CancelRange(streamed.Read);
		}
	}

	TextureStreamer* TextureStreamer::Find(AssetManagerBase& manager)
	{
		return manager.FindExtension<TextureStreamer>();
	}

	Ref<Texture> TextureStreamer::GetPreview(AssetManagerBase& manager, AssetHandle texture)
	{
		const Ref<Asset> asset = manager.GetAsset(texture);
		return asset && asset->GetType() == AssetType::Texture ? std::static_pointer_cast<Texture>(asset) : nullptr;
	}

	Ref<Texture> TextureStreamer::PeekTexture(AssetHandle handle) const
	{
		const Ref<Asset> asset = m_Manager.PeekLoadedAsset(handle);
		return asset && asset->GetType() == AssetType::Texture ? std::static_pointer_cast<Texture>(asset) : nullptr;
	}

	bool TextureStreamer::RefinesPartialAssets(const AssetUploadContext* upload) const
	{
		// Levels are raised through the renderer's upload queue only.
		return dynamic_cast<const RendererUploadContext*>(upload) != nullptr;
	}

	void TextureStreamer::ReportView(const TextureStreamingView& view, std::span<const VisibleMaterialUse> materials)
	{
		const float viewportHeight = std::max(view.ViewportHeight, 1.0f);
		for (const VisibleMaterialUse& use : materials)
		{
			const Ref<Asset> asset = m_Manager.PeekLoadedAsset(use.Material);
			if (!asset || asset->GetType() != AssetType::Material)
				continue;
			const MaterialProperties& properties = std::static_pointer_cast<Material>(asset)->GetProperties();
			for (AssetHandle handle : { properties.BaseColorMap, properties.MetallicRoughnessMap, properties.NormalMap, properties.OcclusionMap, properties.EmissiveMap })
			{
				if (!handle.IsValid())
					continue;
				const Ref<Texture> texture = PeekTexture(handle);
				if (!texture)
					continue; // Its load is the renderer's request
				const uint32_t size = std::max(texture->GetWidth(), texture->GetHeight());
				const uint32_t level = view.Streaming ? TextureStreamingPolicy::ComputeWantedLevel(size, texture->GetMipCount(), use.MaxUVScale, use.PixelSize, m_LevelBias)
					: 0;
				m_Policy.AddNeed(handle, level, use.PixelSize / viewportHeight, view.CameraCut, !view.Streaming);
			}
		}
	}

	void TextureStreamer::RequestLevel(AssetHandle texture, uint32_t level, float coverage)
	{
		if (PeekTexture(texture))
			m_Policy.AddNeed(texture, level, coverage, false);
	}

	AssetFinalizeContext TextureStreamer::MakeCallContext(const AssetFinalizeContext& context) const
	{
		AssetFinalizeContext call = context;
		const uint64_t usedBytes = context.UploadedBytes ? *context.UploadedBytes - std::min(*context.UploadedBytes, m_StartUploadedBytes) : 0;
		const float usedMs = context.UploadMs ? std::max(0.0f, *context.UploadMs - m_StartUploadMs) : 0.0f;
		call.UploadBudget = context.UploadBudget > usedBytes ? context.UploadBudget - usedBytes : 0;
		call.UploadMsBudget = std::max(0.0f, context.UploadMsBudget - usedMs);
		call.FirstOfFrame = context.FirstOfFrame && usedBytes == 0;
		return call;
	}

	uint64_t TextureStreamer::GetTextureRoom() const
	{
		const AssetResidencyBudgets budgets = m_Manager.GetResidencyBudgets();
		const uint64_t budget = budgets.GetPoolBudget(AssetMemoryPool::GpuTextures);
		if (budget == AssetResidencyBudgets::c_Unlimited)
			return AssetResidencyBudgets::c_Unlimited;
		const uint64_t resident = m_Manager.GetResidentMemory().GpuTextures;
		return budget > resident ? budget - resident : 0;
	}

	bool TextureStreamer::ContinueChange(AssetHandle handle, Texture& texture, Streamed& streamed, const AssetFinalizeContext& context)
	{
		const uint32_t target = texture.GetChangeLevel();
		const AssetFinalizeResult result = texture.ContinueLevelChange(MakeCallContext(context));
		m_Manager.UpdateMemoryUsage(handle, texture);
		if (result == AssetFinalizeResult::Pending || result == AssetFinalizeResult::AwaitingGPU)
			return false;
		streamed.Changing = false;
		if (result == AssetFinalizeResult::Done)
		{
			std::scoped_lock<std::mutex> lock(m_StatsMutex);
			if (target < streamed.ChangeFrom)
				m_Stats.LevelsRaised += streamed.ChangeFrom - target;
			else
				m_Stats.LevelsDropped += target - streamed.ChangeFrom;
		}
		return true;
	}

	uint64_t TextureStreamer::Drop(AssetHandle handle, Texture& texture, uint32_t level, const AssetFinalizeContext& context)
	{
		const uint64_t before = texture.GetMemoryUsage().GpuTextures;
		const uint32_t resident = texture.GetResidentLevel();
		if (!texture.DropLevels(level))
			return 0;
		Streamed& streamed = m_Streamed[handle];
		streamed.Changing = true;
		streamed.ChangeFrom = resident;
		// A drop is one GPU copy: it finishes in this call.
		if (!ContinueChange(handle, texture, streamed, context))
		{
			// Not recorded (no batch): it waits like a raise would.
			return 0;
		}
		const uint64_t after = texture.GetMemoryUsage().GpuTextures;
		return before > after ? before - after : 0;
	}

	void TextureStreamer::OnUpdate(const AssetFinalizeContext& context)
	{
		ST_PROFILE_FUNCTION();
		m_Frame++;
		m_Policy.Update(m_Frame);
		m_StartUploadedBytes = context.UploadedBytes ? *context.UploadedBytes : 0;
		m_StartUploadMs = context.UploadMs ? *context.UploadMs : 0.0f;
		const RendererUploadContext* upload = dynamic_cast<const RendererUploadContext*>(context.Upload);
		uint64_t streamedBytes = 0;

		// Changes under way go on first (uploads of raised levels), and reads that completed start theirs.
		for (auto it = m_Streamed.begin(); it != m_Streamed.end();)
		{
			const AssetHandle handle = it->first;
			Streamed& streamed = it->second;
			const Ref<Texture> texture = PeekTexture(handle);
			if (!texture)
			{
				// Evicted, unloaded or replaced by a reload: what was read for it is of no use.
				if (streamed.Read != 0)
					m_Manager.CancelRange(streamed.Read);
				it = m_Streamed.erase(it);
				continue;
			}
			if (streamed.Changing && upload)
				ContinueChange(handle, *texture, streamed, context);

			if (streamed.Read != 0 && !streamed.Changing)
			{
				std::vector<uint8_t> data;
				std::string error;
				const AssetRangeStatus status = m_Manager.PollRange(streamed.Read, data, &error);
				if (status != AssetRangeStatus::Pending)
					streamed.Read = 0;
				if (status == AssetRangeStatus::Complete)
				{
					streamedBytes += data.size();
					// Raised as far as it is still wanted (a prefix of what was read: the finer levels come last), once its
					// resident levels are what they were when it was read.
					const TextureStreamingPolicy::Target* target = m_Policy.Find(handle);
					const uint32_t wanted = target ? std::max(target->Level == TextureStreamingPolicy::c_NoLevel ? streamed.ReadResident : target->Level, streamed.ReadLevel)
						: streamed.ReadResident;
					if (texture->GetResidentLevel() == streamed.ReadResident && wanted < streamed.ReadResident && texture->CanChangeLevels())
					{
						const CookedTextureByteRange range = texture->GetCookedLayout().GetLevelsByteRange(wanted, streamed.ReadResident);
						if (range.Size <= data.size())
						{
							data.resize(static_cast<size_t>(range.Size));
							if (texture->RaiseLevels(wanted, std::move(data)))
							{
								streamed.Changing = true;
								streamed.ChangeFrom = streamed.ReadResident;
								m_Manager.UpdateMemoryUsage(handle, *texture);
								if (upload)
									ContinueChange(handle, *texture, streamed, context);
							}
						}
					}
				}
				else if (status == AssetRangeStatus::Failed || status == AssetRangeStatus::Unknown)
				{
					ST_CORE_WARN("Texture streaming: the levels of '{}' could not be read: {}", texture->GetSpecification().DebugName, error);
					streamed.RetryFrame = m_Frame + c_RetryFrames;
				}
			}

			if (streamed.Read == 0 && !streamed.Changing && streamed.RetryFrame <= m_Frame)
				it = m_Streamed.erase(it);
			else
				++it;
		}

		// New reads for textures below their target, and drops of levels no longer needed. The room left in the budget is
		// looked up once something would raise.
		std::optional<uint64_t> room;
		uint32_t drops = 0;
		uint32_t belowWanted = 0;
		for (const auto& [handle, target] : m_Policy.GetTargets())
		{
			const Ref<Texture> texture = PeekTexture(handle);
			if (!texture)
				continue;
			const uint32_t tail = texture->GetTailLevel();
			const uint32_t level = target.Level == TextureStreamingPolicy::c_NoLevel ? tail : std::min(target.Level, texture->GetMipCount() - 1);
			texture->SetWantedLevel(level);
			const uint32_t resident = texture->GetResidentLevel();
			if (level < resident)
				belowWanted++;
			auto streamed = m_Streamed.find(handle);
			const bool busy = streamed != m_Streamed.end() && (streamed->second.Read != 0 || streamed->second.Changing || streamed->second.RetryFrame > m_Frame);
			if (busy || !texture->CanChangeLevels() || !upload || !m_Manager.HasStoredForm(handle))
				continue;

			if (level < resident)
			{
				// What the raise adds once its new texture replaced the resident one: read only what the budget can hold.
				const uint64_t added = texture->GetLevelBytes(level, resident);
				if (!room)
					room = GetTextureRoom();
				if (*room != AssetResidencyBudgets::c_Unlimited && added > *room)
					continue;
				const CookedTextureByteRange range = texture->GetCookedLayout().GetLevelsByteRange(level, resident);
				const TextureStreamingPolicy::Rank rank = TextureStreamingPolicy::RankRaise(resident - level, target.Coverage, target.CameraCut);
				const uint64_t read = m_Manager.RequestRange(handle, range.Offset, range.Size, rank.Priority, rank.Score);
				if (read == 0)
					continue;
				Streamed& entry = m_Streamed[handle];
				entry.Read = read;
				entry.ReadLevel = level;
				entry.ReadResident = resident;
				if (*room != AssetResidencyBudgets::c_Unlimited)
					*room -= added;
			}
			else if (level > resident && drops < c_MaxDropsPerFrame && m_Manager.ArePartialLoadsActive())
			{
				const uint64_t freed = Drop(handle, *texture, level, context);
				if (freed > 0)
					drops++;
				if (room && *room != AssetResidencyBudgets::c_Unlimited)
					*room += freed;
			}
		}

		std::scoped_lock<std::mutex> lock(m_StatsMutex);
		m_Stats.BytesStreamed += streamedBytes;
		m_Stats.BytesStreamedLastFrame = streamedBytes;
		m_Stats.AssetsBelowWanted = belowWanted;
		m_Stats.Reads = 0;
		m_Stats.Changes = 0;
		for (const auto& [handle, streamed] : m_Streamed)
		{
			m_Stats.Reads += streamed.Read != 0 ? 1u : 0u;
			m_Stats.Changes += streamed.Changing ? 1u : 0u;
		}
	}

	uint64_t TextureStreamer::ReleaseMemory(AssetMemoryPool pool, uint64_t bytes, const AssetFinalizeContext& context)
	{
		// A manager that loads whole textures (partial loads off) keeps them whole: residency evicts them instead.
		if (pool != AssetMemoryPool::GpuTextures || !m_Manager.ArePartialLoadsActive() || !dynamic_cast<const RendererUploadContext*>(context.Upload))
			return 0;

		// The least needed first: textures the latest frame did not need (longest unneeded first), then the ones in view,
		// smallest on screen first, each down to its resident tail, never further; raises under way go first. A texture
		// dropped this way is raised again once the budget can hold the level it needs (OnUpdate).
		struct Candidate
		{
			AssetHandle Handle;
			bool Needed = false;
			float Coverage = 0.0f;
			uint64_t LastNeededFrame = 0;
		};
		std::vector<Candidate> candidates;
		for (const auto& [handle, target] : m_Policy.GetTargets())
		{
			// What a view without streaming needs keeps every level: it waits for them (residency evicts whole textures).
			if (target.Whole && target.LastNeededFrame == m_Frame)
				continue;
			candidates.push_back(Candidate { handle, target.LastNeededFrame == m_Frame, target.Coverage, target.LastNeededFrame });
		}
		std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b)
		{
			if (a.Needed != b.Needed)
				return !a.Needed;
			return a.Needed ? a.Coverage < b.Coverage : a.LastNeededFrame < b.LastNeededFrame;
		});

		uint64_t freed = 0;
		for (const Candidate& candidate : candidates)
		{
			if (freed >= bytes)
				break;
			const Ref<Texture> texture = PeekTexture(candidate.Handle);
			if (!texture || !m_Manager.HasStoredForm(candidate.Handle))
				continue;
			// A change under way goes first: its new texture comes on top of the resident one.
			auto streamed = m_Streamed.find(candidate.Handle);
			if (streamed != m_Streamed.end() && streamed->second.Changing)
			{
				const uint64_t before = texture->GetMemoryUsage().GpuTextures;
				texture->CancelLevelChange();
				streamed->second.Changing = false;
				m_Manager.UpdateMemoryUsage(candidate.Handle, *texture);
				const uint64_t after = texture->GetMemoryUsage().GpuTextures;
				freed += before > after ? before - after : 0;
			}
			if (freed >= bytes || texture->GetResidentLevel() >= texture->GetTailLevel() || !texture->CanChangeLevels())
				continue;
			freed += Drop(candidate.Handle, *texture, texture->GetTailLevel(), context);
		}
		return freed;
	}

	void TextureStreamer::AddStats(AssetManagerStats& stats) const
	{
		std::scoped_lock<std::mutex> lock(m_StatsMutex);
		stats.Detail.LevelsRaised += m_Stats.LevelsRaised;
		stats.Detail.LevelsDropped += m_Stats.LevelsDropped;
		stats.Detail.BytesStreamed += m_Stats.BytesStreamed;
		stats.Detail.BytesStreamedLastFrame += m_Stats.BytesStreamedLastFrame;
		stats.Detail.AssetsBelowWanted += m_Stats.AssetsBelowWanted;
		stats.Detail.Reads += m_Stats.Reads;
		stats.Detail.Changes += m_Stats.Changes;
	}

}
