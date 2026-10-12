#include "stpch.h"
#include "Strata/Renderer/TextureStreamer.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Renderer/RendererUploadContext.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Renderer/Texture.h"

#include <algorithm>
#include <cmath>

namespace Strata
{

	namespace
	{

		// Products of two axis scales below this (squared world units per unit of the mesh) count as this: a surface scaled
		// to nothing shows nothing, and its density must stay finite.
		constexpr float c_MinScaleProduct = 1e-12f;

	}

	////////////////////////////////////////////////////////////////////////////////
	// TextureStreamingPolicy
	////////////////////////////////////////////////////////////////////////////////

	uint32_t TextureStreamingPolicy::ComputeWantedLevel(uint32_t size, uint32_t mipCount, float uvScale, float pixels)
	{
		if (mipCount == 0)
			return 0;
		const uint32_t coarsest = mipCount - 1;
		const float texels = static_cast<float>(size) * std::max(std::isfinite(uvScale) ? uvScale : 1.0f, 0.0f);
		// Drawn no larger than a pixel (or not at all): the coarsest level is all it shows.
		if (!(pixels > 1.0f) || !std::isfinite(pixels))
			return coarsest;
		if (!(texels > pixels))
			return 0;
		const float level = std::floor(std::log2(texels / pixels));
		return static_cast<uint32_t>(std::min(level, static_cast<float>(coarsest)));
	}

	TextureStreamingPolicy::ViewScale TextureStreamingPolicy::GetViewScale(const SceneCamera& camera, float viewportHeight)
	{
		ViewScale view;
		view.Position = camera.Position;
		view.Orthographic = camera.Orthographic;
		view.Near = std::max(camera.Near, 1e-4f);
		// Like LODSelection::GetScreenFraction: an orthographic camera's VerticalFOV is the height it shows.
		if (camera.Orthographic)
			view.UnitPixels = viewportHeight / std::max(camera.VerticalFOV, 1e-4f);
		else
			view.UnitPixels = viewportHeight / (2.0f * std::tan(std::max(camera.VerticalFOV, 1e-3f) * 0.5f));
		return view;
	}

	float TextureStreamingPolicy::GetWorldPixels(const ViewScale& view, const AABB& bounds)
	{
		if (view.Orthographic)
			return view.UnitPixels;
		const glm::vec3 outside = glm::max(glm::abs(view.Position - bounds.GetCenter()) - bounds.GetExtents(), glm::vec3(0.0f));
		return view.UnitPixels / std::max(glm::length(outside), view.Near);
	}

	float TextureStreamingPolicy::GetUVPixels(float worldPixels, const glm::vec3& uvDensity, const glm::vec3& axisScale)
	{
		// Texture coordinate units per world unit of the surfaces facing each axis: theirs per unit of the mesh, over the
		// geometric mean of the scales along the two axes they span. The sparsest surfaces decide: they spread each texel over
		// the most pixels, so they need the finest level.
		const glm::vec3 spanned(axisScale.y * axisScale.z, axisScale.x * axisScale.z, axisScale.x * axisScale.y);
		float density = std::numeric_limits<float>::infinity();
		for (glm::length_t axis = 0; axis < 3; axis++)
		{
			if (uvDensity[axis] > 0.0f)
				density = std::min(density, uvDensity[axis] / std::sqrt(std::max(std::abs(spanned[axis]), c_MinScaleProduct)));
		}
		if (!(density > 0.0f) || !std::isfinite(density))
			return std::numeric_limits<float>::max();
		return worldPixels / density;
	}

	TextureStreamingPolicy::Rank TextureStreamingPolicy::RankRaise(uint32_t missingLevels, float coverage, bool cameraCut)
	{
		Rank rank;
		rank.Priority = cameraCut ? AssetPriority::Normal : AssetPriority::Low;
		rank.Score = static_cast<float>(missingLevels) * std::clamp(std::isfinite(coverage) ? coverage : 0.0f, 0.0f, 1.0f);
		return rank;
	}

	bool TextureStreamingPolicy::IsCameraCut(const glm::vec3& previousPosition, const glm::vec3& previousForward, const glm::vec3& position, const glm::vec3& forward)
	{
		return glm::distance(position, previousPosition) > c_CameraCutDistance || glm::dot(forward, previousForward) < c_CameraCutCosine;
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

	void TextureStreamer::ReportView(const TextureStreamingView& view, std::span<const VisibleTextureUse> textures)
	{
		// The renderer resolved the textures for the frame: nothing is looked up in the manager (a frame draws up to tens of
		// thousands of materials, most of them often without textures, which the renderer leaves out).
		const float viewportHeight = std::max(view.ViewportHeight, 1.0f);
		for (const VisibleTextureUse& use : textures)
		{
			const uint32_t level = view.Streaming ? TextureStreamingPolicy::ComputeWantedLevel(use.Size, use.MipCount, use.UVScale, use.UVPixels) : 0;
			m_Policy.AddNeed(use.Texture, level, use.PixelSize / viewportHeight, view.CameraCut, !view.Streaming);
		}
	}

	void TextureStreamer::RequestLevel(AssetHandle texture, uint32_t level, float coverage)
	{
		if (PeekTexture(texture))
			m_Policy.AddNeed(texture, level, coverage, false);
	}

	void TextureStreamer::Hold(const Ref<TextureHold>& hold)
	{
		if (!hold)
			return;
		for (const std::weak_ptr<TextureHold>& held : m_Holds)
		{
			if (held.lock() == hold)
				return;
		}
		m_Holds.push_back(hold);
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
			{
				m_Stats.LevelsRaised += streamed.ChangeFrom - target;
			}
			else
			{
				m_Stats.LevelsDropped += target - streamed.ChangeFrom;
				if (streamed.ChangeForBudget)
					m_Stats.LevelsDroppedForBudget += target - streamed.ChangeFrom;
			}
		}
		streamed.ChangeForBudget = false;
		return true;
	}

	uint64_t TextureStreamer::Drop(AssetHandle handle, Texture& texture, uint32_t level, bool forBudget, const AssetFinalizeContext& context)
	{
		const uint64_t before = texture.GetMemoryUsage().GpuTextures;
		const uint32_t resident = texture.GetResidentLevel();
		if (!texture.DropLevels(level))
			return 0;
		Streamed& streamed = m_Streamed[handle];
		streamed.Changing = true;
		streamed.ChangeForBudget = forBudget;
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

	std::vector<TextureStreamer::DropCandidate> TextureStreamer::GetDropCandidates() const
	{
		std::vector<DropCandidate> candidates;
		for (const auto& [handle, target] : m_Policy.GetTargets())
		{
			// What a view without streaming needs keeps every level: it waits for them (residency evicts whole textures).
			if (target.Whole && target.LastNeededFrame == m_Frame)
				continue;
			candidates.push_back(DropCandidate { handle, target.LastNeededFrame == m_Frame, target.Coverage, target.LastNeededFrame });
		}
		std::sort(candidates.begin(), candidates.end(), [](const DropCandidate& a, const DropCandidate& b)
		{
			if (a.Needed != b.Needed)
				return !a.Needed;
			if (a.Needed ? a.Coverage != b.Coverage : a.LastNeededFrame != b.LastNeededFrame)
				return a.Needed ? a.Coverage < b.Coverage : a.LastNeededFrame < b.LastNeededFrame;
			return a.Handle < b.Handle; // The same order in every run
		});
		return candidates;
	}

	uint64_t TextureStreamer::ReleaseLevels(AssetHandle handle, uint64_t bytes, const AssetFinalizeContext& context)
	{
		const Ref<Texture> texture = PeekTexture(handle);
		if (!texture || !m_Manager.HasStoredForm(handle))
			return 0;
		uint64_t freed = 0;
		auto streamed = m_Streamed.find(handle);
		if (streamed != m_Streamed.end())
		{
			// A read under way would raise what goes now: it is of no use any more.
			if (streamed->second.Read != 0)
			{
				m_Manager.CancelRange(streamed->second.Read);
				streamed->second.Read = 0;
			}
			if (streamed->second.Changing)
			{
				const uint64_t before = texture->GetMemoryUsage().GpuTextures;
				texture->CancelLevelChange();
				streamed->second.Changing = false;
				streamed->second.ChangeForBudget = false;
				m_Manager.UpdateMemoryUsage(handle, *texture);
				const uint64_t after = texture->GetMemoryUsage().GpuTextures;
				freed += before > after ? before - after : 0;
			}
		}
		const uint32_t resident = texture->GetResidentLevel();
		const uint32_t tail = texture->GetTailLevel();
		if (freed >= bytes || resident >= tail || !texture->CanChangeLevels())
			return freed;
		// The finest levels, as many as free what is still to be freed (at most down to the tail).
		uint32_t level = resident + 1;
		while (level < tail && texture->GetLevelBytes(resident, level) < bytes - freed)
			level++;
		return freed + Drop(handle, *texture, level, true, context);
	}

	uint64_t TextureStreamer::GetReleasableBytes(AssetHandle handle) const
	{
		const Ref<Texture> texture = PeekTexture(handle);
		if (!texture || !m_Manager.HasStoredForm(handle))
			return 0;
		const uint32_t mipCount = texture->GetMipCount();
		uint64_t bytes = 0;
		bool changing = false;
		auto streamed = m_Streamed.find(handle);
		if (streamed != m_Streamed.end())
		{
			if (streamed->second.Read != 0)
				bytes += texture->GetLevelBytes(streamed->second.ReadLevel, mipCount);
			if (streamed->second.Changing)
			{
				changing = true;
				bytes += texture->GetLevelBytes(texture->GetChangeLevel(), mipCount);
			}
		}
		// Its levels above the tail, once a change under way is cancelled.
		const uint32_t resident = texture->GetResidentLevel();
		if ((changing || texture->CanChangeLevels()) && resident < texture->GetTailLevel())
			bytes += texture->GetLevelBytes(resident, texture->GetTailLevel());
		return bytes;
	}

	uint64_t TextureStreamer::GetRoomFromLessNeeded(AssetHandle raising, float need, const std::vector<DropCandidate>& candidates, uint32_t drops) const
	{
		uint64_t bytes = 0;
		for (const DropCandidate& candidate : candidates)
		{
			if (drops >= c_MaxDropsPerFrame)
				break;
			if (candidate.Needed && !(candidate.Coverage < need))
				break; // Least needed first: the rest are needed as much or more
			if (candidate.Handle == raising)
				continue;
			const uint64_t releasable = GetReleasableBytes(candidate.Handle);
			if (releasable == 0)
				continue;
			bytes += releasable;
			drops++;
		}
		return bytes;
	}

	uint64_t TextureStreamer::MakeRoom(AssetHandle raising, uint64_t bytes, float need, std::vector<DropCandidate>& candidates, uint32_t& drops,
		const AssetFinalizeContext& context)
	{
		uint64_t gained = 0;
		for (const DropCandidate& candidate : candidates)
		{
			if (gained >= bytes || drops >= c_MaxDropsPerFrame)
				break;
			if (candidate.Needed && !(candidate.Coverage < need))
				break; // Least needed first: the rest are needed as much or more
			if (candidate.Handle == raising)
				continue;
			const Ref<Texture> texture = PeekTexture(candidate.Handle);
			if (!texture)
				continue;
			// The room its read holds goes first (the read is abandoned), then its finest levels.
			auto streamed = m_Streamed.find(candidate.Handle);
			if (streamed != m_Streamed.end() && streamed->second.Read != 0)
			{
				gained += texture->GetLevelBytes(streamed->second.ReadLevel, texture->GetMipCount());
				m_Manager.CancelRange(streamed->second.Read);
				streamed->second.Read = 0;
			}
			if (gained >= bytes)
				break;
			const uint64_t freed = ReleaseLevels(candidate.Handle, bytes - gained, context);
			if (freed > 0)
				drops++;
			gained += freed;
		}
		return gained;
	}

	void TextureStreamer::OnUpdate(const AssetFinalizeContext& context)
	{
		ST_PROFILE_FUNCTION();
		// Held textures are needed whole in every frame their holds live, as if a view without streaming drew them.
		std::erase_if(m_Holds, [](const std::weak_ptr<TextureHold>& held) { return held.expired(); });
		for (const std::weak_ptr<TextureHold>& held : m_Holds)
		{
			const Ref<TextureHold> hold = held.lock();
			if (hold && PeekTexture(hold->Texture))
				m_Policy.AddNeed(hold->Texture, 0, 1.0f, false, true);
		}
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
								streamed.ChangeForBudget = false;
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

		// Each texture's wanted level; drops of levels no longer needed; queued reads move up as their textures are needed
		// more (a camera cut's view first); and the textures below their target, to be ranked for the room in the budget.
		uint32_t drops = 0;
		uint32_t belowWanted = 0;
		std::vector<Raise> raises;
		for (const auto& [handle, target] : m_Policy.GetTargets())
		{
			const Ref<Texture> texture = PeekTexture(handle);
			if (!texture)
				continue;
			const uint32_t tail = texture->GetTailLevel();
			const uint32_t level = target.Level == TextureStreamingPolicy::c_NoLevel ? tail : std::min(target.Level, texture->GetMipCount() - 1);
			const uint32_t resident = texture->GetResidentLevel();
			// What the latest frame did not draw is not raised: its held level (the hysteresis) only keeps what it has from
			// going, and raising it would take room from what is drawn (and be taken back the next frame).
			const bool needed = target.LastNeededFrame == m_Frame;
			const uint32_t wanted = needed ? level : std::max(level, resident);
			texture->SetWantedLevel(wanted);
			if (wanted < resident)
				belowWanted++;
			auto streamed = m_Streamed.find(handle);
			if (streamed != m_Streamed.end() && streamed->second.Read != 0 && !streamed->second.Changing && wanted < resident)
			{
				const TextureStreamingPolicy::Rank rank = TextureStreamingPolicy::RankRaise(resident - level, target.Coverage, target.CameraCut);
				if (rank.Outranks(streamed->second.ReadRank))
				{
					m_Manager.RaiseRange(streamed->second.Read, rank.Priority, rank.Score);
					streamed->second.ReadRank = rank;
				}
			}
			const bool busy = streamed != m_Streamed.end() && (streamed->second.Read != 0 || streamed->second.Changing || streamed->second.RetryFrame > m_Frame);
			if (busy || !texture->CanChangeLevels() || !upload || !m_Manager.HasStoredForm(handle))
				continue;

			if (wanted < resident)
			{
				const float need = target.Whole ? std::numeric_limits<float>::infinity() : target.Coverage;
				raises.push_back(Raise { handle, texture, level, resident, need, TextureStreamingPolicy::RankRaise(resident - level, target.Coverage, target.CameraCut) });
			}
			else if (level > resident && drops < c_MaxDropsPerFrame && m_Manager.ArePartialLoadsActive())
			{
				if (Drop(handle, *texture, level, false, context) > 0)
					drops++;
			}
		}

		if (!raises.empty())
		{
			// The room in the budget goes in rank order. A raise builds its new texture beside the resident one, which goes
			// only once the new one replaced it: it needs room for the new texture whole until then, as do the reads under way
			// for theirs.
			std::sort(raises.begin(), raises.end(), [](const Raise& a, const Raise& b)
			{
				if (a.Rank.Outranks(b.Rank) || b.Rank.Outranks(a.Rank))
					return a.Rank.Outranks(b.Rank);
				return a.Handle < b.Handle; // The same order in every run
			});
			uint64_t room = GetTextureRoom();
			const bool limited = room != AssetResidencyBudgets::c_Unlimited;
			if (limited)
			{
				for (const auto& [handle, streamed] : m_Streamed)
				{
					const Ref<Texture> texture = streamed.Read != 0 ? PeekTexture(handle) : nullptr;
					if (texture)
						room -= std::min(room, texture->GetLevelBytes(streamed.ReadLevel, texture->GetMipCount()));
				}
			}
			std::vector<DropCandidate> candidates;
			bool candidatesListed = false;
			for (const Raise& raise : raises)
			{
				// The finest level the room, with what textures needed less can give up, holds; levels are taken from those
				// only once the raise is decided, and only as many as it misses.
				uint32_t level = raise.Level;
				const uint32_t mipCount = raise.Target->GetMipCount();
				if (limited)
				{
					uint64_t available = room;
					if (raise.Target->GetLevelBytes(raise.Level, mipCount) > room)
					{
						if (!candidatesListed)
						{
							candidates = GetDropCandidates();
							candidatesListed = true;
						}
						available += GetRoomFromLessNeeded(raise.Handle, raise.Need, candidates, drops);
					}
					level = raise.Resident;
					while (level > raise.Level && raise.Target->GetLevelBytes(level - 1, mipCount) <= available)
						level--;
					if (level == raise.Resident)
						continue;
					const uint64_t required = raise.Target->GetLevelBytes(level, mipCount);
					if (required > room)
						room += MakeRoom(raise.Handle, required - room, raise.Need, candidates, drops, context);
					if (required > room)
						continue; // A drop could not finish in this call: what it frees counts once it has
				}
				// A texture MakeRoom took levels from no longer reads what it was ranked for.
				if (raise.Target->GetResidentLevel() != raise.Resident)
					continue;
				const CookedTextureByteRange range = raise.Target->GetCookedLayout().GetLevelsByteRange(level, raise.Resident);
				const uint64_t read = m_Manager.RequestRange(raise.Handle, range.Offset, range.Size, raise.Rank.Priority, raise.Rank.Score);
				if (read == 0)
					continue;
				Streamed& entry = m_Streamed[raise.Handle];
				entry.Read = read;
				entry.ReadLevel = level;
				entry.ReadResident = raise.Resident;
				entry.ReadRank = raise.Rank;
				if (limited)
					room -= std::min(room, raise.Target->GetLevelBytes(level, mipCount));
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

		// The least needed first, each giving up its finest levels, only as many as the budget takes (a texture dropped this
		// way is raised again once the budget can hold the level it needs, OnUpdate).
		uint64_t freed = 0;
		for (const DropCandidate& candidate : GetDropCandidates())
		{
			if (freed >= bytes)
				break;
			freed += ReleaseLevels(candidate.Handle, bytes - freed, context);
		}
		return freed;
	}

	void TextureStreamer::AddStats(AssetManagerStats& stats) const
	{
		std::scoped_lock<std::mutex> lock(m_StatsMutex);
		stats.Detail.LevelsRaised += m_Stats.LevelsRaised;
		stats.Detail.LevelsDropped += m_Stats.LevelsDropped;
		stats.Detail.LevelsDroppedForBudget += m_Stats.LevelsDroppedForBudget;
		stats.Detail.BytesStreamed += m_Stats.BytesStreamed;
		stats.Detail.BytesStreamedLastFrame += m_Stats.BytesStreamedLastFrame;
		stats.Detail.AssetsBelowWanted += m_Stats.AssetsBelowWanted;
		stats.Detail.Reads += m_Stats.Reads;
		stats.Detail.Changes += m_Stats.Changes;
	}

}
