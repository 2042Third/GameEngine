#pragma once

#include "Strata/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace Strata
{

	// Stable indices of the textures materials sample (Renderer::GetTextureTable): a texture keeps its index for its life, and
	// the index's entry follows the texture's bindless slot (packed with its sampler and grayscale bit: PackEntry). Material
	// data holds indices, so it stays the same while a texture streams levels and moves from bindless slot to bindless slot;
	// shaders read the entry first (SampleMaterialTexture, Include/Scene.glsl). Indices 0-2 stand for white, black and flat
	// normal (BindlessTextureTable's reserved slots).
	//
	// An entry also holds a minimum level of detail: a texture that gained levels fades them in (SetFadeIn), from the level
	// it showed before to its finest, over c_FadeInSeconds, so that new detail does not pop in. SampleMaterialTexture
	// (Include/MaterialTextures.glsl) scales the sampling gradients up to it, which keeps anisotropic filtering.
	//
	// Entries change on any thread (textures finalize and stream on the main thread, and are destroyed on any); renderers
	// upload the changed ones before they draw (RecordUploads, once per frame for all of them). A freed index draws white
	// and is reused only once every frame that may still read it has completed (BeginFrame).
	class TextureTable
	{
	public:
		// What the GPU reads per index (RenderData's std430 layout in Include/Scene.glsl: uvec2).
		struct GPUEntry
		{
			uint32_t Entry = 0;  // PackEntry
			float MinLod = 0.0f; // The finest level of detail sampled, in the texture's own levels (0: no limit)
		};

		// How long raised levels take to fade in (SetFadeInSeconds).
		static constexpr float c_FadeInSeconds = 0.25f;
		static constexpr uint32_t c_White = 0;
		static constexpr uint32_t c_Black = 1;
		static constexpr uint32_t c_FlatNormal = 2;
		static constexpr uint32_t c_ReservedIndices = 3;
		static constexpr uint32_t c_InvalidIndex = ~0u;

		TextureTable(nvrhi::IDevice* device, uint32_t framesInFlight);

		TextureTable(const TextureTable&) = delete;
		TextureTable& operator=(const TextureTable&) = delete;

		// The entry of a bindless slot: the slot in bits 0-23, the sampler (TextureFilter * 3 + TextureWrap) in bits 24-30 and
		// RenderData::c_TextureSlotGrayscale for one-channel textures sampled as gray.
		static uint32_t PackEntry(uint32_t bindlessSlot, uint32_t samplerIndex, bool grayscale);

		// A new index holding `entry` (any thread).
		uint32_t Allocate(uint32_t entry);
		// Points the index at another entry (any thread). Reserved and invalid indices are ignored. A fade under way ends: the
		// new entry is sampled at its finest.
		void Set(uint32_t index, uint32_t entry);
		// Points the index at a texture that gained `gainedLevels` levels (any thread): it is sampled no finer than the level
		// that matches what the previous entry showed at first, and fades to its finest over the fade-in time (at once when
		// that is 0 or nothing was gained).
		void SetFadeIn(uint32_t index, uint32_t entry, uint32_t gainedLevels);
		// How long fades take (c_FadeInSeconds by default; 0 turns them off). Main thread.
		void SetFadeInSeconds(float seconds);
		float GetFadeInSeconds() const;
		// The minimum level of detail an index is sampled with now (as of the latest UpdateFades), and how many fades are under
		// way.
		float GetMinLod(uint32_t index) const;
		uint32_t GetFadeCount() const;
		// Moves the fades on to `seconds` (GetClockSeconds), ending those that are through; RecordUploads does so at the
		// clock's time. Main thread.
		void UpdateFades(double seconds);
		// The steady clock fades are timed by, in seconds.
		static double GetClockSeconds();
		// The index draws white from now on and is reused once the frames that may read it completed (any thread).
		void Free(uint32_t index);
		uint32_t GetEntry(uint32_t index) const;
		uint32_t GetAllocatedCount() const;

		// Once per device frame (Renderer::BeginFrame): indices freed long enough ago become reusable.
		void BeginFrame(uint64_t frameIndex);
		// Writes the entries that changed since the last call (the whole table into a new buffer when it grew) into the command
		// list, ahead of what samples them; returns false when the buffer could not be created (logged). Main thread.
		bool RecordUploads(nvrhi::ICommandList* commandList, uint64_t* outBytes = nullptr);
		// Valid once RecordUploads ran; replaced when the table grows (GetBufferVersion changes).
		nvrhi::IBuffer* GetBuffer() const { return m_Buffer; }
		uint64_t GetBufferVersion() const { return m_BufferVersion; }
	private:
		void MarkDirtyLocked(uint32_t index);
		void EndFadeLocked(uint32_t index);
	private:
		nvrhi::IDevice* m_Device;
		uint32_t m_FramesInFlight;

		mutable std::mutex m_Mutex;
		std::vector<uint32_t> m_Entries;
		std::vector<float> m_MinLods;
		std::vector<uint8_t> m_Allocated;
		std::vector<uint8_t> m_Dirty;
		std::vector<uint32_t> m_DirtyList;
		std::vector<uint32_t> m_FreeIndices;
		struct PendingFree
		{
			uint32_t Index = 0;
			uint64_t Frame = 0;
		};
		std::deque<PendingFree> m_PendingFrees;
		uint64_t m_CurrentFrame = 0;
		uint32_t m_AllocatedCount = 0;
		// Fades under way (a few textures at a time: those that just gained levels).
		struct Fade
		{
			uint32_t Index = 0;
			float FromLod = 0.0f;
			double StartSeconds = 0.0;
		};
		std::vector<Fade> m_Fades;
		float m_FadeInSeconds = c_FadeInSeconds;

		// Main thread: the GPU's copy.
		nvrhi::BufferHandle m_Buffer;
		uint64_t m_BufferCapacity = 0; // Entries
		uint64_t m_BufferVersion = 0;
		std::vector<GPUEntry> m_UploadScratch;
		std::vector<uint32_t> m_DirtyScratch;
	};

}
