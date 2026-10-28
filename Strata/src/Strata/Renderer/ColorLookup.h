#pragma once

#include "Strata/Asset/Asset.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	// A 3D color lookup table: a grade made in another tool (imported from a .cube file by ColorLookupImporter) that maps a
	// display-encoded color to another, applied after the tone curve (PostProcessComponent::ColorLookup). Its lattice spans
	// [0, 1] on every axis with GetSize() points each way, red changing fastest, then green, then blue; between the points it
	// is interpolated trilinearly (Sample). The GPU copy is an RGBA16F 3D texture, uploaded in one step when the asset is
	// finalized (65^3 points are 2.2 MB, under c_AssetUploadStepBytes); the CPU copy goes once it is up.
	//
	// Cooked form (version c_FormatVersion): the magic "STLU", the version, the size and a zero (four little-endian uint32),
	// then size^3 points of four half floats (red, green, blue, 1). Deserialize validates all of it.
	class ColorLookup : public Asset
	{
	public:
		static constexpr uint32_t c_MinSize = 2;
		static constexpr uint32_t c_MaxSize = 65;
		static constexpr uint32_t c_FormatVersion = 1;
		static constexpr uint64_t c_HeaderBytes = 16;

		static AssetType GetStaticType() { return AssetType::ColorLookup; }
		AssetType GetType() const override { return GetStaticType(); }

		// A table of `size`^3 colors (red fastest), rounded to half floats; null, with the reason, for a size outside
		// [c_MinSize, c_MaxSize], another number of colors, or colors that are not finite or beyond a half float's range.
		static Ref<ColorLookup> Create(uint32_t size, std::span<const glm::vec3> colors, std::string* outError = nullptr);
		// The cooked form of the table Create would make; empty, with the reason, for what Create refuses.
		static std::vector<uint8_t> Serialize(uint32_t size, std::span<const glm::vec3> colors, std::string* outError = nullptr);
		// Rejects truncated, padded and garbage bytes, and points that are not finite.
		static Ref<ColorLookup> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);

		// The lattice that maps every color to itself.
		static std::vector<glm::vec3> MakeIdentity(uint32_t size);
		// The color the lattice gives `coordinate` (clamped to [0, 1]): the trilinear interpolation of the eight points around it,
		// as the GPU samples the texture (and the importer resamples a table). `lattice` holds `size`^3 colors, red fastest.
		static glm::vec3 Sample(uint32_t size, std::span<const glm::vec3> lattice, glm::vec3 coordinate);

		uint32_t GetSize() const { return m_Size; }
		// The colors as stored (rounded to half floats), red fastest; empty once uploaded to the GPU.
		std::vector<glm::vec3> GetColors() const;
		// The GPU texture (RGBA16F, 3D); null until the asset was finalized with a renderer.
		nvrhi::ITexture* GetTexture() const { return m_Texture.Get(); }

		AssetFinalizeResult FinalizeOnMainThread(const AssetFinalizeContext& context) override;
		AssetMemoryUsage GetMemoryUsage() const override;
		AssetMemoryUsage GetFinalizedMemoryUsage() const override;
	private:
		ColorLookup() = default;
		uint64_t GetTexelBytes() const { return static_cast<uint64_t>(m_Size) * m_Size * m_Size * 4 * sizeof(uint16_t); }
	private:
		uint32_t m_Size = 0;
		std::vector<uint16_t> m_Texels; // Half floats, four per point (the cooked texels); released once uploaded
		nvrhi::TextureHandle m_Texture;
	};

}
