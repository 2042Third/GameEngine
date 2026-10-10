#pragma once

#include <cstdint>

// The asset pipeline's BC7 encoder (TextureCooker stores BC7 and BC7SRGB levels with it), written from the format's
// definition in Renderer/BC7Format.h, which the decoder in Renderer/BlockCompression shares.
//
// Every candidate encoding is fitted the same way: per subset, the principal axis of its texels gives the first
// endpoints, which are quantized to the mode's precision (p-bits chosen by how near they bring the endpoints); each
// texel takes the index of the nearest interpolated color; then the endpoints can be refitted by least squares to that
// index assignment and quantized again, as long as that lowers the error, and every other combination of p-bits tried
// on the best of them. Anchor texels are fixed up by swapping a subset's endpoints (interpolation is symmetric, so the
// decoded texels do not change). The error is the sum of the squared differences of all four channels; the candidate
// with the least error is stored. Blocks whose texels are all the same are stored in mode 5, which reproduces any
// color exactly.
namespace Strata::BC7Encoder
{

	// Every mode: bit m allows mode m.
	constexpr uint32_t c_AllModes = 0xFF;

	struct EncodeSettings
	{
		// High searches every mode. It screens candidates (their first endpoints, quantized once): mode 6; modes 4 and 5
		// in the channel rotation their estimate prefers (mode 4 with both index selections); and the 64 partitions of
		// modes 1, 3 and 7, and of modes 2 and 0 (mode 0: the first 16), ranked by an estimate of their error (the part
		// of each subset off its principal axis plus quantization along it), the best two (mode 0: one) of each mode. The
		// two best candidates and mode 6's are then fitted in full (three refits, every p-bit combination), so a block
		// never ends worse than mode 6 alone gives. Once a candidate errs by at most one squared step per texel, the modes
		// with partitions are not tried. Fast fits mode 6, and mode 5 when the block's alpha varies, with one refit.
		bool HighQuality = true;
		// The modes the encoder may use (bit m: mode m; at least one), to measure or test a mode on its own. Fast uses the
		// modes it would use that this allows, or else all that this allows.
		uint32_t AllowedModes = c_AllModes;
	};

	// What EncodeBlock stored.
	struct BlockInfo
	{
		uint32_t Mode = 0;
		uint32_t Partition = 0;      // Modes with two or three subsets
		uint32_t Rotation = 0;       // Modes 4 and 5: the channel swapped with alpha after decoding (0: none)
		uint32_t IndexSelection = 0; // Mode 4: 1 when color has the three-bit indices
		uint32_t Error = 0;          // Sum of the squared differences of every channel of every texel, decoded against source
		uint8_t Decoded[16 * 4] = {}; // The texels the block decodes to: RGBA, row-major
	};

	// Encodes 4x4 RGBA texels (row-major, four bytes each) into a 16-byte BC7 block and returns its error
	// (BlockInfo::Error). Deterministic: the same texels and settings give the same block on every thread. When every
	// texel is opaque, every texel of the block decodes with alpha 255. Thread-safe.
	uint32_t EncodeBlock(const uint8_t* rgba, uint8_t* outBlock, const EncodeSettings& settings = {}, BlockInfo* outInfo = nullptr);

}
