#include "stpch.h"
#include "Strata/Scene/CookedScene.h"

#include "Strata/Core/BinaryStream.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyBinary.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Strata
{

	using namespace CookedSceneFormat;

	namespace
	{

		// Values a column decodes between two asks whether the read is cancelled.
		constexpr uint32_t c_CancelCheckValues = 4096;

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

		// The properties of a type a cooked document holds: those its JSON form holds (not transient), in registration order.
		std::vector<const PropertyInfo*> GetCookedProperties(const ComponentInfo& info)
		{
			std::vector<const PropertyInfo*> properties;
			for (const PropertyInfo& property : info.Properties)
			{
				if (!property.IsTransient())
					properties.push_back(&property);
			}
			return properties;
		}

		ExtraKind GetExtraKind(const ComponentInfo& info)
		{
			if (info.SerializeExtraBinary && info.DeserializeExtraBinary)
				return ExtraKind::Binary;
			if (info.SerializeExtra)
				return ExtraKind::MessagePack;
			return ExtraKind::None;
		}

		bool ToMessagePack(const nlohmann::json& json, std::vector<uint8_t>& outBytes, std::string* outError)
		{
			try
			{
				outBytes = nlohmann::json::to_msgpack(json);
			}
			catch (const std::exception& exception)
			{
				return Fail(outError, fmt::format("Data cannot be stored as MessagePack: {}", exception.what()));
			}
			if (outBytes.size() > c_MaxBlobBytes)
				return Fail(outError, fmt::format("Data of {} bytes is larger than the {} a cooked document holds per value", outBytes.size(), c_MaxBlobBytes));
			return true;
		}

		std::optional<nlohmann::json> FromMessagePack(std::span<const uint8_t> bytes)
		{
			nlohmann::json json = nlohmann::json::from_msgpack(bytes.begin(), bytes.end(), true, false);
			if (json.is_discarded())
				return std::nullopt;
			return json;
		}

		// The names a document holds, each once, in the order they were first added.
		class StringTable
		{
		public:
			uint32_t Add(const std::string& text)
			{
				auto [it, added] = m_Index.try_emplace(text, static_cast<uint32_t>(m_Strings.size()));
				if (added)
					m_Strings.push_back(text);
				return it->second;
			}

			uint32_t GetCount() const { return static_cast<uint32_t>(m_Strings.size()); }

			uint64_t GetBytes() const
			{
				uint64_t bytes = 0;
				for (const std::string& text : m_Strings)
					bytes += text.size();
				return bytes;
			}

			std::vector<uint8_t> Write() const
			{
				BinaryWriter writer;
				uint32_t offset = 0;
				writer.Write(offset);
				for (const std::string& text : m_Strings)
				{
					offset += static_cast<uint32_t>(text.size());
					writer.Write(offset);
				}
				for (const std::string& text : m_Strings)
					writer.WriteBytes(text.data(), text.size());
				return writer.TakeData();
			}
		private:
			std::vector<std::string> m_Strings;
			std::unordered_map<std::string, uint32_t> m_Index;
		};

		struct SectionData
		{
			SectionKind Kind = SectionKind::Strings;
			uint32_t Index = 0;
			uint32_t Count = 0;
			std::vector<uint8_t> Bytes;
		};

		void Align(std::vector<uint8_t>& bytes)
		{
			bytes.resize((bytes.size() + c_SectionAlignment - 1) / c_SectionAlignment * c_SectionAlignment, 0);
		}

		uint32_t ComputeHeaderCrc(Header header)
		{
			header.HeaderCrc = 0;
			return Hash::CRC32C(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&header), sizeof(header)));
		}

	}

	bool CookedScene::IsCooked(std::span<const uint8_t> bytes)
	{
		return bytes.size() >= sizeof(c_Magic) && std::memcmp(bytes.data(), c_Magic, sizeof(c_Magic)) == 0;
	}

	////////////////////////////////////////////////////////////////////////////////
	// CookedSceneWriter
	////////////////////////////////////////////////////////////////////////////////

	bool CookedSceneWriter::Write(SceneDocumentKind kind, const EntityPrototype& prototype, std::string_view name, const nlohmann::json& settings,
		std::vector<uint8_t>& outBytes, std::string* outError)
	{
		ST_PROFILE_FUNCTION();
		const size_t count = prototype.GetEntityCount();
		if (count > Scene::c_MaxEntities)
			return Fail(outError, fmt::format("{} entities are more than a scene holds ({})", count, Scene::c_MaxEntities));

		StringTable strings;
		std::vector<SectionData> sections;
		if (kind == SceneDocumentKind::Scene)
		{
			nlohmann::json info = { { "Name", std::string(name) } };
			if (!settings.is_null())
				info["Settings"] = settings;
			const std::string text = JsonUtils::Dump(info);
			sections.push_back(SectionData { SectionKind::SceneInfo, 0, 1, std::vector<uint8_t>(text.begin(), text.end()) });
		}

		BinaryWriter ids;
		BinaryWriter parents;
		for (size_t index = 0; index < count; index++)
		{
			ids.Write(static_cast<uint64_t>(prototype.GetEntityID(index)));
			const uint32_t parent = prototype.GetParentIndices()[index];
			parents.Write(parent == EntityPrototype::c_NoParent ? c_NoParent : parent);
		}
		sections.push_back(SectionData { SectionKind::EntityIDs, 0, static_cast<uint32_t>(count), ids.TakeData() });
		sections.push_back(SectionData { SectionKind::Parents, 0, static_cast<uint32_t>(count), parents.TakeData() });

		// The entities of each component type, in registration order (the document's type order) and entity order.
		const std::vector<const ComponentInfo*>& infos = ComponentRegistry::GetAll();
		std::unordered_map<const ComponentInfo*, size_t> registrationIndex;
		for (size_t index = 0; index < infos.size(); index++)
			registrationIndex.emplace(infos[index], index);
		std::vector<std::vector<std::pair<uint32_t, const void*>>> members(infos.size());
		for (size_t entity = 0; entity < count; entity++)
		{
			const std::span<const ComponentInfo* const> components = prototype.GetComponents(entity);
			const std::span<const void* const> values = prototype.GetComponentValues(entity);
			for (size_t slot = 0; slot < components.size(); slot++)
			{
				const auto found = registrationIndex.find(components[slot]);
				if (found != registrationIndex.end())
					members[found->second].emplace_back(static_cast<uint32_t>(entity), values[slot]);
			}
		}

		BinaryWriter types;
		uint32_t typeCount = 0;
		for (size_t type = 0; type < infos.size(); type++)
		{
			if (members[type].empty())
				continue;
			const ComponentInfo& info = *infos[type];
			const std::vector<const PropertyInfo*> properties = GetCookedProperties(info);
			const ExtraKind extra = GetExtraKind(info);
			types.Write(strings.Add(info.Name));
			types.Write(static_cast<uint32_t>(properties.size()));
			types.Write(static_cast<uint32_t>(extra));
			for (const PropertyInfo* property : properties)
			{
				types.Write(strings.Add(property->Name));
				types.Write(static_cast<uint32_t>(property->Type));
			}

			const std::vector<std::pair<uint32_t, const void*>>& column = members[type];
			const bool dense = column.size() == count;
			BinaryWriter writer;
			writer.Write(dense ? c_ColumnDense : 0u);
			writer.Write(static_cast<uint32_t>(column.size()));
			if (!dense)
			{
				for (const auto& [entity, value] : column)
					writer.Write(entity);
			}
			for (const PropertyInfo* property : properties)
			{
				for (const auto& [entity, value] : column)
				{
					std::string error;
					if (!WritePropertyValue(writer, property->Type, property->GetValue(value), &error))
						return Fail(outError, fmt::format("Entity {}: {}.{}: {}", prototype.GetEntityID(entity).ToString(), info.Name, property->Name, error));
				}
			}
			for (const auto& [entity, value] : column)
			{
				std::vector<uint8_t> bytes;
				std::string error;
				if (extra == ExtraKind::Binary)
				{
					BinaryWriter extraWriter;
					if (!info.SerializeExtraBinary(value, extraWriter, &error))
						return Fail(outError, fmt::format("Entity {}: {}: {}", prototype.GetEntityID(entity).ToString(), info.Name, error));
					bytes = extraWriter.TakeData();
				}
				else if (extra == ExtraKind::MessagePack)
				{
					nlohmann::json json = nlohmann::json::object();
					info.SerializeExtra(value, json);
					if (!ToMessagePack(json, bytes, &error))
						return Fail(outError, fmt::format("Entity {}: {}: {}", prototype.GetEntityID(entity).ToString(), info.Name, error));
				}
				else
				{
					continue;
				}
				writer.Write(static_cast<uint32_t>(bytes.size()));
				writer.WriteBytes(bytes.data(), bytes.size());
			}
			sections.push_back(SectionData { SectionKind::Column, typeCount, static_cast<uint32_t>(column.size()), writer.TakeData() });
			typeCount++;
		}
		sections.push_back(SectionData { SectionKind::ComponentTypes, 0, typeCount, types.TakeData() });

		BinaryWriter unknown;
		uint32_t unknownCount = 0;
		for (size_t entity = 0; entity < count; entity++)
		{
			const nlohmann::json* components = prototype.GetUnknownComponents(entity);
			if (!components)
				continue;
			std::vector<uint8_t> bytes;
			std::string error;
			if (!ToMessagePack(*components, bytes, &error))
				return Fail(outError, fmt::format("Entity {}: unknown components: {}", prototype.GetEntityID(entity).ToString(), error));
			unknown.Write(static_cast<uint32_t>(entity));
			unknown.Write(static_cast<uint32_t>(bytes.size()));
			unknown.WriteBytes(bytes.data(), bytes.size());
			unknownCount++;
		}
		if (unknownCount > 0)
			sections.push_back(SectionData { SectionKind::UnknownComponents, 0, unknownCount, unknown.TakeData() });
		sections.insert(sections.begin(), SectionData { SectionKind::Strings, 0, strings.GetCount(), strings.Write() });

		// The header, the sections (each aligned), the table.
		std::vector<uint8_t> bytes(sizeof(Header), 0);
		std::vector<Section> table;
		for (const SectionData& section : sections)
		{
			Align(bytes);
			Section entry {};
			entry.Kind = static_cast<uint32_t>(section.Kind);
			entry.Index = section.Index;
			entry.Offset = bytes.size();
			entry.Size = section.Bytes.size();
			entry.Count = section.Count;
			entry.Crc = Hash::CRC32C(section.Bytes);
			table.push_back(entry);
			bytes.insert(bytes.end(), section.Bytes.begin(), section.Bytes.end());
		}
		Align(bytes);
		const uint64_t tableOffset = bytes.size();
		const auto* tableBytes = reinterpret_cast<const uint8_t*>(table.data());
		bytes.insert(bytes.end(), tableBytes, tableBytes + table.size() * sizeof(Section));

		Header header {};
		std::memcpy(header.Magic, c_Magic, sizeof(c_Magic));
		header.Version = c_Version;
		header.Flags = 0;
		header.HeaderSize = sizeof(Header);
		header.Kind = static_cast<uint32_t>(kind);
		header.EntityCount = static_cast<uint32_t>(count);
		header.ComponentTypeCount = typeCount;
		header.StringCount = strings.GetCount();
		header.StringBytes = strings.GetBytes();
		header.SectionTableOffset = tableOffset;
		header.SectionCount = static_cast<uint32_t>(table.size());
		header.SectionTableCrc = Hash::CRC32C(std::span<const uint8_t>(tableBytes, table.size() * sizeof(Section)));
		header.HeaderCrc = ComputeHeaderCrc(header);
		std::memcpy(bytes.data(), &header, sizeof(header));
		outBytes = std::move(bytes);
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// CookedSceneReader
	////////////////////////////////////////////////////////////////////////////////

	namespace
	{

		struct StoredProperty
		{
			std::string_view Name;
			PropertyType Type = PropertyType::Float;
		};

		struct StoredType
		{
			std::string_view Name;
			ExtraKind Extra = ExtraKind::None;
			std::vector<StoredProperty> Properties;
		};

		// The fewest bytes a value of the type takes (PropertyBinary: strings their length at least).
		uint64_t GetMinimumValueBytes(PropertyType type)
		{
			switch (type)
			{
				case PropertyType::Bool:   return 1;
				case PropertyType::Int:
				case PropertyType::UInt:
				case PropertyType::Float:
				case PropertyType::Enum:
				case PropertyType::String: return 4;
				case PropertyType::Vec2:
				case PropertyType::Asset:
				case PropertyType::Entity: return 8;
				case PropertyType::Vec3:
				case PropertyType::Color3: return 12;
				case PropertyType::Vec4:
				case PropertyType::Quat:
				case PropertyType::Color4: return 16;
			}
			return 1;
		}

		// One column decoded (on a job of its own): the values of a registered type, or the JSON of a type this build does
		// not register.
		struct DecodedColumn
		{
			const ComponentInfo* Info = nullptr;
			Scope<ComponentValueArray> Values;
			std::vector<uint32_t> Entities;
			std::vector<nlohmann::json> Unknown; // Per member, for a type this build does not register
			std::vector<std::string> Warnings;
			std::string Error;
			bool Cancelled = false;
		};

		// Decodes a column section of `stored` for `entityCount` entities.
		void DecodeColumn(std::span<const uint8_t> bytes, uint32_t declaredCount, const StoredType& stored, uint32_t entityCount,
			const CookedSceneReadOptions& options, DecodedColumn& out)
		{
			auto fail = [&out, &stored](std::string message)
			{
				out.Error = fmt::format("Component '{}': {}", stored.Name, message);
			};
			BinaryReader reader(bytes);
			const uint32_t flags = reader.Read<uint32_t>();
			const uint32_t members = reader.Read<uint32_t>();
			if (!reader.IsValid())
				return fail("its column ends early");
			if ((flags & ~c_ColumnDense) != 0)
				return fail(fmt::format("its column has unknown flags {:#x}", flags));
			if (members != declaredCount || members > entityCount)
				return fail(fmt::format("its column holds {} entities, its section says {} (of {})", members, declaredCount, entityCount));
			const bool dense = (flags & c_ColumnDense) != 0;
			if (dense && members != entityCount)
				return fail("its dense column does not hold every entity");
			out.Entities.resize(members);
			if (dense)
			{
				for (uint32_t index = 0; index < members; index++)
					out.Entities[index] = index;
			}
			else
			{
				if (members > reader.GetRemaining() / sizeof(uint32_t))
					return fail("its column ends early");
				for (uint32_t index = 0; index < members; index++)
				{
					const uint32_t entity = reader.Read<uint32_t>();
					if (entity >= entityCount || (index > 0 && entity <= out.Entities[index - 1]))
						return fail("its entities are not strictly increasing indices of the document's entities");
					out.Entities[index] = entity;
				}
			}

			// Every member's values take at least this many bytes: a column that claims more members than its bytes can hold is
			// refused before its values are allocated.
			uint64_t memberBytes = stored.Extra != ExtraKind::None ? sizeof(uint32_t) : 0;
			for (const StoredProperty& property : stored.Properties)
				memberBytes += GetMinimumValueBytes(property.Type);
			if (memberBytes > 0 && members > reader.GetRemaining() / memberBytes)
				return fail("its column ends early");

			const ComponentInfo* info = ComponentRegistry::Find(stored.Name);
			if (info && HasFlag(info->Flags, ComponentFlags::NoSerialize))
				info = nullptr;
			out.Info = info;
			if (info)
			{
				out.Values = info->CreateValueArray();
				out.Values->Reserve(members);
				for (uint32_t index = 0; index < members; index++)
					out.Values->AppendDefault();
			}
			else
			{
				out.Unknown.assign(members, nlohmann::json::object());
			}

			uint64_t decoded = 0;
			auto cancelled = [&]()
			{
				if (++decoded % c_CancelCheckValues != 0 || !options.IsCancelled || !options.IsCancelled())
					return false;
				out.Cancelled = true;
				out.Error = "The read was cancelled";
				return true;
			};
			for (const StoredProperty& storedProperty : stored.Properties)
			{
				const PropertyInfo* property = nullptr;
				if (info)
				{
					for (const PropertyInfo& candidate : info->Properties)
					{
						if (candidate.Name == storedProperty.Name)
						{
							property = &candidate;
							break;
						}
					}
					if (!property)
						out.Warnings.push_back(fmt::format("Component '{}': unknown property '{}' skipped", info->Name, storedProperty.Name));
					else if (property->Type != storedProperty.Type)
						return fail(fmt::format("property '{}' is a {} in the document and a {} in this build", storedProperty.Name,
							PropertyTypeToString(storedProperty.Type), PropertyTypeToString(property->Type)));
				}
				PropertyInfo unknownProperty;
				unknownProperty.Name = std::string(storedProperty.Name);
				unknownProperty.Type = storedProperty.Type == PropertyType::Enum ? PropertyType::Int : storedProperty.Type;
				for (uint32_t index = 0; index < members; index++)
				{
					if (cancelled())
						return;
					std::string error;
					std::optional<PropertyValue> value = ReadPropertyValue(reader, storedProperty.Type, &error);
					if (!value)
					{
						if (!reader.IsValid())
							return fail(fmt::format("property '{}': {}", storedProperty.Name, error));
						out.Warnings.push_back(fmt::format("Entity #{}: {}.{}: {}; skipped", out.Entities[index], stored.Name, storedProperty.Name, error));
						continue;
					}
					if (info)
					{
						if (property && !property->SetValue(out.Values->At(index), *value, &error))
							out.Warnings.push_back(fmt::format("Entity #{}: {}.{}: {}", out.Entities[index], info->Name, property->Name, error));
					}
					else
					{
						out.Unknown[index][unknownProperty.Name] = PropertyValueToJson(unknownProperty, *value);
					}
				}
			}

			if (stored.Extra != ExtraKind::None)
			{
				bool reportedUnreadable = false;
				for (uint32_t index = 0; index < members; index++)
				{
					if (cancelled())
						return;
					const uint32_t size = reader.Read<uint32_t>();
					if (!reader.IsValid() || size > reader.GetRemaining() || size > c_MaxBlobBytes)
						return fail("its extra data ends early");
					const std::span<const uint8_t> extra = reader.ReadView(size);
					std::string error;
					if (stored.Extra == ExtraKind::Binary)
					{
						if (!info || !info->DeserializeExtraBinary)
						{
							if (!reportedUnreadable)
								out.Warnings.push_back(fmt::format("Component '{}': its binary extra data cannot be read by this build; dropped", stored.Name));
							reportedUnreadable = true;
							continue;
						}
						BinaryReader extraReader(extra);
						if (!info->DeserializeExtraBinary(out.Values->At(index), extraReader, &error))
						{
							if (!extraReader.IsValid())
								return fail(fmt::format("its extra data: {}", error));
							out.Warnings.push_back(fmt::format("Entity #{}: {}: {}", out.Entities[index], info->Name, error));
						}
						else if (extraReader.GetRemaining() != 0)
						{
							return fail("its extra data holds more than it reads");
						}
					}
					else
					{
						const std::optional<nlohmann::json> json = FromMessagePack(extra);
						if (!json || !json->is_object())
							return fail("its extra data is not a MessagePack object");
						if (!info)
						{
							for (const auto& [key, value] : json->items())
								out.Unknown[index][key] = value;
						}
						else if (info->DeserializeExtra && !info->DeserializeExtra(out.Values->At(index), *json, &error))
						{
							out.Warnings.push_back(fmt::format("Entity #{}: {}: {}", out.Entities[index], info->Name, error));
						}
					}
				}
			}
			if (reader.GetRemaining() != 0)
				return fail("its column holds more than its values");
		}

	}

	std::optional<CookedSceneContents> CookedSceneReader::Read(std::span<const uint8_t> bytes, const CookedSceneReadOptions& options, std::string* outError)
	{
		ST_PROFILE_FUNCTION();
		auto fail = [outError](std::string message) -> std::optional<CookedSceneContents>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};
		auto cancelled = [&options]() { return options.IsCancelled && options.IsCancelled(); };

		// The header.
		if (bytes.size() < sizeof(Header))
			return fail("The cooked document is shorter than its header");
		Header header {};
		std::memcpy(&header, bytes.data(), sizeof(header));
		if (std::memcmp(header.Magic, c_Magic, sizeof(c_Magic)) != 0)
			return fail("Not a cooked document");
		if (header.Version != c_Version)
			return fail(fmt::format("Unsupported cooked document version {} (supported: {})", header.Version, c_Version));
		if (ComputeHeaderCrc(header) != header.HeaderCrc)
			return fail("The cooked document's header is damaged (its checksum does not match)");
		if (header.Flags != 0)
			return fail(fmt::format("The cooked document has unknown flags {:#x}", header.Flags));
		if (header.HeaderSize != sizeof(Header))
			return fail(fmt::format("The cooked document's header has {} bytes, not {}", header.HeaderSize, sizeof(Header)));
		if (header.Kind > static_cast<uint32_t>(SceneDocumentKind::Model))
			return fail(fmt::format("The cooked document's kind {} is unknown", header.Kind));
		if (header.EntityCount > Scene::c_MaxEntities)
			return fail(fmt::format("The cooked document holds {} entities, more than a scene holds ({})", header.EntityCount, Scene::c_MaxEntities));

		// The section table.
		const uint64_t size = bytes.size();
		if (header.SectionTableOffset % c_SectionAlignment != 0 || header.SectionTableOffset < sizeof(Header) || header.SectionTableOffset > size
			|| header.SectionCount > (size - header.SectionTableOffset) / sizeof(Section))
			return fail("The cooked document's section table lies outside it");
		const std::span<const uint8_t> tableBytes = bytes.subspan(static_cast<size_t>(header.SectionTableOffset), header.SectionCount * sizeof(Section));
		if (Hash::CRC32C(tableBytes) != header.SectionTableCrc)
			return fail("The cooked document's section table is damaged (its checksum does not match)");
		std::vector<Section> sections(header.SectionCount);
		if (!sections.empty())
			std::memcpy(sections.data(), tableBytes.data(), tableBytes.size());
		std::vector<const Section*> byOffset;
		for (const Section& section : sections)
		{
			if (section.Kind < static_cast<uint32_t>(SectionKind::Strings) || section.Kind > static_cast<uint32_t>(SectionKind::UnknownComponents))
				return fail(fmt::format("The cooked document has a section of unknown kind {}", section.Kind));
			if (section.Offset % c_SectionAlignment != 0 || section.Offset < sizeof(Header) || section.Offset > header.SectionTableOffset
				|| section.Size > header.SectionTableOffset - section.Offset)
				return fail("A section of the cooked document lies outside it or is not aligned");
			byOffset.push_back(&section);
		}
		std::sort(byOffset.begin(), byOffset.end(), [](const Section* a, const Section* b) { return a->Offset < b->Offset; });
		for (size_t index = 1; index < byOffset.size(); index++)
		{
			if (byOffset[index - 1]->Offset + byOffset[index - 1]->Size > byOffset[index]->Offset)
				return fail("Sections of the cooked document overlap");
		}
		const Section* strings = nullptr;
		const Section* sceneInfo = nullptr;
		const Section* ids = nullptr;
		const Section* parents = nullptr;
		const Section* types = nullptr;
		const Section* unknown = nullptr;
		// Each type has its column: there are no more types than sections.
		if (header.ComponentTypeCount > sections.size())
			return fail("The cooked document's component types are more than its sections");
		std::vector<const Section*> columns(header.ComponentTypeCount, nullptr);
		for (const Section& section : sections)
		{
			const Section** single = nullptr;
			switch (static_cast<SectionKind>(section.Kind))
			{
				case SectionKind::Strings:           single = &strings; break;
				case SectionKind::SceneInfo:         single = &sceneInfo; break;
				case SectionKind::EntityIDs:         single = &ids; break;
				case SectionKind::Parents:           single = &parents; break;
				case SectionKind::ComponentTypes:    single = &types; break;
				case SectionKind::UnknownComponents: single = &unknown; break;
				case SectionKind::Column:
					if (section.Index >= columns.size() || columns[section.Index])
						return fail("A column of the cooked document names no component type or one named before");
					columns[section.Index] = &section;
					continue;
			}
			if (*single)
				return fail(fmt::format("The cooked document holds section {} twice", section.Kind));
			*single = &section;
		}
		const bool scene = header.Kind == static_cast<uint32_t>(SceneDocumentKind::Scene);
		if (!strings || !ids || !parents || !types || scene != (sceneInfo != nullptr))
			return fail("The cooked document lacks a section it needs");
		if (sceneInfo && sceneInfo->Count != 1)
			return fail("The cooked document's scene information does not match its count");
		if (unknown && unknown->Count > unknown->Size / (2 * sizeof(uint32_t)))
			return fail("The cooked document's unknown components are more than their section holds");
		for (const Section& section : sections)
		{
			if (Hash::CRC32C(bytes.subspan(static_cast<size_t>(section.Offset), static_cast<size_t>(section.Size))) != section.Crc)
				return fail(fmt::format("A section of the cooked document is damaged (kind {}: its checksum does not match)", section.Kind));
		}
		auto sectionBytes = [&bytes](const Section& section)
		{
			return bytes.subspan(static_cast<size_t>(section.Offset), static_cast<size_t>(section.Size));
		};
		const size_t totalSections = sections.size();
		size_t decodedSections = 0;
		auto reportSection = [&]()
		{
			decodedSections++;
			if (options.Progress)
				options.Progress(static_cast<float>(static_cast<double>(decodedSections) / static_cast<double>(std::max<size_t>(totalSections, 1))));
		};
		if (cancelled())
			return fail("The read was cancelled");

		// Strings.
		const uint32_t stringCount = header.StringCount;
		if (strings->Count != stringCount || stringCount >= strings->Size / sizeof(uint32_t)
			|| strings->Size != (static_cast<uint64_t>(stringCount) + 1) * sizeof(uint32_t) + header.StringBytes)
			return fail("The cooked document's strings do not match their section");
		std::vector<std::string_view> names(stringCount);
		{
			const std::span<const uint8_t> section = sectionBytes(*strings);
			BinaryReader reader(section);
			std::vector<uint32_t> offsets(static_cast<size_t>(stringCount) + 1);
			for (uint32_t& offset : offsets)
				offset = reader.Read<uint32_t>();
			const char* text = reinterpret_cast<const char*>(section.data()) + offsets.size() * sizeof(uint32_t);
			if (offsets.front() != 0 || offsets.back() != header.StringBytes)
				return fail("The cooked document's strings do not match their section");
			for (uint32_t index = 0; index < stringCount; index++)
			{
				if (offsets[index + 1] < offsets[index])
					return fail("The cooked document's strings do not match their section");
				names[index] = std::string_view(text + offsets[index], offsets[index + 1] - offsets[index]);
			}
		}
		reportSection();

		CookedSceneContents contents;
		contents.Kind = static_cast<SceneDocumentKind>(header.Kind);
		if (sceneInfo)
		{
			const std::span<const uint8_t> section = sectionBytes(*sceneInfo);
			const std::optional<nlohmann::json> info = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(section.data()), section.size()));
			if (!info || !info->is_object())
				return fail("The cooked document's scene information is not a JSON object");
			contents.Name = JsonUtils::GetString(*info, "Name", "Untitled");
			if (const nlohmann::json* settings = JsonUtils::Find(*info, "Settings"))
				contents.Settings = *settings;
			reportSection();
		}

		// Entities and parents.
		const uint32_t entityCount = header.EntityCount;
		if (ids->Count != entityCount || ids->Size != static_cast<uint64_t>(entityCount) * sizeof(uint64_t))
			return fail("The cooked document's entity IDs do not match their count");
		if (parents->Count != entityCount || parents->Size != static_cast<uint64_t>(entityCount) * sizeof(uint32_t))
			return fail("The cooked document's parents do not match their count");
		Ref<EntityPrototype> prototype = CreateRef<EntityPrototype>();
		EntityPrototype& built = *prototype;
		{
			BinaryReader idReader(sectionBytes(*ids));
			BinaryReader parentReader(sectionBytes(*parents));
			built.m_IDs.reserve(entityCount);
			built.m_IndexByID.reserve(entityCount);
			built.m_Parents.resize(entityCount, EntityPrototype::c_NoParent);
			for (uint32_t index = 0; index < entityCount; index++)
			{
				const UUID id(idReader.Read<uint64_t>());
				if (!id.IsValid() || !built.m_IndexByID.emplace(id, index).second)
					return fail(fmt::format("The cooked document holds a null entity ID or one twice (entity #{})", index));
				built.m_IDs.push_back(id);
				const uint32_t parent = parentReader.Read<uint32_t>();
				if (parent == c_NoParent)
					built.m_TopLevel.push_back(index);
				else if (parent >= index)
					return fail(fmt::format("Entity #{} of the cooked document names parent #{}, which does not come before it", index, parent));
				else
					built.m_Parents[index] = parent;
			}
		}
		reportSection();
		reportSection();

		// Component types.
		std::vector<StoredType> storedTypes(header.ComponentTypeCount);
		{
			BinaryReader reader(sectionBytes(*types));
			if (types->Count != header.ComponentTypeCount)
				return fail("The cooked document's component types do not match their count");
			std::unordered_set<std::string_view> typeNames;
			for (StoredType& type : storedTypes)
			{
				const uint32_t name = reader.Read<uint32_t>();
				const uint32_t propertyCount = reader.Read<uint32_t>();
				const uint32_t extra = reader.Read<uint32_t>();
				if (!reader.IsValid() || name >= stringCount || propertyCount > reader.GetRemaining() / (2 * sizeof(uint32_t))
					|| extra > static_cast<uint32_t>(ExtraKind::MessagePack))
					return fail("The cooked document's component types are damaged");
				type.Name = names[name];
				type.Extra = static_cast<ExtraKind>(extra);
				if (!typeNames.insert(type.Name).second)
					return fail(fmt::format("The cooked document holds component type '{}' twice", type.Name));
				type.Properties.resize(propertyCount);
				for (StoredProperty& property : type.Properties)
				{
					const uint32_t propertyName = reader.Read<uint32_t>();
					const uint32_t propertyType = reader.Read<uint32_t>();
					if (!reader.IsValid() || propertyName >= stringCount || propertyType > static_cast<uint32_t>(PropertyType::Entity))
						return fail(fmt::format("The cooked document's component type '{}' has a damaged property", type.Name));
					property.Name = names[propertyName];
					property.Type = static_cast<PropertyType>(propertyType);
				}
			}
			if (reader.GetRemaining() != 0)
				return fail("The cooked document's component types hold more than their count");
		}
		reportSection();

		// Columns, decoded in parallel: each into arrays of its own.
		std::vector<DecodedColumn> decoded(storedTypes.size());
		auto decodeColumn = [&](size_t type)
		{
			if (columns[type])
				DecodeColumn(sectionBytes(*columns[type]), columns[type]->Count, storedTypes[type], entityCount, options, decoded[type]);
		};
		if (JobSystem::IsInitialized() && storedTypes.size() > 1)
		{
			JobSystem::ParallelFor(static_cast<uint32_t>(storedTypes.size()), 1, [&](uint32_t begin, uint32_t end)
			{
				for (uint32_t type = begin; type < end; type++)
					decodeColumn(type);
			});
		}
		else
		{
			for (size_t type = 0; type < storedTypes.size(); type++)
				decodeColumn(type);
		}
		for (DecodedColumn& column : decoded)
		{
			if (!column.Error.empty())
				return fail(column.Error);
			for (std::string& warning : column.Warnings)
				contents.Warnings.push_back(std::move(warning));
			reportSection();
		}
		if (cancelled())
			return fail("The read was cancelled");

		// What this build does not register: the columns of unknown types and the unknown components, as JSON per entity.
		std::unordered_map<uint32_t, nlohmann::json> unknownByEntity;
		for (size_t type = 0; type < decoded.size(); type++)
		{
			const DecodedColumn& column = decoded[type];
			if (column.Info || !columns[type])
				continue;
			contents.Warnings.push_back(fmt::format("Unknown component '{}' ({} {}) kept unchanged: no module of this build registers it", storedTypes[type].Name,
				column.Entities.size(), column.Entities.size() == 1 ? "entity" : "entities"));
			for (size_t member = 0; member < column.Entities.size(); member++)
			{
				nlohmann::json& components = unknownByEntity[column.Entities[member]];
				if (!components.is_object())
					components = nlohmann::json::object();
				components[std::string(storedTypes[type].Name)] = column.Unknown[member];
			}
		}
		if (unknown)
		{
			BinaryReader reader(sectionBytes(*unknown));
			for (uint32_t entry = 0; entry < unknown->Count; entry++)
			{
				const uint32_t entity = reader.Read<uint32_t>();
				const uint32_t blobSize = reader.Read<uint32_t>();
				if (!reader.IsValid() || entity >= entityCount || blobSize > reader.GetRemaining() || blobSize > c_MaxBlobBytes)
					return fail("The cooked document's unknown components are damaged");
				const std::optional<nlohmann::json> json = FromMessagePack(reader.ReadView(blobSize));
				if (!json || !json->is_object())
					return fail("The cooked document's unknown components are not MessagePack objects");
				nlohmann::json& components = unknownByEntity[entity];
				if (!components.is_object())
					components = nlohmann::json::object();
				for (const auto& [name, component] : json->items())
					components[name] = component;
			}
			if (reader.GetRemaining() != 0)
				return fail("The cooked document's unknown components hold more than their count");
			reportSection();
		}

		// Each entity's components in registration order, as EntityPrototype::Compile lists them.
		std::unordered_map<const ComponentInfo*, size_t> registrationIndex;
		const std::vector<const ComponentInfo*>& infos = ComponentRegistry::GetAll();
		for (size_t index = 0; index < infos.size(); index++)
			registrationIndex.emplace(infos[index], index);
		std::vector<size_t> order;
		for (size_t type = 0; type < decoded.size(); type++)
		{
			if (decoded[type].Info)
				order.push_back(type);
		}
		std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return registrationIndex.at(decoded[a].Info) < registrationIndex.at(decoded[b].Info); });
		std::vector<uint32_t> counts(static_cast<size_t>(entityCount) + 1, 0);
		for (const size_t type : order)
		{
			for (const uint32_t entity : decoded[type].Entities)
				counts[entity]++;
		}
		built.m_ComponentOffsets.resize(static_cast<size_t>(entityCount) + 1, 0);
		for (uint32_t index = 0; index < entityCount; index++)
			built.m_ComponentOffsets[index + 1] = built.m_ComponentOffsets[index] + counts[index];
		built.m_Components.resize(built.m_ComponentOffsets[entityCount], nullptr);
		built.m_Values.resize(built.m_ComponentOffsets[entityCount], nullptr);
		std::vector<uint32_t> cursor(built.m_ComponentOffsets.begin(), built.m_ComponentOffsets.end() - 1);
		for (const size_t type : order)
		{
			DecodedColumn& column = decoded[type];
			for (size_t member = 0; member < column.Entities.size(); member++)
			{
				const uint32_t slot = cursor[column.Entities[member]]++;
				built.m_Components[slot] = column.Info;
				built.m_Values[slot] = column.Values->At(member);
			}
			built.m_Arrays.push_back(std::move(column.Values));
		}
		built.m_UnknownIndex.resize(entityCount, EntityPrototype::c_NoUnknownComponents);
		std::vector<uint32_t> unknownEntities;
		for (const auto& [entity, components] : unknownByEntity)
			unknownEntities.push_back(entity);
		std::sort(unknownEntities.begin(), unknownEntities.end());
		for (const uint32_t entity : unknownEntities)
		{
			built.m_UnknownIndex[entity] = static_cast<uint32_t>(built.m_UnknownComponents.size());
			built.m_UnknownComponents.push_back(std::move(unknownByEntity[entity]));
		}
		built.FindReferences();
		built.ComputeMemoryUsage();
		contents.Prototype = std::move(prototype);
		return contents;
	}

	Ref<Scene> CookedSceneReader::ReadScene(std::span<const uint8_t> bytes, const CookedSceneReadOptions& options, std::vector<std::string>* outWarnings,
		std::string* outError)
	{
		ST_PROFILE_FUNCTION();
		std::optional<CookedSceneContents> contents = Read(bytes, options, outError);
		if (!contents)
			return nullptr;
		if (contents->Kind != SceneDocumentKind::Scene)
		{
			if (outError)
				*outError = fmt::format("The cooked document is a {}, not a scene", SceneDocumentKindToString(contents->Kind));
			return nullptr;
		}
		Ref<Scene> scene = CreateRef<Scene>(contents->Name);
		std::vector<std::string> warnings = std::move(contents->Warnings);
		if (contents->Settings.is_object())
			SceneSerializer::DeserializeSettings(contents->Settings, scene->GetSettings(), &warnings);
		// The prototype keeps the document's IDs: instantiated with them, references between its entities stay as they are.
		InstantiationOptions instantiation;
		instantiation.KeepIDs = true;
		std::string error;
		scene->Instantiate(contents->Prototype, instantiation, &error);
		if (!error.empty())
		{
			if (outError)
				*outError = std::move(error);
			return nullptr;
		}
		if (outWarnings)
		{
			for (std::string& warning : warnings)
				outWarnings->push_back(std::move(warning));
		}
		return scene;
	}

}
