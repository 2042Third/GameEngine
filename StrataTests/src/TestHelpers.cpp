#include "TestHelpers.h"

#include "Strata/Core/UUID.h"

#include <stb_image_write.h>

#include <cmath>
#include <mutex>
#include <numbers>
#include <vector>

namespace Strata::Tests
{

	static std::mutex s_TemporaryDirectoriesMutex;
	static std::vector<std::filesystem::path> s_TemporaryDirectories;

	std::filesystem::path CreateTemporaryDirectory(const std::string& name)
	{
		std::filesystem::path directory = std::filesystem::temp_directory_path() / "StrataTests" / (name + "_" + UUID().ToString());
		std::error_code error;
		std::filesystem::remove_all(directory, error);
		std::filesystem::create_directories(directory, error);

		std::scoped_lock<std::mutex> lock(s_TemporaryDirectoriesMutex);
		s_TemporaryDirectories.push_back(directory);
		return directory;
	}

	std::vector<uint8_t> CreateSineWav(float durationSeconds, uint32_t sampleRate, uint16_t channels, float frequency, float silentSeconds, float amplitude)
	{
		const uint32_t silentFrames = static_cast<uint32_t>(std::lround(silentSeconds * static_cast<float>(sampleRate)));
		const uint32_t frameCount = static_cast<uint32_t>(std::lround(durationSeconds * static_cast<float>(sampleRate)));
		const uint32_t blockAlign = channels * 2u;
		const uint32_t dataSize = frameCount * blockAlign;

		std::vector<uint8_t> wav;
		wav.reserve(44 + dataSize);
		const auto writeTag = [&](const char* tag) { wav.insert(wav.end(), tag, tag + 4); };
		const auto writeU16 = [&](uint16_t value)
		{
			wav.push_back(static_cast<uint8_t>(value & 0xFF));
			wav.push_back(static_cast<uint8_t>(value >> 8));
		};
		const auto writeU32 = [&](uint32_t value)
		{
			for (uint32_t byte = 0; byte < 4; byte++)
				wav.push_back(static_cast<uint8_t>(value >> (byte * 8)));
		};

		writeTag("RIFF");
		writeU32(36 + dataSize);
		writeTag("WAVE");

		writeTag("fmt ");
		writeU32(16);
		writeU16(1); // PCM
		writeU16(channels);
		writeU32(sampleRate);
		writeU32(sampleRate * blockAlign);
		writeU16(static_cast<uint16_t>(blockAlign));
		writeU16(16); // Bits per sample

		writeTag("data");
		writeU32(dataSize);
		for (uint32_t frame = 0; frame < frameCount; frame++)
		{
			const double phase = 2.0 * std::numbers::pi * frequency * static_cast<double>(frame) / static_cast<double>(sampleRate);
			const int16_t sample = frame < silentFrames ? int16_t(0) : static_cast<int16_t>(std::lround(amplitude * std::sin(phase) * 32767.0));
			for (uint16_t channel = 0; channel < channels; channel++)
				writeU16(static_cast<uint16_t>(sample));
		}
		return wav;
	}

	std::vector<uint8_t> EncodePNG(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba)
	{
		std::vector<uint8_t> encoded;
		auto write = [](void* context, void* data, int size)
		{
			auto* output = static_cast<std::vector<uint8_t>*>(context);
			output->insert(output->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
		};
		if (rgba.size() < static_cast<size_t>(width) * height * 4
			|| !stbi_write_png_to_func(write, &encoded, static_cast<int>(width), static_cast<int>(height), 4, rgba.data(), static_cast<int>(width * 4)))
			return {};
		return encoded;
	}

	std::vector<uint8_t> CreateSolidPNG(uint32_t width, uint32_t height, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)
	{
		std::vector<uint8_t> pixels;
		pixels.reserve(static_cast<size_t>(width) * height * 4);
		for (uint32_t index = 0; index < width * height; index++)
			pixels.insert(pixels.end(), { red, green, blue, alpha });
		return EncodePNG(width, height, pixels);
	}

	void CleanupTemporaryDirectories()
	{
		std::scoped_lock<std::mutex> lock(s_TemporaryDirectoriesMutex);
		for (const std::filesystem::path& directory : s_TemporaryDirectories)
		{
			std::error_code error;
			std::filesystem::remove_all(directory, error);
		}
		s_TemporaryDirectories.clear();
	}

}
