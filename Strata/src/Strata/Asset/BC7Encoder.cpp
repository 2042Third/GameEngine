#include "stpch.h"
#include "Strata/Asset/BC7Encoder.h"

#include "Strata/Core/Assert.h"
#include "Strata/Renderer/BC7Format.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace Strata::BC7Encoder
{

	namespace
	{

		constexpr uint32_t c_NoEncoding = std::numeric_limits<uint32_t>::max();

		// Least-squares refits of the endpoints per subset (each stops early once it no longer lowers the error): High
		// screens candidates with their first endpoints and refits those it polishes; Fast refits its candidates once.
		constexpr uint32_t c_ScreeningRefinements = 0;
		constexpr uint32_t c_PolishingRefinements = 3;
		constexpr uint32_t c_FastRefinements = 1;
		// Partitions screened per mode with two or three subsets at High: the best by the estimate.
		constexpr uint32_t c_TwoSubsetCandidates = 2;
		constexpr uint32_t c_ThreeSubsetCandidates = 2;
		// Mode 0 rarely wins: mode 2 has its partitions too, with endpoints at least as precise, and blocks of three regions
		// with more than four levels each are rare.
		constexpr uint32_t c_ModeZeroCandidates = 1;
		// An error the search does not try to lower with the modes that have partitions: one squared step per texel (53 dB
		// for opaque color). Opaque mode 6 stores only odd values, so a block of two colors with even values errs by 48 in
		// it; mode 3 stores those exactly.
		constexpr uint32_t c_NegligibleError = 16;
		// Power iterations for the principal axis of a subset fit: from a column of the scatter matrix they converge in a few.
		constexpr uint32_t c_FitAxisIterations = 4;

		// The block being encoded.
		struct SourceBlock
		{
			int32_t Texels[16][4] = {};
			uint32_t AlphaError = 0; // The error of modes without alpha, which decode it as 255
			bool Opaque = true;      // Every alpha is 255: every candidate must decode alpha 255
			bool AlphaVaries = false;
			bool Flat = true;        // Every texel is the same
		};

		enum class PBitKind : uint8_t
		{
			None = 0,
			Shared,     // One per subset
			PerEndpoint
		};

		// How a subset stores its endpoints and indices. A subset fit of one channel (the separate alpha of modes 4 and 5)
		// uses ColorBits for it.
		struct SubsetFormat
		{
			uint32_t ColorBits = 0;
			uint32_t AlphaBits = 0;
			PBitKind PBits = PBitKind::None;
			uint32_t IndexBits = 0;
		};

		// How much work a fit does: least-squares refits of the endpoints (each stops early once it no longer lowers the
		// error), and whether every combination of p-bits is tried on the best endpoints.
		struct FitEffort
		{
			uint32_t Refinements = 1;
			bool AllPBits = false;
		};

		// The texels of a subset with C channels each.
		template<uint32_t C>
		struct SubsetPoints
		{
			uint32_t Count = 0;
			int32_t Values[16][C] = {};
		};

		// A subset's encoding: its stored endpoint values (without p-bits), p-bits and the index of each of its texels.
		struct SubsetEncoding
		{
			uint32_t Error = c_NoEncoding;
			uint8_t Raw[2][4] = {};
			uint8_t PBits[2] = {};
			uint8_t Indices[16] = {};
		};

		// A whole block's encoding in the format's terms, before it is packed into bits.
		struct BlockEncoding
		{
			uint32_t Mode = 0;
			uint32_t Partition = 0;
			uint32_t Rotation = 0;
			uint32_t IndexSelection = 0;
			uint8_t Raw[6][4] = {};        // Stored values of endpoint (subset * 2 + 0 or 1), per channel, without p-bits
			uint8_t PBits[6] = {};         // Per endpoint (shared p-bits: both endpoints of a subset hold it)
			uint8_t ColorIndices[16] = {}; // Per texel: its index (modes 4 and 5: its color index)
			uint8_t AlphaIndices[16] = {}; // Modes 4 and 5: its alpha index
			uint32_t Error = c_NoEncoding;
		};

		constexpr const BC7::ModeInfo& GetMode(uint32_t mode)
		{
			return BC7::c_Modes[mode];
		}

		PBitKind GetPBitKind(const BC7::ModeInfo& mode)
		{
			if (mode.EndpointPBits)
				return PBitKind::PerEndpoint;
			return mode.SharedPBits ? PBitKind::Shared : PBitKind::None;
		}

		// The index bits of a mode 4 or 5 block's color and alpha.
		uint32_t GetColorIndexBits(const BlockEncoding& encoding)
		{
			const BC7::ModeInfo& mode = GetMode(encoding.Mode);
			return encoding.IndexSelection ? mode.SecondaryIndexBits : mode.IndexBits;
		}

		uint32_t GetAlphaIndexBits(const BlockEncoding& encoding)
		{
			const BC7::ModeInfo& mode = GetMode(encoding.Mode);
			return encoding.IndexSelection ? mode.IndexBits : mode.SecondaryIndexBits;
		}

		// The 8-bit value of a stored endpoint value of `bits` bits, with p-bit `pBit` appended when hasPBit.
		uint32_t Expand(uint32_t stored, uint32_t bits, bool hasPBit, uint32_t pBit)
		{
			return hasPBit ? BC7::ExpandBits((stored << 1) | pBit, bits + 1) : BC7::ExpandBits(stored, bits);
		}

		// For every precision a mode stores endpoint values in (4 to 8 stored bits alone, or 4 to 7 followed by a p-bit of 0
		// or 1), the stored value whose expansion is nearest to each 8-bit value (the lower one of two as near), and its
		// expansion.
		struct QuantizationTable
		{
			uint8_t Stored[9][3][256] = {};   // [stored bits][0: no p-bit, 1: p-bit 0, 2: p-bit 1][value]
			uint8_t Expanded[9][3][256] = {};
		};

		QuantizationTable BuildQuantizationTable()
		{
			QuantizationTable table;
			for (uint32_t bits = 4; bits <= 8; bits++)
			{
				for (uint32_t kind = 0; kind < 3; kind++)
				{
					const bool hasPBit = kind > 0;
					// Eight bits and a p-bit would make nine, more than an expanded value holds; no mode stores that.
					if (hasPBit && bits == 8)
						continue;
					for (uint32_t value = 0; value < 256; value++)
					{
						uint32_t bestDistance = 256;
						for (uint32_t stored = 0; stored < (1u << bits); stored++)
						{
							const uint32_t expanded = Expand(stored, bits, hasPBit, hasPBit ? kind - 1 : 0);
							const uint32_t distance = expanded > value ? expanded - value : value - expanded;
							if (distance < bestDistance)
							{
								bestDistance = distance;
								table.Stored[bits][kind][value] = static_cast<uint8_t>(stored);
								table.Expanded[bits][kind][value] = static_cast<uint8_t>(expanded);
							}
						}
					}
				}
			}
			return table;
		}

		const QuantizationTable& GetQuantizationTable()
		{
			static const QuantizationTable s_Table = BuildQuantizationTable();
			return s_Table;
		}

		// The table row of a channel's precision: kind 0 without p-bits, else 1 + the p-bit.
		uint32_t GetQuantizationKind(const SubsetFormat& format, uint32_t pBit)
		{
			return format.PBits == PBitKind::None ? 0 : 1 + pBit;
		}

		uint32_t ToTableIndex(float value)
		{
			return static_cast<uint32_t>(std::clamp(value, 0.0f, 255.0f) + 0.5f);
		}

		// The largest eigenvalue of a symmetric positive semi-definite matrix and its eigenvector (unit length), by power
		// iteration from the column of the largest diagonal entry, which lies in the matrix's range and so is never
		// orthogonal to the principal axis. A matrix without spread gives 0 and the first axis.
		template<uint32_t C>
		float FindPrincipalAxis(const float (&matrix)[C][C], uint32_t iterations, float (&outAxis)[C])
		{
			uint32_t largest = 0;
			for (uint32_t row = 1; row < C; row++)
			{
				if (matrix[row][row] > matrix[largest][largest])
					largest = row;
			}
			for (uint32_t row = 0; row < C; row++)
				outAxis[row] = row == 0 ? 1.0f : 0.0f;
			if (matrix[largest][largest] <= 1e-3f)
				return 0.0f;

			float axis[C];
			for (uint32_t row = 0; row < C; row++)
				axis[row] = matrix[row][largest];
			for (uint32_t iteration = 0; iteration < iterations; iteration++)
			{
				float next[C] = {};
				float scale = 0.0f;
				for (uint32_t row = 0; row < C; row++)
				{
					for (uint32_t column = 0; column < C; column++)
						next[row] += matrix[row][column] * axis[column];
					scale = std::max(scale, std::abs(next[row]));
				}
				if (scale <= 1e-12f)
					break;
				const float inverseScale = 1.0f / scale;
				for (uint32_t row = 0; row < C; row++)
					axis[row] = next[row] * inverseScale;
			}

			float lengthSquared = 0.0f;
			for (uint32_t row = 0; row < C; row++)
				lengthSquared += axis[row] * axis[row];
			if (lengthSquared <= 1e-24f)
				return 0.0f;
			const float inverseLength = 1.0f / std::sqrt(lengthSquared);
			float eigenvalue = 0.0f;
			for (uint32_t row = 0; row < C; row++)
			{
				outAxis[row] = axis[row] * inverseLength;
				float product = 0.0f;
				for (uint32_t column = 0; column < C; column++)
					product += matrix[row][column] * axis[column] * inverseLength;
				eigenvalue += outAxis[row] * product;
			}
			return std::max(eigenvalue, 0.0f);
		}

		template<uint32_t C>
		uint32_t GetSquaredDistance(const int32_t (&first)[C], const int32_t (&second)[C])
		{
			uint32_t distance = 0;
			for (uint32_t channel = 0; channel < C; channel++)
			{
				const int32_t difference = first[channel] - second[channel];
				distance += static_cast<uint32_t>(difference * difference);
			}
			return distance;
		}

		// Assigns every texel of a subset the index of the nearest interpolated color between two expanded endpoints and
		// returns the subset's error. Four colors are compared one by one; of eight or sixteen, the texel's projection onto
		// the line between the endpoints picks the nearest by position along it, which is compared with its neighbors (the
		// interpolated colors are rounded off the line, and the weights are not quite even).
		template<uint32_t C>
		uint32_t AssignIndices(const SubsetPoints<C>& points, const int32_t (&expanded)[2][C], uint32_t indexBits, uint8_t* outIndices)
		{
			const uint8_t* weights = BC7::GetWeights(indexBits);
			const int32_t indexCount = 1 << indexBits;
			int32_t palette[16][C];
			for (int32_t index = 0; index < indexCount; index++)
			{
				for (uint32_t channel = 0; channel < C; channel++)
					palette[index][channel] = BC7::Interpolate(static_cast<uint32_t>(expanded[0][channel]), static_cast<uint32_t>(expanded[1][channel]), weights[index]);
			}

			float direction[C];
			float lengthSquared = 0.0f;
			for (uint32_t channel = 0; channel < C; channel++)
			{
				direction[channel] = static_cast<float>(expanded[1][channel] - expanded[0][channel]);
				lengthSquared += direction[channel] * direction[channel];
			}
			const bool project = indexCount > 4 && lengthSquared > 0.0f;
			const float scale = project ? static_cast<float>(indexCount - 1) / lengthSquared : 0.0f;

			uint32_t total = 0;
			for (uint32_t texel = 0; texel < points.Count; texel++)
			{
				int32_t first = 0;
				int32_t last = indexCount - 1;
				if (project)
				{
					float position = 0.0f;
					for (uint32_t channel = 0; channel < C; channel++)
						position += static_cast<float>(points.Values[texel][channel] - expanded[0][channel]) * direction[channel];
					const int32_t nearest = static_cast<int32_t>(std::clamp(position * scale, 0.0f, static_cast<float>(indexCount - 1)) + 0.5f);
					first = std::max(nearest - 1, 0);
					last = std::min(nearest + 1, indexCount - 1);
				}
				uint32_t bestError = c_NoEncoding;
				int32_t bestIndex = first;
				for (int32_t index = first; index <= last; index++)
				{
					const uint32_t error = GetSquaredDistance<C>(palette[index], points.Values[texel]);
					if (error < bestError)
					{
						bestError = error;
						bestIndex = index;
					}
				}
				outIndices[texel] = static_cast<uint8_t>(bestIndex);
				total += bestError;
			}
			return total;
		}

		// Quantizes endpoints with the given p-bits and assigns the indices. A pinned channel (alpha of an opaque block)
		// stores 255 exactly: its stored values are all ones (its p-bits must be 1).
		template<uint32_t C>
		SubsetEncoding QuantizeEndpoints(const SubsetPoints<C>& points, const SubsetFormat& format, int32_t pinnedChannel, const float (&endpoints)[2][C],
			const uint32_t (&pBits)[2])
		{
			const QuantizationTable& table = GetQuantizationTable();
			SubsetEncoding encoding;
			int32_t expanded[2][C];
			for (uint32_t endpoint = 0; endpoint < 2; endpoint++)
			{
				encoding.PBits[endpoint] = static_cast<uint8_t>(pBits[endpoint]);
				const uint32_t kind = GetQuantizationKind(format, pBits[endpoint]);
				for (uint32_t channel = 0; channel < C; channel++)
				{
					const uint32_t bits = channel < 3 ? format.ColorBits : format.AlphaBits;
					const uint32_t value = static_cast<int32_t>(channel) == pinnedChannel ? 255u : ToTableIndex(endpoints[endpoint][channel]);
					encoding.Raw[endpoint][channel] = table.Stored[bits][kind][value];
					expanded[endpoint][channel] = table.Expanded[bits][kind][value];
				}
			}
			encoding.Error = AssignIndices(points, expanded, format.IndexBits, encoding.Indices);
			return encoding;
		}

		// The p-bits whose expansions lie nearest to the endpoints: chosen per endpoint, or per subset when they are shared.
		// A pinned channel needs p-bits of 1.
		template<uint32_t C>
		void ChoosePBits(const SubsetFormat& format, int32_t pinnedChannel, const float (&endpoints)[2][C], uint32_t (&outPBits)[2])
		{
			outPBits[0] = 0;
			outPBits[1] = 0;
			if (format.PBits == PBitKind::None)
				return;
			if (pinnedChannel >= 0)
			{
				outPBits[0] = 1;
				outPBits[1] = 1;
				return;
			}

			const QuantizationTable& table = GetQuantizationTable();
			float distances[2][2] = {};
			for (uint32_t endpoint = 0; endpoint < 2; endpoint++)
			{
				for (uint32_t pBit = 0; pBit < 2; pBit++)
				{
					for (uint32_t channel = 0; channel < C; channel++)
					{
						const uint32_t bits = channel < 3 ? format.ColorBits : format.AlphaBits;
						const float value = std::clamp(endpoints[endpoint][channel], 0.0f, 255.0f);
						const float difference = static_cast<float>(table.Expanded[bits][1 + pBit][ToTableIndex(value)]) - value;
						distances[endpoint][pBit] += difference * difference;
					}
				}
			}
			if (format.PBits == PBitKind::PerEndpoint)
			{
				outPBits[0] = distances[0][1] < distances[0][0] ? 1 : 0;
				outPBits[1] = distances[1][1] < distances[1][0] ? 1 : 0;
			}
			else
			{
				outPBits[0] = outPBits[1] = distances[0][1] + distances[1][1] < distances[0][0] + distances[1][0] ? 1 : 0;
			}
		}

		// Fits a subset's endpoints and indices (see the header): p-bits follow the endpoints while they are refitted, and
		// with effort.AllPBits every other combination is tried on the best endpoints at the end.
		template<uint32_t C>
		SubsetEncoding FitSubset(const SubsetPoints<C>& points, const SubsetFormat& format, int32_t pinnedChannel, const FitEffort& effort)
		{
			SubsetEncoding best;
			if (points.Count == 0)
			{
				best.Error = 0;
				return best;
			}

			float mean[C] = {};
			for (uint32_t texel = 0; texel < points.Count; texel++)
			{
				for (uint32_t channel = 0; channel < C; channel++)
					mean[channel] += static_cast<float>(points.Values[texel][channel]);
			}
			for (uint32_t channel = 0; channel < C; channel++)
				mean[channel] /= static_cast<float>(points.Count);

			float scatter[C][C] = {};
			for (uint32_t texel = 0; texel < points.Count; texel++)
			{
				float offset[C];
				for (uint32_t channel = 0; channel < C; channel++)
					offset[channel] = static_cast<float>(points.Values[texel][channel]) - mean[channel];
				for (uint32_t row = 0; row < C; row++)
				{
					for (uint32_t column = row; column < C; column++)
						scatter[row][column] += offset[row] * offset[column];
				}
			}
			for (uint32_t row = 0; row < C; row++)
			{
				for (uint32_t column = 0; column < row; column++)
					scatter[row][column] = scatter[column][row];
			}

			// The first endpoints: the extent of the texels along their principal axis.
			float axis[C];
			FindPrincipalAxis(scatter, c_FitAxisIterations, axis);
			float lowest = 0.0f;
			float highest = 0.0f;
			for (uint32_t texel = 0; texel < points.Count; texel++)
			{
				float projection = 0.0f;
				for (uint32_t channel = 0; channel < C; channel++)
					projection += (static_cast<float>(points.Values[texel][channel]) - mean[channel]) * axis[channel];
				lowest = std::min(lowest, projection);
				highest = std::max(highest, projection);
			}
			float endpoints[2][C];
			for (uint32_t channel = 0; channel < C; channel++)
			{
				endpoints[0][channel] = std::clamp(mean[channel] + axis[channel] * lowest, 0.0f, 255.0f);
				endpoints[1][channel] = std::clamp(mean[channel] + axis[channel] * highest, 0.0f, 255.0f);
			}

			float bestEndpoints[2][C] = {};
			auto tryEndpoints = [&]()
			{
				uint32_t pBits[2];
				ChoosePBits(format, pinnedChannel, endpoints, pBits);
				const SubsetEncoding candidate = QuantizeEndpoints(points, format, pinnedChannel, endpoints, pBits);
				if (candidate.Error >= best.Error)
					return false;
				best = candidate;
				std::memcpy(bestEndpoints, endpoints, sizeof(endpoints));
				return true;
			};
			tryEndpoints();

			// Refit the endpoints to the index assignment by least squares: minimize the sum over texels of
			// |(1 - t) e0 + t e1 - x|^2, where t is the texel's weight.
			const uint8_t* weights = BC7::GetWeights(format.IndexBits);
			for (uint32_t refinement = 0; refinement < effort.Refinements && best.Error > 0; refinement++)
			{
				float firstFirst = 0.0f;
				float firstSecond = 0.0f;
				float secondSecond = 0.0f;
				float firstTarget[C] = {};
				float secondTarget[C] = {};
				for (uint32_t texel = 0; texel < points.Count; texel++)
				{
					const float second = static_cast<float>(weights[best.Indices[texel]]) * (1.0f / 64.0f);
					const float first = 1.0f - second;
					firstFirst += first * first;
					firstSecond += first * second;
					secondSecond += second * second;
					for (uint32_t channel = 0; channel < C; channel++)
					{
						firstTarget[channel] += first * static_cast<float>(points.Values[texel][channel]);
						secondTarget[channel] += second * static_cast<float>(points.Values[texel][channel]);
					}
				}
				const float determinant = firstFirst * secondSecond - firstSecond * firstSecond;
				// Every texel has the same weight: there is nothing to refit.
				if (determinant <= 1e-6f)
					break;

				const float inverseDeterminant = 1.0f / determinant;
				for (uint32_t channel = 0; channel < C; channel++)
				{
					endpoints[0][channel] = std::clamp((secondSecond * firstTarget[channel] - firstSecond * secondTarget[channel]) * inverseDeterminant, 0.0f, 255.0f);
					endpoints[1][channel] = std::clamp((firstFirst * secondTarget[channel] - firstSecond * firstTarget[channel]) * inverseDeterminant, 0.0f, 255.0f);
				}
				if (!tryEndpoints())
					break;
			}

			// Every other combination of p-bits on the best endpoints.
			if (effort.AllPBits && format.PBits != PBitKind::None && pinnedChannel < 0 && best.Error > 0)
			{
				const bool perEndpoint = format.PBits == PBitKind::PerEndpoint;
				const uint8_t chosen[2] = { best.PBits[0], best.PBits[1] };
				for (uint32_t combination = 0; combination < (perEndpoint ? 4u : 2u); combination++)
				{
					const uint32_t pBits[2] = { combination & 1u, perEndpoint ? (combination >> 1) & 1u : combination & 1u };
					if (pBits[0] == chosen[0] && pBits[1] == chosen[1])
						continue;
					const SubsetEncoding candidate = QuantizeEndpoints(points, format, pinnedChannel, bestEndpoints, pBits);
					if (candidate.Error < best.Error)
						best = candidate;
				}
			}
			return best;
		}

		SourceBlock LoadBlock(const uint8_t* rgba)
		{
			SourceBlock block;
			int32_t lowestAlpha = 255;
			int32_t highestAlpha = 0;
			for (uint32_t texel = 0; texel < 16; texel++)
			{
				for (uint32_t channel = 0; channel < 4; channel++)
				{
					block.Texels[texel][channel] = rgba[texel * 4 + channel];
					if (block.Texels[texel][channel] != block.Texels[0][channel])
						block.Flat = false;
				}
				const int32_t alpha = block.Texels[texel][3];
				lowestAlpha = std::min(lowestAlpha, alpha);
				highestAlpha = std::max(highestAlpha, alpha);
				block.AlphaError += static_cast<uint32_t>((255 - alpha) * (255 - alpha));
			}
			block.Opaque = lowestAlpha == 255;
			block.AlphaVaries = lowestAlpha != highestAlpha;
			return block;
		}

		// The texels of `subset` in a partition, as points of the given source channels.
		template<uint32_t C>
		SubsetPoints<C> GatherPoints(const SourceBlock& block, const uint8_t* texels, uint32_t count, const uint32_t (&channels)[C])
		{
			SubsetPoints<C> points;
			points.Count = count;
			for (uint32_t point = 0; point < count; point++)
			{
				for (uint32_t channel = 0; channel < C; channel++)
					points.Values[point][channel] = block.Texels[texels[point]][channels[channel]];
			}
			return points;
		}

		// What a candidate encoding is before it is fitted: the mode and the choices it makes about texels and channels.
		struct CandidateShape
		{
			uint32_t Mode = 0;
			uint32_t Partition = 0;      // Modes with several subsets
			uint32_t Rotation = 0;       // Modes 4 and 5
			uint32_t IndexSelection = 0; // Mode 4
		};

		// Encodes a block in a mode with one index per texel (all but 4 and 5) and a partition. Gives up (an error of
		// c_NoEncoding) once the error reaches `bound`, as the encoding could no longer be among the best.
		BlockEncoding EncodeSubsets(const SourceBlock& block, const CandidateShape& shape, const FitEffort& effort, uint32_t bound)
		{
			const BC7::ModeInfo& mode = GetMode(shape.Mode);
			const SubsetFormat format { mode.ColorBits, mode.AlphaBits, GetPBitKind(mode), mode.IndexBits };
			BlockEncoding encoding;
			encoding.Mode = shape.Mode;
			encoding.Partition = shape.Partition;
			uint32_t error = mode.AlphaBits == 0 ? block.AlphaError : 0;
			for (uint32_t subset = 0; subset < mode.Subsets; subset++)
			{
				if (error >= bound)
					return BlockEncoding {};

				uint8_t texels[16];
				uint32_t count = 0;
				for (uint32_t texel = 0; texel < 16; texel++)
				{
					if (BC7::GetSubset(mode.Subsets, shape.Partition, texel) == subset)
						texels[count++] = static_cast<uint8_t>(texel);
				}

				SubsetEncoding fit;
				if (mode.AlphaBits > 0)
					fit = FitSubset(GatherPoints(block, texels, count, { 0u, 1u, 2u, 3u }), format, block.Opaque ? 3 : -1, effort);
				else
					fit = FitSubset(GatherPoints(block, texels, count, { 0u, 1u, 2u }), format, -1, effort);
				error += fit.Error;
				for (uint32_t endpoint = 0; endpoint < 2; endpoint++)
				{
					std::memcpy(encoding.Raw[subset * 2 + endpoint], fit.Raw[endpoint], 4);
					encoding.PBits[subset * 2 + endpoint] = fit.PBits[endpoint];
				}
				for (uint32_t point = 0; point < count; point++)
					encoding.ColorIndices[texels[point]] = fit.Indices[point];
			}
			if (error >= bound)
				return BlockEncoding {};
			encoding.Error = error;
			return encoding;
		}

		// Encodes a block in mode 4 or 5 with a rotation (and, in mode 4, an index selection): color and alpha are fitted
		// apart, after swapping alpha with channel rotation - 1 (which decoding swaps back).
		BlockEncoding EncodeRotated(const SourceBlock& block, const CandidateShape& shape, const FitEffort& effort, uint32_t bound)
		{
			BlockEncoding encoding;
			encoding.Mode = shape.Mode;
			encoding.Rotation = shape.Rotation;
			encoding.IndexSelection = shape.IndexSelection;
			const BC7::ModeInfo& mode = GetMode(shape.Mode);
			const SubsetFormat colorFormat { mode.ColorBits, 0, PBitKind::None, GetColorIndexBits(encoding) };
			const SubsetFormat alphaFormat { mode.AlphaBits, 0, PBitKind::None, GetAlphaIndexBits(encoding) };

			uint32_t colorChannels[3] = { 0, 1, 2 };
			uint32_t alphaChannel[1] = { 3 };
			if (shape.Rotation > 0)
			{
				colorChannels[shape.Rotation - 1] = 3;
				alphaChannel[0] = shape.Rotation - 1;
			}
			constexpr uint8_t c_AllTexels[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
			// In an opaque block, alpha (in a color channel after a rotation) stores 255 exactly.
			const int32_t pinnedColor = block.Opaque && shape.Rotation > 0 ? static_cast<int32_t>(shape.Rotation - 1) : -1;
			const int32_t pinnedAlpha = block.Opaque && shape.Rotation == 0 ? 0 : -1;
			const SubsetEncoding alpha = FitSubset(GatherPoints(block, c_AllTexels, 16, alphaChannel), alphaFormat, pinnedAlpha, effort);
			if (alpha.Error >= bound)
				return BlockEncoding {};
			const SubsetEncoding color = FitSubset(GatherPoints(block, c_AllTexels, 16, colorChannels), colorFormat, pinnedColor, effort);
			if (alpha.Error + color.Error >= bound)
				return BlockEncoding {};
			for (uint32_t endpoint = 0; endpoint < 2; endpoint++)
			{
				std::memcpy(encoding.Raw[endpoint], color.Raw[endpoint], 3);
				encoding.Raw[endpoint][3] = alpha.Raw[endpoint][0];
			}
			std::memcpy(encoding.ColorIndices, color.Indices, 16);
			std::memcpy(encoding.AlphaIndices, alpha.Indices, 16);
			encoding.Error = color.Error + alpha.Error;
			return encoding;
		}

		BlockEncoding EncodeCandidate(const SourceBlock& block, const CandidateShape& shape, const FitEffort& effort, uint32_t bound)
		{
			return shape.Mode == 4 || shape.Mode == 5 ? EncodeRotated(block, shape, effort, bound) : EncodeSubsets(block, shape, effort, bound);
		}

		// Stored mode 5 color endpoints that interpolate exactly (or as near as can be) to each 8-bit value, per index
		// of the two inner weights (21 and 43 of 64).
		struct FlatColorTable
		{
			struct Entry
			{
				uint8_t Low = 0;
				uint8_t High = 0;
				uint8_t Error = 255; // |interpolated - value|
			};
			Entry Entries[2][256];
		};

		FlatColorTable BuildFlatColorTable()
		{
			FlatColorTable table;
			const uint32_t colorBits = GetMode(5).ColorBits;
			const uint32_t storedCount = 1u << colorBits;
			for (uint32_t inner = 0; inner < 2; inner++)
			{
				const uint32_t weight = BC7::c_Weights2[inner + 1];
				for (uint32_t low = 0; low < storedCount; low++)
				{
					for (uint32_t high = 0; high < storedCount; high++)
					{
						const uint32_t value = BC7::Interpolate(BC7::ExpandBits(low, colorBits), BC7::ExpandBits(high, colorBits), weight);
						FlatColorTable::Entry& entry = table.Entries[inner][value];
						if (entry.Error != 0)
							entry = { static_cast<uint8_t>(low), static_cast<uint8_t>(high), 0 };
					}
				}
				// Values no pair reaches exactly take the pair of the nearest value that is reached.
				for (uint32_t value = 0; value < 256; value++)
				{
					FlatColorTable::Entry& entry = table.Entries[inner][value];
					for (uint32_t distance = 1; entry.Error != 0 && distance < 256; distance++)
					{
						for (const int32_t neighbor : { static_cast<int32_t>(value) - static_cast<int32_t>(distance), static_cast<int32_t>(value + distance) })
						{
							if (neighbor >= 0 && neighbor < 256 && table.Entries[inner][neighbor].Error == 0)
							{
								entry = { table.Entries[inner][neighbor].Low, table.Entries[inner][neighbor].High, static_cast<uint8_t>(distance) };
								break;
							}
						}
					}
				}
			}
			return table;
		}

		// A block whose texels are all the same, in mode 5: each color channel from the table, alpha from its 8 bits.
		BlockEncoding EncodeFlat(const SourceBlock& block)
		{
			static const FlatColorTable s_Table = BuildFlatColorTable();
			uint32_t bestInner = 0;
			uint32_t bestError = c_NoEncoding;
			for (uint32_t inner = 0; inner < 2; inner++)
			{
				uint32_t error = 0;
				for (uint32_t channel = 0; channel < 3; channel++)
				{
					const uint32_t difference = s_Table.Entries[inner][block.Texels[0][channel]].Error;
					error += difference * difference * 16;
				}
				if (error < bestError)
				{
					bestError = error;
					bestInner = inner;
				}
			}

			BlockEncoding encoding;
			encoding.Mode = 5;
			for (uint32_t channel = 0; channel < 3; channel++)
			{
				const FlatColorTable::Entry& entry = s_Table.Entries[bestInner][block.Texels[0][channel]];
				encoding.Raw[0][channel] = entry.Low;
				encoding.Raw[1][channel] = entry.High;
			}
			encoding.Raw[0][3] = static_cast<uint8_t>(block.Texels[0][3]);
			encoding.Raw[1][3] = static_cast<uint8_t>(block.Texels[0][3]);
			std::fill(std::begin(encoding.ColorIndices), std::end(encoding.ColorIndices), static_cast<uint8_t>(bestInner + 1));
			encoding.Error = bestError;
			return encoding;
		}

		// 1 / n for the texel counts of a set (0 for none): the estimate divides by counts often.
		struct InverseCounts
		{
			float Values[17] = {};
		};

		constexpr InverseCounts BuildInverseCounts()
		{
			InverseCounts inverse;
			for (uint32_t count = 1; count <= 16; count++)
				inverse.Values[count] = 1.0f / static_cast<float>(count);
			return inverse;
		}

		constexpr InverseCounts c_InverseCounts = BuildInverseCounts();

		// The sums of a set of texels the partition estimate needs, in one row of 16 lanes so that adding sets vectorizes:
		// the count, the channel sums (r, g, b, a) and the products of channel pairs (rr, rg, rb, ra, gg, gb, ga, bb, ba, aa).
		// Floats hold them exactly (at most 16 * 255^2, below 2^24).
		struct TexelSums
		{
			static constexpr uint32_t c_CountLane = 0;
			static constexpr uint32_t c_SumLanes = 1;
			static constexpr uint32_t c_ProductLanes = 5;
			static constexpr uint32_t c_ProductLane[4][4] = { { 0, 1, 2, 3 }, { 1, 4, 5, 6 }, { 2, 5, 7, 8 }, { 3, 6, 8, 9 } };

			float Lanes[16] = {};

			// The terms one texel adds.
			static TexelSums FromTexel(const int32_t (&texel)[4])
			{
				TexelSums terms;
				terms.Lanes[c_CountLane] = 1.0f;
				for (uint32_t row = 0; row < 4; row++)
				{
					terms.Lanes[c_SumLanes + row] = static_cast<float>(texel[row]);
					for (uint32_t column = row; column < 4; column++)
						terms.Lanes[c_ProductLanes + c_ProductLane[row][column]] = static_cast<float>(texel[row] * texel[column]);
				}
				return terms;
			}

			TexelSums operator+(const TexelSums& other) const
			{
				TexelSums sum;
				for (uint32_t lane = 0; lane < 16; lane++)
					sum.Lanes[lane] = Lanes[lane] + other.Lanes[lane];
				return sum;
			}

			TexelSums operator-(const TexelSums& other) const
			{
				TexelSums difference;
				for (uint32_t lane = 0; lane < 16; lane++)
					difference.Lanes[lane] = Lanes[lane] - other.Lanes[lane];
				return difference;
			}
		};

		// How a set of texels spreads in C of its channels: the trace of its scatter matrix (the squared distances to the
		// mean) and an estimate of the part of it along the principal axis.
		struct Spread
		{
			float Total = 0.0f;
			float Principal = 0.0f;
		};

		// The scatter matrix of a set of texels in C of their channels: sum of (x - mean)(x - mean)^T.
		template<uint32_t C>
		void GetScatter(const TexelSums& sums, const uint32_t (&channels)[C], float (&outScatter)[C][C])
		{
			const float inverseCount = c_InverseCounts.Values[std::min(static_cast<uint32_t>(sums.Lanes[TexelSums::c_CountLane]), 16u)];
			for (uint32_t row = 0; row < C; row++)
			{
				const float rowSum = sums.Lanes[TexelSums::c_SumLanes + channels[row]];
				for (uint32_t column = row; column < C; column++)
				{
					const float product = sums.Lanes[TexelSums::c_ProductLanes + TexelSums::c_ProductLane[channels[row]][channels[column]]];
					outScatter[row][column] = product - rowSum * sums.Lanes[TexelSums::c_SumLanes + channels[column]] * inverseCount;
					outScatter[column][row] = outScatter[row][column];
				}
			}
		}

		// The spread of a set of texels, with the largest eigenvalue of its scatter matrix estimated cheaply for ranking
		// candidates: |S e|^2 / (e^T S e) for the axis e of the largest diagonal entry. Written in S's eigenvalues l and e's
		// components a along their axes, that is the mean of the eigenvalues weighted by l a^2: never above the largest,
		// and exact when the texels lie on a line or spread alike in every direction.
		template<uint32_t C>
		Spread MeasureSpread(const TexelSums& sums, const uint32_t (&channels)[C])
		{
			Spread spread;
			if (sums.Lanes[TexelSums::c_CountLane] < 0.5f)
				return spread;
			float scatter[C][C];
			GetScatter(sums, channels, scatter);
			uint32_t largest = 0;
			for (uint32_t row = 0; row < C; row++)
			{
				spread.Total += scatter[row][row];
				if (scatter[row][row] > scatter[largest][largest])
					largest = row;
			}
			if (scatter[largest][largest] <= 1e-3f)
				return spread;
			float columnLengthSquared = 0.0f;
			for (uint32_t row = 0; row < C; row++)
				columnLengthSquared += scatter[row][largest] * scatter[row][largest];
			spread.Principal = std::max(std::min(columnLengthSquared / scatter[largest][largest], spread.Total), 0.0f);
			return spread;
		}

		// MeasureSpread of the color channels (red, green and blue), written out: the partition estimate measures three
		// subsets of 64 partitions and two of another 64 per block.
		Spread MeasureColorSpread(const TexelSums& sums)
		{
			Spread spread;
			const uint32_t count = static_cast<uint32_t>(sums.Lanes[TexelSums::c_CountLane]);
			if (count == 0)
				return spread;
			const float inverseCount = c_InverseCounts.Values[std::min(count, 16u)];
			const float* lanes = sums.Lanes;
			const float red = lanes[TexelSums::c_SumLanes];
			const float green = lanes[TexelSums::c_SumLanes + 1];
			const float blue = lanes[TexelSums::c_SumLanes + 2];
			const float redRed = lanes[TexelSums::c_ProductLanes + TexelSums::c_ProductLane[0][0]] - red * red * inverseCount;
			const float redGreen = lanes[TexelSums::c_ProductLanes + TexelSums::c_ProductLane[0][1]] - red * green * inverseCount;
			const float redBlue = lanes[TexelSums::c_ProductLanes + TexelSums::c_ProductLane[0][2]] - red * blue * inverseCount;
			const float greenGreen = lanes[TexelSums::c_ProductLanes + TexelSums::c_ProductLane[1][1]] - green * green * inverseCount;
			const float greenBlue = lanes[TexelSums::c_ProductLanes + TexelSums::c_ProductLane[1][2]] - green * blue * inverseCount;
			const float blueBlue = lanes[TexelSums::c_ProductLanes + TexelSums::c_ProductLane[2][2]] - blue * blue * inverseCount;
			spread.Total = redRed + greenGreen + blueBlue;

			float diagonal = redRed;
			float columnLengthSquared = redRed * redRed + redGreen * redGreen + redBlue * redBlue;
			if (greenGreen > diagonal)
			{
				diagonal = greenGreen;
				columnLengthSquared = redGreen * redGreen + greenGreen * greenGreen + greenBlue * greenBlue;
			}
			if (blueBlue > diagonal)
			{
				diagonal = blueBlue;
				columnLengthSquared = redBlue * redBlue + greenBlue * greenBlue + blueBlue * blueBlue;
			}
			if (diagonal <= 1e-3f)
				return spread;
			spread.Principal = std::max(std::min(columnLengthSquared / diagonal, spread.Total), 0.0f);
			return spread;
		}

		// What quantizing evenly spread texels to the levels of `indexBits`-bit indices leaves of their variance: a uniform
		// spread of variance v between n evenly spaced levels leaves v / (n - 1)^2.
		constexpr float GetQuantizationShare(uint32_t indexBits)
		{
			const float levels = static_cast<float>((1u << indexBits) - 1);
			return 1.0f / (levels * levels);
		}

		// The estimated error of fitting a line with `indexBits`-bit indices to a set of texels: what the principal axis
		// does not explain, plus the quantization along it.
		float EstimateLineError(const Spread& spread, uint32_t indexBits)
		{
			return spread.Total - spread.Principal + spread.Principal * GetQuantizationShare(indexBits);
		}

		// The texels of each subset of each three-subset partition, as bit masks.
		struct ThreeSubsetMasks
		{
			uint16_t Masks[64][3] = {};
		};

		constexpr ThreeSubsetMasks BuildThreeSubsetMasks()
		{
			ThreeSubsetMasks masks;
			for (uint32_t partition = 0; partition < 64; partition++)
			{
				for (uint32_t texel = 0; texel < 16; texel++)
				{
					uint16_t& mask = masks.Masks[partition][BC7::c_Partitions3[partition][texel]];
					mask = static_cast<uint16_t>(mask | (1u << texel));
				}
			}
			return masks;
		}

		constexpr ThreeSubsetMasks c_ThreeSubsetMasks = BuildThreeSubsetMasks();

		// Keeps the `capacity` partitions of lowest estimate, ties to the lower partition number.
		struct PartitionRanking
		{
			static constexpr uint32_t c_Capacity = 4;
			uint32_t Count = 0;
			uint32_t Capacity = 1;
			uint32_t Partitions[c_Capacity] = {};
			float Estimates[c_Capacity] = {};

			void Add(uint32_t partition, float estimate)
			{
				uint32_t position = Count;
				while (position > 0 && estimate < Estimates[position - 1])
					position--;
				if (position >= Capacity)
					return;
				const uint32_t last = std::min(Count, Capacity - 1);
				for (uint32_t slot = last; slot > position; slot--)
				{
					Partitions[slot] = Partitions[slot - 1];
					Estimates[slot] = Estimates[slot - 1];
				}
				Partitions[position] = partition;
				Estimates[position] = estimate;
				Count = std::min(Count + 1, Capacity);
			}
		};

		struct SearchSettings
		{
			uint32_t Modes = c_AllModes;
			FitEffort Screening;
			// Whether the best screened candidates, and mode 6's, are fitted again with the polishing effort.
			bool Polish = false;
			FitEffort Polishing;
			uint32_t TwoSubsetCandidates = 1;   // Partitions screened per mode with two subsets, the best by the estimate
			uint32_t ThreeSubsetCandidates = 1; // The same for mode 2 (three subsets)
			uint32_t ModeZeroCandidates = 1;    // And for mode 0
		};

		// The search over a block's candidate encodings. Every candidate is screened: fitted with the screening effort, giving
		// up once it cannot be among the best c_PolishedCandidates. Polishing then fits those again with more effort, and
		// mode 6 too, so the search never ends worse than mode 6 alone would.
		class Search
		{
		public:
			static constexpr uint32_t c_PolishedCandidates = 2;

			Search(const SourceBlock& block, const SearchSettings& settings)
				: m_Block(block), m_Settings(settings)
			{
				for (uint32_t texel = 0; texel < 16; texel++)
				{
					m_Terms[texel] = TexelSums::FromTexel(block.Texels[texel]);
					m_Total = m_Total + m_Terms[texel];
				}
			}

			// Runs the search, cheapest modes first so that later candidates can give up early, and keeps the best encoding
			// in `best` (ties to the one already there).
			void Run(BlockEncoding& best)
			{
				if (IsAllowed(6))
					Screen({ 6, 0, 0, 0 });
				if (IsAllowed(4) || IsAllowed(5))
					SearchRotations();
				if ((IsAllowed(1) || IsAllowed(3) || IsAllowed(7)) && !IsNegligible())
					SearchTwoSubsets();
				if ((IsAllowed(0) || IsAllowed(2)) && !IsNegligible())
					SearchThreeSubsets();

				if (!m_Settings.Polish)
				{
					Consider(m_Screened[0], best);
					return;
				}
				bool polishedMode6 = false;
				for (uint32_t candidate = 0; candidate < m_ScreenedCount; candidate++)
				{
					const BlockEncoding& screened = m_Screened[candidate];
					const CandidateShape shape { screened.Mode, screened.Partition, screened.Rotation, screened.IndexSelection };
					Consider(EncodeCandidate(m_Block, shape, m_Settings.Polishing, c_NoEncoding), best);
					polishedMode6 = polishedMode6 || screened.Mode == 6;
				}
				if (IsAllowed(6) && !polishedMode6)
					Consider(EncodeCandidate(m_Block, { 6, 0, 0, 0 }, m_Settings.Polishing, c_NoEncoding), best);
			}
		private:
			bool IsAllowed(uint32_t mode) const { return ((m_Settings.Modes >> mode) & 1u) != 0; }

			static void Consider(const BlockEncoding& candidate, BlockEncoding& best)
			{
				if (candidate.Error < best.Error)
					best = candidate;
			}

			// Whether the best candidate so far errs too little for more subsets to make a visible difference.
			bool IsNegligible() const
			{
				return m_ScreenedCount > 0 && m_Screened[0].Error <= c_NegligibleError;
			}

			// The error a screened candidate must stay below to be among the best.
			uint32_t GetBound() const
			{
				return m_ScreenedCount < c_PolishedCandidates ? c_NoEncoding : m_Screened[c_PolishedCandidates - 1].Error;
			}

			// Fits a candidate with the screening effort and keeps it if it is among the best (ties to the earlier ones).
			void Screen(const CandidateShape& shape)
			{
				const BlockEncoding encoding = EncodeCandidate(m_Block, shape, m_Settings.Screening, GetBound());
				if (encoding.Error >= GetBound())
					return;
				uint32_t position = std::min(m_ScreenedCount, c_PolishedCandidates - 1);
				while (position > 0 && encoding.Error < m_Screened[position - 1].Error)
				{
					m_Screened[position] = m_Screened[position - 1];
					position--;
				}
				m_Screened[position] = encoding;
				m_ScreenedCount = std::min(m_ScreenedCount + 1, c_PolishedCandidates);
			}

			// Modes 4 and 5: the rotation whose estimate is best (color as a line plus alpha's quantization) for mode 5 and
			// for each index selection of mode 4.
			void SearchRotations()
			{
				Spread colorSpreads[4];
				float alphaSpreads[4];
				for (uint32_t rotation = 0; rotation < 4; rotation++)
				{
					uint32_t colorChannels[3] = { 0, 1, 2 };
					const uint32_t alphaChannel = rotation == 0 ? 3 : rotation - 1;
					if (rotation > 0)
						colorChannels[rotation - 1] = 3;
					colorSpreads[rotation] = MeasureSpread(m_Total, colorChannels);
					alphaSpreads[rotation] = MeasureSpread(m_Total, { alphaChannel }).Total;
				}

				for (const auto& [modeIndex, indexSelection] : { std::pair { 5u, 0u }, std::pair { 4u, 0u }, std::pair { 4u, 1u } })
				{
					if (!IsAllowed(modeIndex))
						continue;
					BlockEncoding layout;
					layout.Mode = modeIndex;
					layout.IndexSelection = indexSelection;
					const uint32_t colorIndexBits = GetColorIndexBits(layout);
					const float alphaQuantization = GetQuantizationShare(GetAlphaIndexBits(layout));
					uint32_t bestRotation = 0;
					float bestEstimate = std::numeric_limits<float>::max();
					for (uint32_t rotation = 0; rotation < 4; rotation++)
					{
						const float estimate = EstimateLineError(colorSpreads[rotation], colorIndexBits) + alphaSpreads[rotation] * alphaQuantization;
						if (estimate < bestEstimate)
						{
							bestEstimate = estimate;
							bestRotation = rotation;
						}
					}
					Screen({ modeIndex, 0, bestRotation, indexSelection });
				}
			}

			// Modes 1, 3 and 7: every partition's estimate, then the best few screened.
			void SearchTwoSubsets()
			{
				// Mode 7 stores the same partitions as mode 3 with less precise endpoints; in an opaque block (alpha 255, so
				// p-bits of 1) every encoding of it has an encoding in mode 3 that decodes the same.
				const bool tryMode7 = IsAllowed(7) && !(m_Block.Opaque && IsAllowed(3));
				// Modes without alpha err by the block's alpha alone.
				const bool tryColorModes = (IsAllowed(1) || IsAllowed(3)) && m_Block.AlphaError < GetBound();
				if (!tryMode7 && !tryColorModes)
					return;
				PrepareRowSums();

				PartitionRanking mode1;
				PartitionRanking mode3;
				PartitionRanking mode7;
				mode1.Capacity = mode3.Capacity = mode7.Capacity = std::min(m_Settings.TwoSubsetCandidates, PartitionRanking::c_Capacity);
				for (uint32_t partition = 0; partition < 64; partition++)
				{
					const TexelSums second = SumTexels(BC7::c_Partitions2[partition]);
					const TexelSums first = m_Total - second;
					if (tryColorModes)
					{
						const Spread firstSpread = MeasureColorSpread(first);
						const Spread secondSpread = MeasureColorSpread(second);
						mode1.Add(partition, EstimateLineError(firstSpread, GetMode(1).IndexBits) + EstimateLineError(secondSpread, GetMode(1).IndexBits));
						mode3.Add(partition, EstimateLineError(firstSpread, GetMode(3).IndexBits) + EstimateLineError(secondSpread, GetMode(3).IndexBits));
					}
					if (tryMode7)
					{
						const Spread firstSpread = MeasureSpread(first, { 0u, 1u, 2u, 3u });
						const Spread secondSpread = MeasureSpread(second, { 0u, 1u, 2u, 3u });
						mode7.Add(partition, EstimateLineError(firstSpread, GetMode(7).IndexBits) + EstimateLineError(secondSpread, GetMode(7).IndexBits));
					}
				}

				if (tryColorModes && IsAllowed(3))
					ScreenRanked(3, mode3);
				if (tryColorModes && IsAllowed(1))
					ScreenRanked(1, mode1);
				if (tryMode7)
					ScreenRanked(7, mode7);
			}

			// Modes 0 (the first 16 partitions) and 2: every partition's estimate, then the best few screened.
			void SearchThreeSubsets()
			{
				if (m_Block.AlphaError >= GetBound())
					return;
				PrepareRowSums();
				PartitionRanking mode0;
				PartitionRanking mode2;
				mode2.Capacity = std::min(m_Settings.ThreeSubsetCandidates, PartitionRanking::c_Capacity);
				mode0.Capacity = std::min(m_Settings.ModeZeroCandidates, PartitionRanking::c_Capacity);
				const uint32_t partitions = IsAllowed(2) ? 64 : 16;
				for (uint32_t partition = 0; partition < partitions; partition++)
				{
					TexelSums subsets[3];
					subsets[1] = SumTexels(c_ThreeSubsetMasks.Masks[partition][1]);
					subsets[2] = SumTexels(c_ThreeSubsetMasks.Masks[partition][2]);
					subsets[0] = m_Total - subsets[1] - subsets[2];
					float estimate0 = 0.0f;
					float estimate2 = 0.0f;
					for (const TexelSums& sums : subsets)
					{
						const Spread spread = MeasureColorSpread(sums);
						estimate0 += EstimateLineError(spread, GetMode(0).IndexBits);
						estimate2 += EstimateLineError(spread, GetMode(2).IndexBits);
					}
					if (partition < 16)
						mode0.Add(partition, estimate0);
					mode2.Add(partition, estimate2);
				}

				if (IsAllowed(2))
					ScreenRanked(2, mode2);
				if (IsAllowed(0))
					ScreenRanked(0, mode0);
			}

			// The sums of every subset of every row's texels (m_RowSums), each from a smaller one and a texel, once.
			void PrepareRowSums()
			{
				if (m_HasRowSums)
					return;
				for (uint32_t row = 0; row < 4; row++)
				{
					for (uint32_t mask = 1; mask < 16; mask++)
						m_RowSums[row][mask] = m_RowSums[row][mask & (mask - 1)] + m_Terms[row * 4 + static_cast<uint32_t>(std::countr_zero(mask))];
				}
				m_HasRowSums = true;
			}

			// The sums of the texels in a mask (bit i: texel i), from the sums of its rows (PrepareRowSums), in one loop over the
			// lanes.
			TexelSums SumTexels(uint32_t mask) const
			{
				const float* first = m_RowSums[0][mask & 15].Lanes;
				const float* second = m_RowSums[1][(mask >> 4) & 15].Lanes;
				const float* third = m_RowSums[2][(mask >> 8) & 15].Lanes;
				const float* fourth = m_RowSums[3][(mask >> 12) & 15].Lanes;
				TexelSums sums;
				for (uint32_t lane = 0; lane < 16; lane++)
					sums.Lanes[lane] = (first[lane] + second[lane]) + (third[lane] + fourth[lane]);
				return sums;
			}

			void ScreenRanked(uint32_t modeIndex, const PartitionRanking& ranking)
			{
				for (uint32_t candidate = 0; candidate < ranking.Count; candidate++)
					Screen({ modeIndex, ranking.Partitions[candidate], 0, 0 });
			}

			const SourceBlock& m_Block;
			SearchSettings m_Settings;
			TexelSums m_Terms[16];      // What each texel adds to sums
			TexelSums m_Total;
			TexelSums m_RowSums[4][16]; // [row][mask]: the sums of the texels of a row in the mask (bit i: texel row * 4 + i)
			bool m_HasRowSums = false;
			BlockEncoding m_Screened[c_PolishedCandidates]; // The best screened candidates, least error first
			uint32_t m_ScreenedCount = 0;
		};

		// Swaps the endpoints of every subset whose anchor texel's index has its top bit set (the format stores anchor
		// indices with one bit less) and mirrors its indices. Weights i and n - 1 - i add up to 64, so the decoded texels
		// stay the same.
		void FixAnchors(BlockEncoding& encoding)
		{
			const BC7::ModeInfo& mode = GetMode(encoding.Mode);
			auto mirror = [](uint8_t* indices, uint32_t bits, uint32_t subsets, uint32_t partition, uint32_t subset)
			{
				for (uint32_t texel = 0; texel < 16; texel++)
				{
					if (BC7::GetSubset(subsets, partition, texel) == subset)
						indices[texel] = static_cast<uint8_t>((1u << bits) - 1 - indices[texel]);
				}
			};

			if (mode.SecondaryIndexBits > 0)
			{
				// Modes 4 and 5: one subset whose color and alpha have separate indices; texel 0 anchors both.
				const uint32_t colorBits = GetColorIndexBits(encoding);
				if (encoding.ColorIndices[0] >> (colorBits - 1))
				{
					for (uint32_t channel = 0; channel < 3; channel++)
						std::swap(encoding.Raw[0][channel], encoding.Raw[1][channel]);
					mirror(encoding.ColorIndices, colorBits, 1, 0, 0);
				}
				const uint32_t alphaBits = GetAlphaIndexBits(encoding);
				if (encoding.AlphaIndices[0] >> (alphaBits - 1))
				{
					std::swap(encoding.Raw[0][3], encoding.Raw[1][3]);
					mirror(encoding.AlphaIndices, alphaBits, 1, 0, 0);
				}
				return;
			}

			for (uint32_t subset = 0; subset < mode.Subsets; subset++)
			{
				const uint32_t anchor = BC7::GetAnchorTexel(mode.Subsets, encoding.Partition, subset);
				if ((encoding.ColorIndices[anchor] >> (mode.IndexBits - 1)) == 0)
					continue;
				std::swap(encoding.Raw[subset * 2], encoding.Raw[subset * 2 + 1]);
				std::swap(encoding.PBits[subset * 2], encoding.PBits[subset * 2 + 1]);
				mirror(encoding.ColorIndices, mode.IndexBits, mode.Subsets, encoding.Partition, subset);
			}
		}

		// The texels an encoding decodes to, computed from its fields as the format defines.
		void DecodeEncoding(const BlockEncoding& encoding, uint8_t* rgba)
		{
			const BC7::ModeInfo& mode = GetMode(encoding.Mode);
			const bool hasPBits = mode.EndpointPBits || mode.SharedPBits;
			uint32_t endpoints[6][4] = {};
			for (uint32_t endpoint = 0; endpoint < mode.Subsets * 2u; endpoint++)
			{
				for (uint32_t channel = 0; channel < 4; channel++)
				{
					const uint32_t bits = channel < 3 ? mode.ColorBits : mode.AlphaBits;
					endpoints[endpoint][channel] = bits == 0 ? 255u : Expand(encoding.Raw[endpoint][channel], bits, hasPBits, encoding.PBits[endpoint]);
				}
			}

			const bool separateAlpha = mode.SecondaryIndexBits > 0;
			const uint8_t* colorWeights = BC7::GetWeights(separateAlpha ? GetColorIndexBits(encoding) : mode.IndexBits);
			const uint8_t* alphaWeights = separateAlpha ? BC7::GetWeights(GetAlphaIndexBits(encoding)) : colorWeights;
			for (uint32_t texel = 0; texel < 16; texel++)
			{
				const uint32_t subset = BC7::GetSubset(mode.Subsets, encoding.Partition, texel);
				const uint32_t* first = endpoints[subset * 2];
				const uint32_t* second = endpoints[subset * 2 + 1];
				const uint32_t colorWeight = colorWeights[encoding.ColorIndices[texel]];
				const uint32_t alphaWeight = alphaWeights[separateAlpha ? encoding.AlphaIndices[texel] : encoding.ColorIndices[texel]];
				uint8_t* destination = rgba + texel * 4;
				for (uint32_t channel = 0; channel < 3; channel++)
					destination[channel] = BC7::Interpolate(first[channel], second[channel], colorWeight);
				destination[3] = BC7::Interpolate(first[3], second[3], alphaWeight);
				if (encoding.Rotation > 0)
					std::swap(destination[3], destination[encoding.Rotation - 1]);
			}
		}

		// Writes the fields of a 128-bit block, least significant bit first.
		class BlockBitWriter
		{
		public:
			// Appends the low `bits` bits (at most 32) of value.
			void Write(uint32_t value, uint32_t bits)
			{
				if (bits == 0)
					return;
				const uint64_t field = static_cast<uint64_t>(value) & ((1ull << bits) - 1);
				if (m_Position < 64)
				{
					m_Low |= field << m_Position;
					if (m_Position + bits > 64)
						m_High |= field >> (64 - m_Position);
				}
				else
				{
					m_High |= field << (m_Position - 64);
				}
				m_Position += bits;
			}

			uint32_t GetPosition() const { return m_Position; }

			void CopyTo(uint8_t* outBytes) const
			{
				for (uint32_t byte = 0; byte < 8; byte++)
				{
					outBytes[byte] = static_cast<uint8_t>(m_Low >> (byte * 8));
					outBytes[byte + 8] = static_cast<uint8_t>(m_High >> (byte * 8));
				}
			}
		private:
			uint64_t m_Low = 0;
			uint64_t m_High = 0;
			uint32_t m_Position = 0;
		};

		// Packs an encoding whose anchors are fixed (FixAnchors) into the block's bits.
		void Pack(const BlockEncoding& encoding, uint8_t* outBlock)
		{
			const BC7::ModeInfo& mode = GetMode(encoding.Mode);
			BlockBitWriter writer;
			writer.Write(1u << encoding.Mode, encoding.Mode + 1);
			writer.Write(encoding.Partition, mode.PartitionBits);
			writer.Write(encoding.Rotation, mode.RotationBits);
			writer.Write(encoding.IndexSelection, mode.IndexSelectionBits);

			const uint32_t endpointCount = mode.Subsets * 2u;
			for (uint32_t channel = 0; channel < 3; channel++)
			{
				for (uint32_t endpoint = 0; endpoint < endpointCount; endpoint++)
					writer.Write(encoding.Raw[endpoint][channel], mode.ColorBits);
			}
			for (uint32_t endpoint = 0; mode.AlphaBits > 0 && endpoint < endpointCount; endpoint++)
				writer.Write(encoding.Raw[endpoint][3], mode.AlphaBits);
			if (mode.EndpointPBits)
			{
				for (uint32_t endpoint = 0; endpoint < endpointCount; endpoint++)
					writer.Write(encoding.PBits[endpoint], 1);
			}
			else if (mode.SharedPBits)
			{
				for (uint32_t subset = 0; subset < mode.Subsets; subset++)
					writer.Write(encoding.PBits[subset * 2], 1);
			}

			// Mode 4's index selection gives the primary (two-bit) indices to alpha and the secondary ones to color.
			const bool alphaFirst = encoding.Mode == 4 && encoding.IndexSelection == 1;
			const uint8_t* primary = alphaFirst ? encoding.AlphaIndices : encoding.ColorIndices;
			const uint8_t* secondary = alphaFirst ? encoding.ColorIndices : encoding.AlphaIndices;
			for (uint32_t texel = 0; texel < 16; texel++)
			{
				bool anchor = false;
				for (uint32_t subset = 0; subset < mode.Subsets; subset++)
					anchor = anchor || texel == BC7::GetAnchorTexel(mode.Subsets, encoding.Partition, subset);
				writer.Write(primary[texel], mode.IndexBits - (anchor ? 1u : 0u));
			}
			for (uint32_t texel = 0; mode.SecondaryIndexBits > 0 && texel < 16; texel++)
				writer.Write(secondary[texel], mode.SecondaryIndexBits - (texel == 0 ? 1u : 0u));
			ST_CORE_ASSERT(writer.GetPosition() == 128, "a BC7 block of mode {} has {} bits", encoding.Mode, writer.GetPosition());
			writer.CopyTo(outBlock);
		}

	}

	uint32_t EncodeBlock(const uint8_t* rgba, uint8_t* outBlock, const EncodeSettings& settings, BlockInfo* outInfo)
	{
		uint32_t modes = settings.AllowedModes & c_AllModes;
		ST_CORE_ASSERT(modes != 0, "BC7 encoding needs at least one allowed mode");
		if (modes == 0)
			modes = c_AllModes;

		const SourceBlock block = LoadBlock(rgba);
		BlockEncoding best;
		if (block.Flat && ((modes >> 5) & 1u))
			best = EncodeFlat(block);
		if (best.Error > 0)
		{
			SearchSettings search;
			if (settings.HighQuality)
			{
				search.Modes = modes;
				search.Screening = { c_ScreeningRefinements, false };
				search.Polish = true;
				search.Polishing = { c_PolishingRefinements, true };
				search.TwoSubsetCandidates = c_TwoSubsetCandidates;
				search.ThreeSubsetCandidates = c_ThreeSubsetCandidates;
				search.ModeZeroCandidates = c_ModeZeroCandidates;
			}
			else
			{
				const uint32_t fastModes = modes & ((1u << 6) | (block.AlphaVaries ? (1u << 5) : 0u));
				search.Modes = fastModes != 0 ? fastModes : modes;
				search.Screening = { c_FastRefinements, false };
			}
			Search(block, search).Run(best);
		}

		FixAnchors(best);
		uint8_t decoded[16 * 4];
		DecodeEncoding(best, decoded);
		uint32_t error = 0;
		for (uint32_t texel = 0; texel < 16; texel++)
		{
			for (uint32_t channel = 0; channel < 4; channel++)
			{
				const int32_t difference = static_cast<int32_t>(decoded[texel * 4 + channel]) - block.Texels[texel][channel];
				error += static_cast<uint32_t>(difference * difference);
			}
		}
		ST_CORE_ASSERT(error == best.Error, "the BC7 encoder expected an error of {} in mode {}, the block decodes with {}", best.Error, best.Mode, error);
		Pack(best, outBlock);

		if (outInfo)
		{
			outInfo->Mode = best.Mode;
			outInfo->Partition = best.Partition;
			outInfo->Rotation = best.Rotation;
			outInfo->IndexSelection = best.IndexSelection;
			outInfo->Error = error;
			std::memcpy(outInfo->Decoded, decoded, sizeof(decoded));
		}
		return error;
	}

}
