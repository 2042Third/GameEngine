#include "Renderer/FlickerMetrics.h"

#include "Strata/Renderer/Renderer.h"

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace Strata::Tests
{

	namespace
	{

		// The scene shaders' octahedral decoding (Include/Scene.slang, OctahedralDecode), before its normalization.
		void DecodeOctahedral(float x, float y, float n[3])
		{
			n[0] = x;
			n[1] = y;
			n[2] = 1.0f - std::abs(x) - std::abs(y);
			const float t = std::max(-n[2], 0.0f);
			n[0] += n[0] >= 0.0f ? -t : t;
			n[1] += n[1] >= 0.0f ? -t : t;
		}

		// Every half-precision value as a float, made once.
		const std::vector<float>& GetHalfTable()
		{
			static const std::vector<float> s_Table = []()
			{
				std::vector<float> table(65536);
				for (uint32_t bits = 0; bits < 65536; bits++)
					table[bits] = glm::unpackHalf1x16(static_cast<uint16_t>(bits));
				return table;
			}();
			return s_Table;
		}

		float DecodeSRGB(uint8_t code)
		{
			const float encoded = static_cast<float>(code) / 255.0f;
			return encoded <= 0.04045f ? encoded / 12.92f : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
		}

		float Luminance(float red, float green, float blue)
		{
			return 0.2126f * red + 0.7152f * green + 0.0722f * blue;
		}

		// Everything of a frame but its values: the matrix it rendered with (the camera's rotation-only view and its projection
		// translated by the TAA's jitter, as SceneRenderer::RenderFrame builds them), the prepass's depth as view depth and its
		// normals in world space.
		FlickerFrame ReadFrameGeometry(SceneRenderer& renderer, const SceneCamera& camera)
		{
			const glm::uvec2 size = renderer.GetViewportSize();
			FlickerFrame frame;
			frame.Width = size.x;
			frame.Height = size.y;
			const glm::mat4 relativeView = glm::mat4(glm::mat3(camera.View));
			const glm::vec2 jitter = renderer.GetStats().Jitter;
			const glm::vec2 jitterClip(2.0f * jitter.x / static_cast<float>(size.x), -2.0f * jitter.y / static_cast<float>(size.y));
			const glm::mat4 projection = glm::translate(glm::mat4(1.0f), glm::vec3(jitterClip, 0.0f)) * camera.Projection;
			frame.ViewProjection = projection * relativeView;
			frame.CameraPosition = glm::dvec3(camera.Position);
			frame.CameraForward = glm::normalize(glm::transpose(glm::mat3(camera.View)) * glm::vec3(0.0f, 0.0f, -1.0f));

			const size_t pixels = static_cast<size_t>(size.x) * size.y;
			ReadbackImage depth;
			REQUIRE(Renderer::ReadTexture(renderer.GetDepthTexture(), depth));
			REQUIRE(depth.Width == size.x);
			REQUIRE(depth.Height == size.y);
			REQUIRE(depth.Pixels.size() == pixels * sizeof(float));
			ReadbackImage normals;
			REQUIRE(Renderer::ReadTexture(renderer.GetNormalTexture(), normals));
			REQUIRE(normals.Format == nvrhi::Format::RG16_FLOAT);
			REQUIRE(normals.Pixels.size() == pixels * 2 * sizeof(uint16_t));

			// The view depth of a reversed-Z depth: the inverse projection's z and w rows do not depend on x and y (nor on the
			// jitter, which only offsets them). The loops over pixels use plain arithmetic on raw arrays: Debug builds run the
			// strafe tests' millions of pixels a frame.
			const glm::mat4 inverseProjection = glm::inverse(projection);
			const float depthZ[2] = { inverseProjection[2][2], inverseProjection[3][2] };
			const float depthW[2] = { inverseProjection[2][3], inverseProjection[3][3] };
			const glm::mat3 inverseView = glm::transpose(glm::mat3(camera.View));
			float rotation[3][3];
			for (int column = 0; column < 3; column++)
			{
				for (int row = 0; row < 3; row++)
					rotation[column][row] = inverseView[column][row];
			}
			const std::vector<float>& halves = GetHalfTable();
			frame.ViewDepth.resize(pixels);
			frame.Normal.resize(pixels);
			float* viewDepth = frame.ViewDepth.data();
			glm::vec3* normal = frame.Normal.data();
			const uint8_t* depthBytes = depth.Pixels.data();
			const uint8_t* normalBytes = normals.Pixels.data();
			for (size_t pixel = 0; pixel < pixels; pixel++)
			{
				float value = 0.0f;
				std::memcpy(&value, depthBytes + pixel * sizeof(float), sizeof(float));
				viewDepth[pixel] = value > 0.0f ? -(depthZ[0] * value + depthZ[1]) / (depthW[0] * value + depthW[1]) : 0.0f;
				uint16_t encoded[2];
				std::memcpy(encoded, normalBytes + pixel * sizeof(encoded), sizeof(encoded));
				float n[3];
				DecodeOctahedral(halves[encoded[0]], halves[encoded[1]], n);
				float world[3];
				for (int row = 0; row < 3; row++)
					world[row] = rotation[0][row] * n[0] + rotation[1][row] * n[1] + rotation[2][row] * n[2];
				const float length = std::sqrt(world[0] * world[0] + world[1] * world[1] + world[2] * world[2]);
				const float scale = length > 0.0f ? 1.0f / length : 0.0f;
				normal[pixel].x = world[0] * scale;
				normal[pixel].y = world[1] * scale;
				normal[pixel].z = world[2] * scale;
			}
			return frame;
		}

		// The value at the 99th percentile: index floor(0.99 (n - 1)) of the sorted values.
		double Percentile99(std::vector<double>& values)
		{
			if (values.empty())
				return 0.0;
			const size_t index = static_cast<size_t>(std::floor(0.99 * static_cast<double>(values.size() - 1)));
			std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
			return values[index];
		}

		double Mean(const std::vector<double>& values)
		{
			if (values.empty())
				return 0.0;
			double sum = 0.0;
			for (const double value : values)
				sum += value;
			return sum / static_cast<double>(values.size());
		}

		// A frame as the measure reads it: raw arrays and its matrix as plain numbers, because Debug builds measure millions of
		// pixels a strafe.
		struct FrameView
		{
			const float* Value = nullptr;
			const float* ViewDepth = nullptr;
			const glm::vec3* Normal = nullptr;
			uint32_t Width = 0;
			uint32_t Height = 0;
			float Matrix[4][4] = {}; // [column][row], as glm stores it
			double Camera[3] = {};
			float Forward[3] = {};

			explicit FrameView(const FlickerFrame& frame, const glm::mat4& matrix)
				: Value(frame.Value.data()), ViewDepth(frame.ViewDepth.data()), Normal(frame.Normal.data()), Width(frame.Width), Height(frame.Height)
			{
				for (int column = 0; column < 4; column++)
				{
					for (int row = 0; row < 4; row++)
						Matrix[column][row] = matrix[column][row];
				}
				for (int axis = 0; axis < 3; axis++)
				{
					Camera[axis] = frame.CameraPosition[axis];
					Forward[axis] = frame.CameraForward[axis];
				}
			}
		};

		// Where a frame saw a world point: bilinear weights over the four texels around it, and whether all four show the same
		// surface (the point's depth in that frame and its normal).
		bool SampleEarlierFrame(const FrameView& frame, const double point[3], const glm::vec3& normal, float& value)
		{
			const float relative[3] = { static_cast<float>(point[0] - frame.Camera[0]), static_cast<float>(point[1] - frame.Camera[1]),
				static_cast<float>(point[2] - frame.Camera[2]) };
			float clip[4];
			for (int row = 0; row < 4; row++)
				clip[row] = frame.Matrix[0][row] * relative[0] + frame.Matrix[1][row] * relative[1] + frame.Matrix[2][row] * relative[2] + frame.Matrix[3][row];
			if (!(clip[3] > 0.0f))
				return false;
			const float x = (clip[0] / clip[3] * 0.5f + 0.5f) * static_cast<float>(frame.Width) - 0.5f;
			const float y = (0.5f - clip[1] / clip[3] * 0.5f) * static_cast<float>(frame.Height) - 0.5f;
			if (!std::isfinite(x) || !std::isfinite(y))
				return false;
			const float qx = std::floor(x);
			const float qy = std::floor(y);
			if (qx < 1.0f || qx >= static_cast<float>(frame.Width) - 2.0f || qy < 1.0f || qy >= static_cast<float>(frame.Height) - 2.0f)
				return false;
			const uint32_t ix = static_cast<uint32_t>(qx);
			const uint32_t iy = static_cast<uint32_t>(qy);
			const float depthInFrame = relative[0] * frame.Forward[0] + relative[1] * frame.Forward[1] + relative[2] * frame.Forward[2];
			const float tolerance = 0.01f * depthInFrame + 0.01f;
			const float fx = x - qx;
			const float fy = y - qy;
			float sum = 0.0f;
			for (uint32_t dy = 0; dy < 2; dy++)
			{
				for (uint32_t dx = 0; dx < 2; dx++)
				{
					const size_t texel = static_cast<size_t>(iy + dy) * frame.Width + ix + dx;
					const glm::vec3& texelNormal = frame.Normal[texel];
					if (!(std::abs(frame.ViewDepth[texel] - depthInFrame) <= tolerance)
						|| !(texelNormal.x * normal.x + texelNormal.y * normal.y + texelNormal.z * normal.z >= 0.98f))
						return false;
					const float weight = (dx ? fx : 1.0f - fx) * (dy ? fy : 1.0f - fy);
					sum += weight * frame.Value[texel];
				}
			}
			value = sum;
			return true;
		}

	}

	FlickerFrame ReadFlickerFrame(SceneRenderer& renderer, const SceneCamera& camera)
	{
		FlickerFrame frame = ReadFrameGeometry(renderer, camera);
		ReadbackImage hdr;
		REQUIRE(Renderer::ReadTexture(renderer.GetHDRTexture(), hdr));
		REQUIRE(hdr.Format == nvrhi::Format::RGBA16_FLOAT);
		REQUIRE(hdr.Width == frame.Width);
		REQUIRE(hdr.Height == frame.Height);
		const size_t pixels = static_cast<size_t>(frame.Width) * frame.Height;
		frame.Value.resize(pixels);
		float* value = frame.Value.data();
		const uint8_t* bytes = hdr.Pixels.data();
		const std::vector<float>& halves = GetHalfTable();
		for (size_t pixel = 0; pixel < pixels; pixel++)
		{
			uint16_t texel[4];
			std::memcpy(texel, bytes + pixel * sizeof(texel), sizeof(texel));
			value[pixel] = Luminance(halves[texel[0]], halves[texel[1]], halves[texel[2]]);
		}
		return frame;
	}

	FlickerFrame ReadDisplayFlickerFrame(SceneRenderer& renderer, const SceneCamera& camera)
	{
		FlickerFrame frame = ReadFrameGeometry(renderer, camera);
		ReadbackImage output;
		REQUIRE(Renderer::ReadTexture(renderer.GetOutputTexture(), output));
		REQUIRE(output.Format == nvrhi::Format::RGBA8_UNORM);
		REQUIRE(output.Width == frame.Width);
		REQUIRE(output.Height == frame.Height);
		float decoded[256];
		for (uint32_t code = 0; code < 256; code++)
			decoded[code] = DecodeSRGB(static_cast<uint8_t>(code));
		const size_t pixels = static_cast<size_t>(frame.Width) * frame.Height;
		frame.Value.resize(pixels);
		float* value = frame.Value.data();
		const uint8_t* bytes = output.Pixels.data();
		for (size_t pixel = 0; pixel < pixels; pixel++)
		{
			const uint8_t* texel = bytes + pixel * output.BytesPerPixel;
			value[pixel] = Luminance(decoded[texel[0]], decoded[texel[1]], decoded[texel[2]]);
		}
		return frame;
	}

	float GetDisplayLuminance(uint8_t red, uint8_t green, uint8_t blue)
	{
		return Luminance(DecodeSRGB(red), DecodeSRGB(green), DecodeSRGB(blue));
	}

	FlickerResult MeasureFlicker(const FlickerFrame& a, const FlickerFrame& b, const FlickerFrame& c, uint32_t blockPixels)
	{
		REQUIRE(blockPixels > 0);
		for (const FlickerFrame* frame : { &a, &b, &c })
		{
			REQUIRE(frame->Width == c.Width);
			REQUIRE(frame->Height == c.Height);
			const size_t pixels = static_cast<size_t>(c.Width) * c.Height;
			REQUIRE(frame->Value.size() == pixels);
			REQUIRE(frame->ViewDepth.size() == pixels);
			REQUIRE(frame->Normal.size() == pixels);
		}
		FlickerResult result;
		result.Frames = 1;
		const uint32_t width = c.Width;
		const uint32_t height = c.Height;
		if (width < 5 || height < 5)
			return result;

		// A pixel's world point: the ray through its center (two points of it unprojected by c's matrix, relative to c's camera)
		// at its view depth.
		const FrameView newest(c, glm::inverse(c.ViewProjection));
		const FrameView oldest(a, a.ViewProjection);
		const FrameView middle(b, b.ViewProjection);
		const uint32_t blocksX = width / blockPixels;
		const uint32_t blocksY = height / blockPixels;
		std::vector<double> blockSum(static_cast<size_t>(blocksX) * blocksY, 0.0);
		std::vector<uint32_t> blockCount(blockSum.size(), 0);
		std::vector<double> magnitudes;
		magnitudes.reserve(static_cast<size_t>(width) * height / 4);
		const float(&inverse)[4][4] = newest.Matrix;
		for (uint32_t y = 2; y < height - 2; y++)
		{
			const float ndcY = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
			for (uint32_t x = 2; x < width - 2; x++)
			{
				const size_t pixel = static_cast<size_t>(y) * width + x;
				const float depth = newest.ViewDepth[pixel];
				if (!(depth > 0.0f))
					continue;
				const float ndcX = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(width) - 1.0f;
				float nearClip[4];
				float farClip[4];
				for (int row = 0; row < 4; row++)
				{
					const float common = inverse[0][row] * ndcX + inverse[1][row] * ndcY + inverse[3][row];
					nearClip[row] = common + inverse[2][row];
					farClip[row] = common + 0.5f * inverse[2][row];
				}
				float nearPoint[3];
				float along[3];
				for (int axis = 0; axis < 3; axis++)
				{
					nearPoint[axis] = nearClip[axis] / nearClip[3];
					along[axis] = farClip[axis] / farClip[3] - nearPoint[axis];
				}
				const float alongDepth = along[0] * newest.Forward[0] + along[1] * newest.Forward[1] + along[2] * newest.Forward[2];
				if (!(std::abs(alongDepth) > 1e-12f))
					continue;
				const float t = (depth - (nearPoint[0] * newest.Forward[0] + nearPoint[1] * newest.Forward[1] + nearPoint[2] * newest.Forward[2])) / alongDepth;
				double point[3];
				for (int axis = 0; axis < 3; axis++)
					point[axis] = newest.Camera[axis] + static_cast<double>(nearPoint[axis] + along[axis] * t);
				const glm::vec3& normal = newest.Normal[pixel];
				float valueA = 0.0f;
				float valueB = 0.0f;
				if (!SampleEarlierFrame(oldest, point, normal, valueA) || !SampleEarlierFrame(middle, point, normal, valueB))
					continue;
				const double valueC = newest.Value[pixel];
				const double mean = (static_cast<double>(valueA) + valueB + valueC) / 3.0;
				const double d = (valueC - 2.0 * valueB + valueA) / std::max(mean, 1e-3);
				magnitudes.push_back(std::abs(d));
				const uint32_t blockX = x / blockPixels;
				const uint32_t blockY = y / blockPixels;
				if (blockX < blocksX && blockY < blocksY)
				{
					const size_t block = static_cast<size_t>(blockY) * blocksX + blockX;
					blockSum[block] += d;
					blockCount[block]++;
				}
			}
		}
		result.MeasuredShare = static_cast<double>(magnitudes.size()) / (static_cast<double>(width) * height);
		result.Mean = Mean(magnitudes);
		result.P99 = Percentile99(magnitudes);
		std::vector<double> blocks;
		const uint32_t blockArea = blockPixels * blockPixels;
		for (size_t block = 0; block < blockSum.size(); block++)
		{
			// A block counts when more than half of its pixels were measured.
			if (2 * blockCount[block] > blockArea)
				blocks.push_back(std::abs(blockSum[block] / blockCount[block]));
		}
		result.Blocks = Mean(blocks);
		result.BlocksP99 = Percentile99(blocks);
		return result;
	}

	FlickerSequence::FlickerSequence(uint32_t firstFrame, uint32_t blockPixels)
		: m_FirstFrame(std::max(firstFrame, 2u)), m_BlockPixels(blockPixels)
	{
	}

	void FlickerSequence::Add(FlickerFrame frame)
	{
		m_Frames.push_back(std::move(frame));
		if (m_Frames.size() > 3)
			m_Frames.pop_front();
		const uint32_t index = m_Added++;
		if (index < m_FirstFrame || m_Frames.size() < 3)
			return;
		const FlickerResult triple = MeasureFlicker(m_Frames[0], m_Frames[1], m_Frames[2], m_BlockPixels);
		m_Sum.Mean += triple.Mean;
		m_Sum.P99 += triple.P99;
		m_Sum.Blocks += triple.Blocks;
		m_Sum.BlocksP99 += triple.BlocksP99;
		m_Sum.MeasuredShare += triple.MeasuredShare;
		m_Sum.Frames++;
	}

	FlickerResult FlickerSequence::GetResult() const
	{
		FlickerResult result;
		if (m_Sum.Frames == 0)
			return result;
		const double count = static_cast<double>(m_Sum.Frames);
		result.Mean = m_Sum.Mean / count;
		result.P99 = m_Sum.P99 / count;
		result.Blocks = m_Sum.Blocks / count;
		result.BlocksP99 = m_Sum.BlocksP99 / count;
		result.MeasuredShare = m_Sum.MeasuredShare / count;
		result.Frames = m_Sum.Frames;
		return result;
	}

}
