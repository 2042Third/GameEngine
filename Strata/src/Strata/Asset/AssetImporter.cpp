#include "stpch.h"
#include "Strata/Asset/AssetImporter.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Reflection/PropertyBuilder.h"
#include "Strata/Reflection/PropertyJson.h"

namespace Strata
{

	namespace
	{

		struct ImporterStorage
		{
			std::mutex Mutex;
			std::atomic<bool> Open = false;
			std::vector<Scope<AssetImporter>> Importers;
		};

		ImporterStorage& GetImporterStorageUnchecked()
		{
			static ImporterStorage s_Storage;
			return s_Storage;
		}

		// Why a number is not within its setting's range, or nothing when it is (or the setting has no range).
		std::optional<std::string> CheckRange(const PropertyInfo& property, const PropertyValue& value)
		{
			if (!property.HasRange())
				return std::nullopt;
			const auto outside = [&property](double number) { return !(number >= property.Min && number <= property.Max); };
			if (const float* number = std::get_if<float>(&value); number && property.Type == PropertyType::Float && outside(*number))
				return fmt::format("{} is outside its range, {} to {}", *number, property.Min, property.Max);
			const int64_t min = static_cast<int64_t>(property.Min);
			const int64_t max = static_cast<int64_t>(property.Max);
			if (const uint32_t* number = std::get_if<uint32_t>(&value); number && property.Type == PropertyType::UInt && outside(*number))
				return fmt::format("{} is outside its range, {} to {}", *number, min, max);
			if (const int32_t* number = std::get_if<int32_t>(&value); number && property.Type == PropertyType::Int && outside(*number))
				return fmt::format("{} is outside its range, {} to {}", *number, min, max);
			return std::nullopt;
		}

		ImporterStorage& GetImporterStorage()
		{
			ImporterStorage& storage = GetImporterStorageUnchecked();
			ST_CORE_VERIFY(storage.Open.load(std::memory_order_acquire),
				"The asset importer registry is used before Engine::RegisterBuiltinModules() registered the asset pipeline "
				"(ModuleRegistrationOptions::AssetPipeline, which programs without importers, such as shipped games, leave empty)");
			return storage;
		}

	}

	nlohmann::json AssetImporter::GetDefaultSettings(const std::filesystem::path&) const
	{
		return nlohmann::json::object();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Import settings
	////////////////////////////////////////////////////////////////////////////////

	ImportSettingsBuilder::ImportSettingsBuilder(std::vector<PropertyInfo>& properties)
		: m_Properties(properties)
	{
	}

	ImportSettingsBuilder& ImportSettingsBuilder::Bool(std::string name, bool defaultValue)
	{
		return Add(std::move(name), PropertyType::Bool, PropertyValue(defaultValue), 0.0f, 0.0f);
	}

	ImportSettingsBuilder& ImportSettingsBuilder::UInt(std::string name, uint32_t defaultValue, uint32_t max)
	{
		return Add(std::move(name), PropertyType::UInt, PropertyValue(defaultValue), 0.0f, static_cast<float>(max));
	}

	ImportSettingsBuilder& ImportSettingsBuilder::Float(std::string name, float defaultValue, float min, float max)
	{
		return Add(std::move(name), PropertyType::Float, PropertyValue(defaultValue), min, max);
	}

	ImportSettingsBuilder& ImportSettingsBuilder::Enum(std::string name, const std::vector<std::string>& choices, std::string_view defaultValue)
	{
		ST_CORE_ASSERT(std::find(choices.begin(), choices.end(), defaultValue) != choices.end(), "Import setting '{}': the default '{}' is no choice", name,
			defaultValue);
		std::vector<EnumValue> values;
		int32_t defaultIndex = 0;
		for (const std::string& choice : choices)
		{
			if (choice == defaultValue)
				defaultIndex = static_cast<int32_t>(values.size());
			values.push_back(EnumValue { choice, static_cast<int32_t>(values.size()) });
		}
		return Add(std::move(name), PropertyType::Enum, PropertyValue(defaultIndex), 0.0f, 0.0f, std::move(values));
	}

	ImportSettingsBuilder& ImportSettingsBuilder::DisplayName(std::string displayName)
	{
		GetLast().DisplayName = std::move(displayName);
		return *this;
	}

	ImportSettingsBuilder& ImportSettingsBuilder::Tooltip(std::string tooltip)
	{
		GetLast().Tooltip = std::move(tooltip);
		return *this;
	}

	ImportSettingsBuilder& ImportSettingsBuilder::Speed(float speed)
	{
		GetLast().Speed = speed;
		return *this;
	}

	PropertyInfo& ImportSettingsBuilder::GetLast()
	{
		ST_CORE_ASSERT(!m_Properties.empty(), "ImportSettingsBuilder: a modifier needs a setting before it");
		return m_Properties.back();
	}

	ImportSettingsBuilder& ImportSettingsBuilder::Add(std::string name, PropertyType type, PropertyValue defaultValue, float min, float max,
		std::vector<EnumValue> enumValues)
	{
		PropertyInfo& property = m_Properties.emplace_back();
		PropertyOptions options;
		options.Min = min;
		options.Max = max;
		Detail::ApplyPropertyOptions(property, name, type, options);
		property.EnumValues = std::move(enumValues);
		// What the accessors convert with: the property without its accessors (a copy of itself would hold them).
		PropertyInfo shape = property;
		property.Getter = [shape, defaultValue](const void* object) -> PropertyValue
		{
			const nlohmann::json& settings = *static_cast<const nlohmann::json*>(object);
			if (!settings.is_object())
				return defaultValue;
			const auto it = settings.find(shape.Name);
			if (it == settings.end())
				return defaultValue;
			std::optional<PropertyValue> value = PropertyValueFromJson(shape, *it);
			// An enum must name one of its options; any other value reads as the default, as the importer reads it.
			if (!value || (shape.Type == PropertyType::Enum && !shape.FindEnumValue(std::get<int32_t>(*value))))
				return defaultValue;
			return *value;
		};
		property.Setter = [shape](void* object, const PropertyValue& value)
		{
			nlohmann::json& settings = *static_cast<nlohmann::json*>(object);
			if (!settings.is_object())
				settings = nlohmann::json::object();
			settings[shape.Name] = PropertyValueToJson(shape, value);
		};
		return *this;
	}

	bool NormalizeImportSettings(const AssetImporter& importer, const nlohmann::json& settings, nlohmann::json& outSettings, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		if (!settings.is_object())
			return fail("Import settings must be an object of the form {\"Setting\": value}");
		const std::vector<PropertyInfo> properties = importer.GetSettingsProperties();
		std::string names;
		for (const PropertyInfo& property : properties)
			names += (names.empty() ? "" : ", ") + property.Name;

		nlohmann::json normalized = nlohmann::json::object();
		for (const auto& [key, value] : settings.items())
		{
			const PropertyInfo* property = FindProperty(properties, key);
			if (!property)
			{
				if (properties.empty())
					return fail(fmt::format("{} assets have no import settings ('{}' is none)", AssetTypeToString(importer.GetType()), key));
				return fail(fmt::format("Unknown import setting '{}' of {} assets (settings: {})", key, AssetTypeToString(importer.GetType()), names));
			}
			std::string error;
			const std::optional<PropertyValue> parsed = PropertyValueFromJson(*property, value, &error);
			if (!parsed)
				return fail(fmt::format("Import setting '{}': {}", property->Name, error.empty() ? std::string("invalid value") : error));
			// Refused rather than clamped (as PropertyInfo::SetValue would): whoever asked for a value outside the range
			// learns the range instead of silently getting another value.
			if (const std::optional<std::string> outside = CheckRange(*property, *parsed))
				return fail(fmt::format("Import setting '{}': {}", property->Name, *outside));
			if (!property->SetValue(&normalized, *parsed, &error))
				return fail(fmt::format("Import setting '{}': {}", property->Name, error.empty() ? std::string("invalid value") : error));
		}
		outSettings = std::move(normalized);
		return true;
	}

	void AssetImporterRegistry::BeginRegistration()
	{
		GetImporterStorageUnchecked().Open.store(true, std::memory_order_release);
	}

	void AssetImporterRegistry::Register(Scope<AssetImporter> importer)
	{
		ImporterStorage& storage = GetImporterStorage();
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		storage.Importers.push_back(std::move(importer));
	}

	const AssetImporter* AssetImporterRegistry::FindByExtension(std::string_view extension)
	{
		const std::string lowered = StringUtils::ToLower(extension);
		ImporterStorage& storage = GetImporterStorage();
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		// Later registrations win, so tools and tests can override built-in importers.
		for (auto it = storage.Importers.rbegin(); it != storage.Importers.rend(); ++it)
		{
			for (const std::string& candidate : (*it)->GetExtensions())
			{
				if (candidate == lowered)
					return it->get();
			}
		}
		return nullptr;
	}

	std::vector<const AssetImporter*> AssetImporterRegistry::GetAll()
	{
		ImporterStorage& storage = GetImporterStorage();
		std::scoped_lock<std::mutex> lock(storage.Mutex);
		std::vector<const AssetImporter*> importers;
		for (const Scope<AssetImporter>& importer : storage.Importers)
			importers.push_back(importer.get());
		return importers;
	}

	AssetHandle DeriveSubAssetHandle(AssetHandle parent, std::string_view key)
	{
		uint64_t value = Hash::Combine(Hash::FNV1a(key), static_cast<uint64_t>(parent));
		// Never collide with the null handle or the range reserved for built-in assets.
		if (value <= c_MaxBuiltinAssetHandle)
			value += c_MaxBuiltinAssetHandle + 1;
		return UUID(value);
	}

	std::optional<std::vector<uint8_t>> ReadImportDependency(const ImportContext& context, ImportResult& result, const std::filesystem::path& path)
	{
		const std::filesystem::path normalized = path.lexically_normal();
		const std::filesystem::path assetDirectory = context.AssetDirectory.lexically_normal();
		if (!FileSystem::IsInside(normalized, assetDirectory))
			return std::nullopt;

		ImportDependency dependency;
		dependency.Path = normalized;
		dependency.Exists = FileSystem::IsRegularFile(normalized);

		// The state is taken before reading: a change while reading then shows as a later modification.
		std::optional<std::vector<uint8_t>> bytes;
		if (dependency.Exists)
		{
			dependency.Size = FileSystem::GetFileSize(normalized).value_or(0);
			dependency.Time = FileSystem::GetLastWriteTime(normalized).value_or(0);
			// A link leading out of the asset directory is not read.
			if (FileSystem::IsInsideResolved(normalized, assetDirectory))
				bytes = FileSystem::ReadBytes(normalized);
			if (bytes)
				dependency.Hash = Hash::FNV1a(std::span<const uint8_t>(*bytes));
			else
				dependency.Size = ImportDependency::c_Unreadable; // Never current: retried until the file can be read
		}

		const bool known = std::any_of(result.Dependencies.begin(), result.Dependencies.end(), [&](const ImportDependency& existing) { return existing.Path == normalized; });
		if (!known)
			result.Dependencies.push_back(std::move(dependency));
		return bytes;
	}

}
