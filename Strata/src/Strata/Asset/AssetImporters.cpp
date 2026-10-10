#include "stpch.h"
#include "Strata/Asset/AssetImporter.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/GltfImporter.h"
#include "Strata/Asset/TextureImporter.h"
#include "Strata/Audio/AudioClipAsset.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Renderer/Font.h"

namespace Strata
{

	namespace
	{

		bool ReadSource(const ImportContext& context, std::vector<uint8_t>& outData, std::string* outError)
		{
			std::optional<std::vector<uint8_t>> data = FileSystem::ReadBytes(context.SourcePath);
			if (!data)
			{
				if (outError)
					*outError = fmt::format("Could not read '{}'", FileSystem::ToUTF8(context.SourcePath));
				return false;
			}
			outData = std::move(*data);
			return true;
		}

		// Assets the engine writes itself (materials, prefabs, scenes) are stored as their JSON source. Importing only
		// validates the document with the type's loader.
		class NativeAssetImporter final : public AssetImporter
		{
		public:
			NativeAssetImporter(AssetType type, std::string_view extension)
				: m_Type(type), m_Extension(extension)
			{
			}

			AssetType GetType() const override { return m_Type; }
			std::vector<std::string> GetExtensions() const override { return { m_Extension }; }
			uint32_t GetVersion() const override { return 1; }
			bool StoresSourceDirectly() const override { return true; }

			bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override
			{
				if (!ReadSource(context, result.Data, outError))
					return false;

				const AssetLoadFunction* loader = AssetLoaderRegistry::Find(m_Type);
				if (!loader)
				{
					if (outError)
						*outError = fmt::format("No loader for asset type {}", AssetTypeToString(m_Type));
					return false;
				}

				AssetMetadata metadata;
				metadata.Handle = context.Handle;
				metadata.Type = m_Type;
				metadata.Path = FileSystem::ToUTF8(FileSystem::GetRelativePath(context.SourcePath, context.AssetDirectory));
				std::string error;
				AssetLoadData document { std::span<const uint8_t>(result.Data) }; // Validated, not taken over: it is the stored form
				if (!(*loader)(metadata, document, &error))
				{
					if (outError)
						*outError = error.empty() ? std::string("Invalid document") : error;
					return false;
				}
				return true;
			}
		private:
			AssetType m_Type;
			std::string m_Extension;
		};

		// WAV, MP3, FLAC and Ogg Vorbis. Settings: { "LoadMode": "Decompressed" | "Streamed" }.
		class AudioClipImporter final : public AssetImporter
		{
		public:
			// Larger files stream by default (music, ambience); smaller ones are decoded once (sound effects).
			static constexpr uint64_t c_StreamThreshold = 1024 * 1024;

			AssetType GetType() const override { return AssetType::AudioClip; }
			std::vector<std::string> GetExtensions() const override { return { ".wav", ".mp3", ".flac", ".ogg" }; }
			uint32_t GetVersion() const override { return 1; }

			nlohmann::json GetDefaultSettings(const std::filesystem::path& sourcePath) const override
			{
				const uint64_t size = FileSystem::GetFileSize(sourcePath).value_or(0);
				nlohmann::json settings = nlohmann::json::object();
				settings["LoadMode"] = size > c_StreamThreshold ? "Streamed" : "Decompressed";
				return settings;
			}

			bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override
			{
				std::vector<uint8_t> encoded;
				if (!ReadSource(context, encoded, outError))
					return false;

				AudioClipLoadMode mode = AudioClipLoadMode::Decompressed;
				const std::string modeName = JsonUtils::GetString(context.Settings, "LoadMode", "Decompressed");
				if (modeName == "Streamed")
					mode = AudioClipLoadMode::Streamed;
				else if (modeName != "Decompressed")
					result.Warnings.push_back(fmt::format("Unknown audio load mode '{}'; expected Decompressed or Streamed", modeName));

				// Opening the clip as streamed only parses the header, which validates the file without decoding it.
				const std::string name = FileSystem::ToUTF8(context.SourcePath.filename());
				if (!AudioClip::LoadFromMemory(encoded, name, AudioClipLoadMode::Streamed))
				{
					if (outError)
						*outError = fmt::format("{}: audio data could not be decoded (supported: WAV, MP3, FLAC, Ogg Vorbis)", name);
					return false;
				}

				result.Data = AudioClipAsset::Cook(encoded, mode);
				return true;
			}
		};

		// TrueType and OpenType fonts, stored as-is.
		class FontImporter final : public AssetImporter
		{
		public:
			AssetType GetType() const override { return AssetType::Font; }
			std::vector<std::string> GetExtensions() const override { return { ".ttf", ".otf" }; }
			uint32_t GetVersion() const override { return 1; }
			bool StoresSourceDirectly() const override { return true; }

			bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override
			{
				if (!ReadSource(context, result.Data, outError))
					return false;
				return Font::Create(result.Data, outError) != nullptr;
			}
		};

	}

	void CreateBuiltinAssetImporters(std::vector<Scope<AssetImporter>>& importers)
	{
		importers.push_back(CreateScope<NativeAssetImporter>(AssetType::Scene, GetNativeAssetExtension(AssetType::Scene)));
		importers.push_back(CreateScope<NativeAssetImporter>(AssetType::Prefab, GetNativeAssetExtension(AssetType::Prefab)));
		importers.push_back(CreateScope<NativeAssetImporter>(AssetType::Material, GetNativeAssetExtension(AssetType::Material)));
		importers.push_back(CreateScope<TextureImporter>());
		importers.push_back(CreateScope<GltfImporter>());
		importers.push_back(CreateScope<AudioClipImporter>());
		importers.push_back(CreateScope<FontImporter>());
	}

}
