#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Scene/EntityPrototype.h"
#include "Strata/Scene/SceneDocumentWriter.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class Scene;

	// The binary form of scene, prefab and model documents that exports put into packs ("cooked scenes"): what a load reads
	// without parsing JSON. Little endian:
	//
	// - a header of 64 bytes (CookedSceneFormat::Header): the magic "STSC", the version, flags (none defined: a file with
	//   any is refused), the header's size, the document's kind, the counts of entities, component types and strings, the
	//   strings' bytes, where the section table is and how many sections it has, a CRC-32C of the table and one of the
	//   header (computed with its own field zero);
	// - sections, each 16-byte aligned, inside the file, overlapping none, each with a CRC-32C in the table:
	//   - Strings: (count + 1) u32 offsets into the bytes that follow, then the bytes (the names of the component types and
	//     their properties);
	//   - SceneInfo (scenes only): {"Name", "Settings"} as compact JSON, the settings read by the JSON reader's rules;
	//   - EntityIDs: a u64 per entity, none null, none twice; Parents: a u32 per entity, 0xFFFFFFFF for the top level, else
	//     below the entity's own index (parents first: the prototype's hierarchy order);
	//   - ComponentTypes: per type its name (a string index), its properties (name and PropertyType each) and the kind of
	//     its extra data (none, binary, MessagePack of its JSON extra); a reader of another build maps them by name;
	//   - Column (one per type, Index = the type): a dense flag (every entity has it) or the entities' indices, strictly
	//     increasing; then each property's values together (PropertyBinary), then each value's extra data (a u32 size and
	//     its bytes);
	//   - UnknownComponents: per entry an entity index, a size and the MessagePack of the components this build did not
	//     register when it cooked them.
	//
	// Entity references are not stored: a reader computes them from the values, as EntityPrototype::Compile does.
	// Everything is validated before it is used: checksums before decoding, every count and index against the sizes that
	// hold it before anything is allocated.
	namespace CookedSceneFormat
	{

		inline constexpr char c_Magic[4] = { 'S', 'T', 'S', 'C' };
		inline constexpr uint32_t c_Version = 1;
		inline constexpr uint32_t c_SectionAlignment = 16;
		inline constexpr uint32_t c_NoParent = 0xFFFFFFFFu;
		// The largest MessagePack blob of an unknown component's or a JSON extra's data.
		inline constexpr uint32_t c_MaxBlobBytes = 16u << 20;
		// Column flags.
		inline constexpr uint32_t c_ColumnDense = 1u;

		enum class SectionKind : uint32_t
		{
			Strings = 1,
			SceneInfo,
			EntityIDs,
			Parents,
			ComponentTypes,
			Column,
			UnknownComponents
		};

		enum class ExtraKind : uint32_t
		{
			None = 0,
			Binary,     // ComponentInfo::SerializeExtraBinary
			MessagePack // The JSON extra (ComponentInfo::SerializeExtra) of a type without a binary form
		};

		struct Header
		{
			char Magic[4];
			uint32_t Version;
			uint32_t Flags;
			uint32_t HeaderSize;
			uint32_t Kind; // SceneDocumentKind
			uint32_t EntityCount;
			uint32_t ComponentTypeCount;
			uint32_t StringCount;
			uint64_t StringBytes;
			uint64_t SectionTableOffset;
			uint32_t SectionCount;
			uint32_t SectionTableCrc;
			uint32_t HeaderCrc;
			uint32_t Reserved;
		};
		static_assert(sizeof(Header) == 64, "The cooked scene header is 64 bytes");

		struct Section
		{
			uint32_t Kind;
			uint32_t Index;
			uint64_t Offset;
			uint64_t Size;
			uint32_t Count;
			uint32_t Crc;
		};
		static_assert(sizeof(Section) == 32, "A cooked scene section entry is 32 bytes");

	}

	// What a cooked document holds: its kind, a scene's name and settings, and its entities as a prototype, with what the
	// reader could not read as written.
	struct CookedSceneContents
	{
		SceneDocumentKind Kind = SceneDocumentKind::Scene;
		std::string Name;          // Scenes
		nlohmann::json Settings;   // Scenes: SceneSerializer::SerializeSettings' object (null when the document has none)
		Ref<EntityPrototype> Prototype;
		std::vector<std::string> Warnings;
	};

	struct CookedSceneReadOptions
	{
		// Asked between sections and every 4,096 values: true ends the read, which fails as cancelled.
		std::function<bool()> IsCancelled;
		// The share of the sections decoded, in [0, 1].
		std::function<void(float share)> Progress;
	};

	class CookedScene
	{
	public:
		// Whether the bytes start as a cooked document (its magic): loaders take those to the binary reader, others to the
		// JSON reader.
		static bool IsCooked(std::span<const uint8_t> bytes);
	};

	class CookedSceneWriter
	{
	public:
		// Writes a prototype's entities as a cooked document of `kind`; scenes also hold `name` and `settings` (the object
		// SceneSerializer::SerializeSettings makes). The bytes depend on nothing but the input. False, with the reason, when a
		// value has no binary form (a string over c_MaxBinaryStringBytes, a blob over c_MaxBlobBytes).
		static bool Write(SceneDocumentKind kind, const EntityPrototype& prototype, std::string_view name, const nlohmann::json& settings,
			std::vector<uint8_t>& outBytes, std::string* outError = nullptr);
	};

	class CookedSceneReader
	{
	public:
		// Reads a cooked document into a prototype (columns decoded in parallel on the job system when it runs). nullopt,
		// with the reason, when the bytes are not a valid cooked document or the read is cancelled.
		static std::optional<CookedSceneContents> Read(std::span<const uint8_t> bytes, const CookedSceneReadOptions& options = {}, std::string* outError = nullptr);
		// Reads a cooked scene into a new scene with its name and settings, its entities keeping their IDs (the prototype
		// instantiated: Scene::Instantiate with InstantiationOptions::KeepIDs).
		static Ref<Scene> ReadScene(std::span<const uint8_t> bytes, const CookedSceneReadOptions& options = {}, std::vector<std::string>* outWarnings = nullptr,
			std::string* outError = nullptr);
		// The cooked documents read since the process started (Read, also through ReadScene), as
		// SceneSerializer::GetDeserializeCount counts JSON ones: tests check that a load reads its document once.
		static uint64_t GetReadCount();
	};

}
