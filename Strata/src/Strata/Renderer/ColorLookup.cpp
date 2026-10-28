#include "stpch.h"
#include "Strata/Renderer/ColorLookup.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/GPUUploadQueue.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/RendererUploadContext.h"
#include "Strata/Renderer/StagingPool.h"
#include "Strata/Renderer/TextureFormat.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace Strata
{

	namespace
	{

		constexpr char c_Magic[4] = { 'S', 'T', 'L', 'U' };
		// The largest finite half float: colors beyond it would be stored as infinity.
		constexpr float c_MaxHalf = 65504.0f;

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

		uint64_t GetPointCount(uint32_t size)
		{
			return static_cast<uint64_t>(size) * size * size;
		}

		// The size and colors Create accepts.
		bool ValidateColors(uint32_t size, std::span<const glm::vec3> colors, std::string* outError)
		{
			if (size < ColorLookup::c_MinSize || size > ColorLookup::c_MaxSize)
				return Fail(outError, fmt::format("a color lookup has {} to {} points each way, not {}", ColorLookup::c_MinSize, ColorLookup::c_MaxSize, size));
			if (colors.size() != GetPointCount(size))
				return Fail(outError, fmt::format("a color lookup of size {} has {} colors, not {}", size, GetPointCount(size), colors.size()));
			for (size_t index = 0; index < colors.size(); index++)
			{
				const glm::vec3& color = colors[index];
				for (int channel = 0; channel < 3; channel++)
				{
					if (!std::isfinite(color[channel]) || std::abs(color[channel]) > c_MaxHalf)
						return Fail(outError, fmt::format("color {} of the lookup is not a finite half float ({})", index, color[channel]));
				}
			}
			return true;
		}

		std::vector<uint16_t> ToTexels(std::span<const glm::vec3> colors)
		{
			std::vector<uint16_t> texels;
			texels.reserve(colors.size() * 4);
			const uint16_t one = glm::packHalf1x16(1.0f);
			for (const glm::vec3& color : colors)
			{
				texels.push_back(glm::packHalf1x16(color.r));
				texels.push_back(glm::packHalf1x16(color.g));
				texels.push_back(glm::packHalf1x16(color.b));
				texels.push_back(one);
			}
			return texels;
		}

	}

	Ref<ColorLookup> ColorLookup::Create(uint32_t size, std::span<const glm::vec3> colors, std::string* outError)
	{
		if (!ValidateColors(size, colors, outError))
			return nullptr;
		Ref<ColorLookup> lookup(new ColorLookup());
		lookup->m_Size = size;
		lookup->m_Texels = ToTexels(colors);
		return lookup;
	}

	std::vector<uint8_t> ColorLookup::Serialize(uint32_t size, std::span<const glm::vec3> colors, std::string* outError)
	{
		if (!ValidateColors(size, colors, outError))
			return {};
		const std::vector<uint16_t> texels = ToTexels(colors);
		std::vector<uint8_t> data(static_cast<size_t>(c_HeaderBytes) + texels.size() * sizeof(uint16_t));
		const uint32_t header[4] = { 0, c_FormatVersion, size, 0 };
		std::memcpy(data.data(), header, sizeof(header));
		std::memcpy(data.data(), c_Magic, sizeof(c_Magic));
		std::memcpy(data.data() + c_HeaderBytes, texels.data(), texels.size() * sizeof(uint16_t));
		return data;
	}

	Ref<ColorLookup> ColorLookup::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		auto fail = [outError](std::string message) -> Ref<ColorLookup>
		{
			Fail(outError, "Color lookup: " + std::move(message));
			return nullptr;
		};
		if (data.size() < c_HeaderBytes)
			return fail(fmt::format("{} bytes are too few for its header", data.size()));
		if (std::memcmp(data.data(), c_Magic, sizeof(c_Magic)) != 0)
			return fail("not a cooked color lookup (no STLU magic)");
		uint32_t header[4] = {};
		std::memcpy(header, data.data(), sizeof(header));
		if (header[1] != c_FormatVersion)
			return fail(fmt::format("format version {} is not {}", header[1], c_FormatVersion));
		const uint32_t size = header[2];
		if (size < c_MinSize || size > c_MaxSize)
			return fail(fmt::format("{} points each way, not {} to {}", size, c_MinSize, c_MaxSize));
		if (header[3] != 0)
			return fail("its header's reserved field is not zero");
		const uint64_t texelBytes = GetPointCount(size) * 4 * sizeof(uint16_t);
		if (data.size() != c_HeaderBytes + texelBytes)
			return fail(fmt::format("{} bytes, a table of size {} has {}", data.size(), size, c_HeaderBytes + texelBytes));

		Ref<ColorLookup> lookup(new ColorLookup());
		lookup->m_Size = size;
		lookup->m_Texels.resize(static_cast<size_t>(texelBytes / sizeof(uint16_t)));
		std::memcpy(lookup->m_Texels.data(), data.data() + c_HeaderBytes, static_cast<size_t>(texelBytes));
		// Half floats with every exponent bit set are infinities and NaNs: they would spread through the image.
		for (size_t index = 0; index < lookup->m_Texels.size(); index++)
		{
			if ((lookup->m_Texels[index] & 0x7C00u) == 0x7C00u)
				return fail(fmt::format("point {} is not finite", index / 4));
		}
		return lookup;
	}

	std::vector<glm::vec3> ColorLookup::MakeIdentity(uint32_t size)
	{
		std::vector<glm::vec3> colors;
		if (size < 2)
			return colors;
		colors.reserve(static_cast<size_t>(GetPointCount(size)));
		const float scale = 1.0f / static_cast<float>(size - 1);
		for (uint32_t blue = 0; blue < size; blue++)
		{
			for (uint32_t green = 0; green < size; green++)
			{
				for (uint32_t red = 0; red < size; red++)
					colors.emplace_back(static_cast<float>(red) * scale, static_cast<float>(green) * scale, static_cast<float>(blue) * scale);
			}
		}
		return colors;
	}

	glm::vec3 ColorLookup::Sample(uint32_t size, std::span<const glm::vec3> lattice, glm::vec3 coordinate)
	{
		ST_CORE_ASSERT(size >= 2 && lattice.size() == GetPointCount(size), "ColorLookup::Sample: {} colors are no lattice of size {}", lattice.size(), size);
		if (size < 2 || lattice.size() != GetPointCount(size))
			return coordinate;
		const float last = static_cast<float>(size - 1);
		glm::uvec3 low;
		glm::vec3 fraction;
		for (int axis = 0; axis < 3; axis++)
		{
			const float value = std::isfinite(coordinate[axis]) ? std::clamp(coordinate[axis], 0.0f, 1.0f) * last : 0.0f;
			low[axis] = std::min(static_cast<uint32_t>(value), size - 2);
			fraction[axis] = value - static_cast<float>(low[axis]);
		}
		auto at = [&](uint32_t red, uint32_t green, uint32_t blue) { return lattice[(static_cast<size_t>(blue) * size + green) * size + red]; };
		glm::vec3 result(0.0f);
		for (uint32_t corner = 0; corner < 8; corner++)
		{
			const glm::uvec3 offset((corner & 1u), (corner >> 1) & 1u, (corner >> 2) & 1u);
			const float weight = (offset.x ? fraction.x : 1.0f - fraction.x) * (offset.y ? fraction.y : 1.0f - fraction.y) * (offset.z ? fraction.z : 1.0f - fraction.z);
			if (weight > 0.0f)
				result += weight * at(low.x + offset.x, low.y + offset.y, low.z + offset.z);
		}
		return result;
	}

	std::vector<glm::vec3> ColorLookup::GetColors() const
	{
		std::vector<glm::vec3> colors;
		colors.reserve(m_Texels.size() / 4);
		for (size_t index = 0; index + 3 < m_Texels.size(); index += 4)
			colors.emplace_back(glm::unpackHalf1x16(m_Texels[index]), glm::unpackHalf1x16(m_Texels[index + 1]), glm::unpackHalf1x16(m_Texels[index + 2]));
		return colors;
	}

	AssetFinalizeResult ColorLookup::FinalizeOnMainThread(const AssetFinalizeContext& context)
	{
		RendererUploadContext* upload = RendererUploadContext::From(context.Upload);
		if (!upload || !upload->GetQueue().IsBatchOpen() || m_Texture)
			return AssetFinalizeResult::Done; // No renderer (tools, tests): CPU data only; or finalized before
		// Past the deadline only the frame's first finalization works (AssetFinalizeContext::FirstOfFrame), creating the texture too.
		if (!context.FirstOfFrame && std::chrono::steady_clock::now() >= context.Deadline)
			return AssetFinalizeResult::Pending;

		// The whole table goes in one step: the frame's first takes it whatever the budgets (its granule is all of it), a later
		// call only when the frame's bytes, time and staging leave room for it.
		GPUUploadQueue& queue = upload->GetQueue();
		const uint64_t bytes = GetTexelBytes();
		const bool framesFirst = GPUUploadQueue::IsFramesFirstStep(context, 0);
		if (queue.PlanStep(context, 0, 0.0f, bytes, bytes) < bytes || !queue.HasStagingRoom(context, StagingPool::GetBufferByteSize(bytes), framesFirst))
			return AssetFinalizeResult::Pending;

		nvrhi::TextureDesc desc;
		desc.dimension = nvrhi::TextureDimension::Texture3D;
		desc.width = m_Size;
		desc.height = m_Size;
		desc.depth = m_Size;
		desc.format = ToNvrhiFormat(TextureFormat::RGBA16F);
		desc.debugName = "ColorLookup";
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		nvrhi::TextureHandle texture = Renderer::GetDevice()->createTexture(desc);
		if (!texture)
		{
			ST_CORE_ERROR("Color lookup: its {}^3 texture could not be created", m_Size);
			return AssetFinalizeResult::Failed;
		}
		GPUTextureRegion region;
		region.Width = m_Size;
		region.Height = m_Size;
		region.Rows = m_Size;
		region.Depth = m_Size;
		region.Data = reinterpret_cast<const uint8_t*>(m_Texels.data());
		const GPUUploadStep step = queue.UploadTexture(context, texture, TextureFormat::RGBA16F, std::span<const GPUTextureRegion>(&region, 1), GPUUploadQueueKind::Graphics);
		if (!step.Recorded)
		{
			ST_CORE_ERROR("Color lookup: no staging memory for its upload");
			return AssetFinalizeResult::Failed;
		}
		// The GPU copy is authoritative from now on.
		m_Texture = std::move(texture);
		std::vector<uint16_t>().swap(m_Texels);
		return AssetFinalizeResult::Done;
	}

	AssetMemoryUsage ColorLookup::GetMemoryUsage() const
	{
		AssetMemoryUsage usage;
		usage.Cpu = m_Texels.size() * sizeof(uint16_t);
		if (m_Texture)
			usage.GpuTextures = GetTexelBytes();
		return usage;
	}

	AssetMemoryUsage ColorLookup::GetFinalizedMemoryUsage() const
	{
		if (!Renderer::IsInitialized() || m_Texture)
			return GetMemoryUsage();
		return AssetMemoryUsage { 0, GetTexelBytes(), 0 };
	}

}
