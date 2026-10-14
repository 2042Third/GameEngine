#include "stpch.h"
#include "Strata/World/WorldAsset.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/PropertyJson.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		bool HasControlCharacter(std::string_view text)
		{
			return std::any_of(text.begin(), text.end(), [](char character)
			{
				const unsigned char byte = static_cast<unsigned char>(character);
				return byte < 0x20 || byte == 0x7F;
			});
		}

		// Readers of one member of an object. A missing member keeps `out` unless it is required; a member of the wrong type
		// or out of range fails with a reason naming `what` and the key.
		class Reader
		{
		public:
			Reader(const nlohmann::json& object, std::string what, std::string& error)
				: m_Object(object), m_What(std::move(what)), m_Error(error)
			{
			}

			bool String(std::string_view key, std::string& out, bool required)
			{
				const nlohmann::json* value = Find(key, required);
				if (!value)
					return m_Error.empty();
				if (!value->is_string())
					return Fail(key, "must be a string");
				out = value->get<std::string>();
				return true;
			}

			bool Bool(std::string_view key, bool& out)
			{
				const nlohmann::json* value = Find(key, false);
				if (!value)
					return true;
				if (!value->is_boolean())
					return Fail(key, "must be true or false");
				out = value->get<bool>();
				return true;
			}

			bool Number(std::string_view key, double& out, bool required)
			{
				const nlohmann::json* value = Find(key, required);
				if (!value)
					return m_Error.empty();
				if (!value->is_number())
					return Fail(key, "must be a number");
				out = value->get<double>();
				if (!std::isfinite(out))
					return Fail(key, "must be finite");
				return true;
			}

			bool Float(std::string_view key, float& out, bool required)
			{
				double value = out;
				if (!Number(key, value, required))
					return false;
				if (std::abs(value) > static_cast<double>(std::numeric_limits<float>::max()))
					return Fail(key, "is out of range");
				out = static_cast<float>(value);
				return true;
			}

			bool Integer(std::string_view key, int64_t& out, bool required)
			{
				const nlohmann::json* value = Find(key, required);
				if (!value)
					return m_Error.empty();
				if (value->is_number_unsigned())
				{
					const uint64_t unsignedValue = value->get<uint64_t>();
					if (unsignedValue > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
						return Fail(key, "is out of range");
					out = static_cast<int64_t>(unsignedValue);
					return true;
				}
				if (!value->is_number_integer())
					return Fail(key, "must be an integer");
				out = value->get<int64_t>();
				return true;
			}

			bool Count(std::string_view key, uint64_t& out)
			{
				int64_t value = 0;
				if (!Integer(key, value, false))
					return false;
				if (value < 0)
					return Fail(key, "must not be negative");
				out = static_cast<uint64_t>(value);
				return true;
			}

			bool Handle(std::string_view key, AssetHandle& out, bool required)
			{
				const nlohmann::json* value = Find(key, required);
				if (!value)
					return m_Error.empty();
				const std::optional<UUID> handle = UUIDFromJson(*value);
				if (!handle)
					return Fail(key, "must be an asset handle (16 hex digits) or null");
				out = *handle;
				return true;
			}

			// An array of `count` finite numbers.
			bool Numbers(std::string_view key, double* out, size_t count, bool required)
			{
				const nlohmann::json* value = Find(key, required);
				if (!value)
					return m_Error.empty();
				if (!value->is_array() || value->size() != count)
					return Fail(key, fmt::format("must be an array of {} numbers", count));
				for (size_t index = 0; index < count; index++)
				{
					const nlohmann::json& item = (*value)[index];
					if (!item.is_number() || !std::isfinite(item.get<double>()))
						return Fail(key, fmt::format("must be an array of {} finite numbers", count));
					out[index] = item.get<double>();
				}
				return true;
			}

			bool Vec3(std::string_view key, glm::vec3& out)
			{
				double values[3] = {};
				if (!Numbers(key, values, 3, true))
					return false;
				for (int axis = 0; axis < 3; axis++)
				{
					if (std::abs(values[axis]) > static_cast<double>(std::numeric_limits<float>::max()))
						return Fail(key, "is out of range");
					out[axis] = static_cast<float>(values[axis]);
				}
				return true;
			}

			// An array member (null or missing: none), with at most `maxSize` items.
			const nlohmann::json* Array(std::string_view key, size_t maxSize, bool required)
			{
				const nlohmann::json* value = Find(key, required);
				if (!value)
					return nullptr;
				if (!value->is_array())
				{
					Fail(key, "must be an array");
					return nullptr;
				}
				if (value->size() > maxSize)
				{
					Fail(key, fmt::format("has more than {} items", maxSize));
					return nullptr;
				}
				return value;
			}
		private:
			const nlohmann::json* Find(std::string_view key, bool required)
			{
				const nlohmann::json* value = JsonUtils::Find(m_Object, key);
				if (value && value->is_null() && !required)
					return nullptr;
				if (!value && required)
					Fail(key, "is missing");
				return value;
			}

			bool Fail(std::string_view key, const std::string& reason)
			{
				m_Error = fmt::format("{}: \"{}\" {}", m_What, key, reason);
				return false;
			}

			const nlohmann::json& m_Object;
			std::string m_What;
			std::string& m_Error;
		};

		bool ReadLayerID(Reader& reader, std::string_view key, uint8_t& out, std::string& error, std::string_view what)
		{
			int64_t id = 0;
			if (!reader.Integer(key, id, true))
				return false;
			if (id < 1 || id > 255)
			{
				error = fmt::format("{}: \"{}\" must be a layer ID from 1 to 255", what, key);
				return false;
			}
			out = static_cast<uint8_t>(id);
			return true;
		}

		bool ReadCoordinate(Reader& reader, std::string_view key, int32_t& out, std::string& error, std::string_view what)
		{
			int64_t coordinate = 0;
			if (!reader.Integer(key, coordinate, true))
				return false;
			if (std::abs(coordinate) > WorldCellKey::c_MaxCoordinate)
			{
				error = fmt::format("{}: \"{}\" is outside the world", what, key);
				return false;
			}
			out = static_cast<int32_t>(coordinate);
			return true;
		}

		std::string DescribeCell(const WorldData& data, const WorldCellKey& key)
		{
			const WorldLayer* layer = data.FindLayer(key.Layer);
			if (layer)
				return fmt::format("Cell ({}, {}, {})", layer->Name, key.X, key.Z);
			return fmt::format("Cell (layer {}, {}, {})", key.Layer, key.X, key.Z);
		}

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

	}

	std::optional<WorldData> WorldData::FromJson(const nlohmann::json& content, std::string* outError)
	{
		std::string error;
		auto fail = [&]() -> std::optional<WorldData>
		{
			if (outError)
				*outError = error;
			return std::nullopt;
		};
		if (!content.is_object())
		{
			error = "The world must be a JSON object";
			return fail();
		}

		WorldData data;
		Reader world(content, "The world", error);
		double origin[2] = { 0.0, 0.0 };
		if (!world.String("Name", data.Name, false) || !world.Handle("PersistentScene", data.PersistentScene, false) || !world.Numbers("GridOrigin", origin, 2, false))
			return fail();
		data.GridOrigin = glm::dvec2(origin[0], origin[1]);

		const nlohmann::json* layers = world.Array("Layers", c_MaxLayers, true);
		if (!layers)
			return fail();
		for (size_t index = 0; index < layers->size(); index++)
		{
			const nlohmann::json& item = (*layers)[index];
			const std::string what = fmt::format("Layer {}", index + 1);
			if (!item.is_object())
			{
				error = fmt::format("{} must be an object", what);
				return fail();
			}
			Reader reader(item, what, error);
			WorldLayer layer;
			if (!ReadLayerID(reader, "ID", layer.ID, error, what) || !reader.String("Name", layer.Name, true) || !reader.Float("CellSize", layer.CellSize, true)
				|| !reader.Float("StreamingDistance", layer.StreamingDistance, true) || !reader.Bool("EnabledByDefault", layer.EnabledByDefault)
				|| !reader.Bool("LoadBeforeStart", layer.LoadBeforeStart))
				return fail();
			data.Layers.push_back(std::move(layer));
		}

		if (const nlohmann::json* cells = world.Array("Cells", c_MaxCells, false))
		{
			data.Cells.reserve(cells->size());
			for (size_t index = 0; index < cells->size(); index++)
			{
				const nlohmann::json& item = (*cells)[index];
				const std::string what = fmt::format("Cell {}", index + 1);
				if (!item.is_object())
				{
					error = fmt::format("{} must be an object", what);
					return fail();
				}
				Reader reader(item, what, error);
				WorldCell cell;
				if (!ReadLayerID(reader, "Layer", cell.Layer, error, what) || !ReadCoordinate(reader, "X", cell.X, error, what)
					|| !ReadCoordinate(reader, "Z", cell.Z, error, what) || !reader.Handle("Scene", cell.Scene, true))
					return fail();
				data.Cells.push_back(cell);
			}
		}
		if (!error.empty())
			return fail();

		if (const nlohmann::json* index = JsonUtils::Find(content, "Index"); index && !index->is_null())
		{
			if (!index->is_object())
			{
				error = "The world's \"Index\" must be an object";
				return fail();
			}
			Reader reader(*index, "The world's index", error);
			WorldIndex cooked;
			int64_t version = 0;
			if (!reader.Integer("Version", version, true))
				return fail();
			if (version != WorldIndex::c_Version)
			{
				error = fmt::format("The world's index has the unsupported version {} (expected {})", version, WorldIndex::c_Version);
				return fail();
			}
			if (!reader.Count("MaxBodiesInRadius", cooked.MaxBodiesInRadius) || !reader.Count("PersistentBodies", cooked.PersistentBodies))
				return fail();
			if (const nlohmann::json* summaries = reader.Array("Cells", c_MaxCells, false))
			{
				cooked.Cells.reserve(summaries->size());
				for (size_t position = 0; position < summaries->size(); position++)
				{
					const nlohmann::json& item = (*summaries)[position];
					const std::string what = fmt::format("Index cell {}", position + 1);
					if (!item.is_object())
					{
						error = fmt::format("{} must be an object", what);
						return fail();
					}
					Reader cellReader(item, what, error);
					WorldCellSummary summary;
					if (!ReadLayerID(cellReader, "Layer", summary.Layer, error, what) || !ReadCoordinate(cellReader, "X", summary.X, error, what)
						|| !ReadCoordinate(cellReader, "Z", summary.Z, error, what) || !cellReader.String("Stamp", summary.Stamp, false)
						|| !cellReader.Count("Entities", summary.Entities) || !cellReader.Count("Bodies", summary.Bodies)
						|| !cellReader.Count("ClosureBytes", summary.ClosureBytes))
						return fail();
					const nlohmann::json* bounds = JsonUtils::Find(item, "Bounds");
					if (!bounds || !bounds->is_object())
					{
						error = fmt::format("{}: \"Bounds\" must be an object with Min and Max", what);
						return fail();
					}
					Reader boundsReader(*bounds, fmt::format("{}'s bounds", what), error);
					if (!boundsReader.Vec3("Min", summary.BoundsMin) || !boundsReader.Vec3("Max", summary.BoundsMax))
						return fail();
					cooked.Cells.push_back(std::move(summary));
				}
			}
			if (!error.empty())
				return fail();
			data.Index = std::move(cooked);
		}

		if (!data.Validate(outError))
			return std::nullopt;
		return data;
	}

	bool WorldData::Validate(std::string* outError) const
	{
		if (Name.size() > 256 || HasControlCharacter(Name))
			return Fail(outError, "The world's name must have at most 256 characters and no control characters");
		if (!std::isfinite(GridOrigin.x) || !std::isfinite(GridOrigin.y) || std::abs(GridOrigin.x) > c_MaxExtent || std::abs(GridOrigin.y) > c_MaxExtent)
			return Fail(outError, fmt::format("The world's GridOrigin must lie within {} km of the origin", c_MaxExtent / 1000.0));

		if (Layers.empty() || Layers.size() > c_MaxLayers)
			return Fail(outError, fmt::format("A world has 1 to {} layers ({} given)", c_MaxLayers, Layers.size()));
		for (size_t index = 0; index < Layers.size(); index++)
		{
			const WorldLayer& layer = Layers[index];
			const std::string what = layer.Name.empty() || HasControlCharacter(layer.Name) ? fmt::format("Layer {}", index + 1) : fmt::format("Layer '{}'", layer.Name);
			if (layer.ID == 0)
				return Fail(outError, fmt::format("{}: the ID must be from 1 to 255", what));
			if (layer.Name.empty() || layer.Name.size() > c_MaxNameLength || HasControlCharacter(layer.Name))
				return Fail(outError, fmt::format("{}: the name must have 1 to {} characters and no control characters", what, c_MaxNameLength));
			if (layer.Name.find(',') != std::string::npos || layer.Name.front() == ' ' || layer.Name.back() == ' ')
				return Fail(outError, fmt::format("{}: the name must not contain commas or start or end with a space (sources list layers separated by commas)", what));
			if (!std::isfinite(layer.CellSize) || layer.CellSize < c_MinCellSize || layer.CellSize > c_MaxCellSize)
				return Fail(outError, fmt::format("{}: CellSize must be from {} to {} m", what, c_MinCellSize, c_MaxCellSize));
			if (!std::isfinite(layer.StreamingDistance) || layer.StreamingDistance < 0.0f || layer.StreamingDistance > c_MaxStreamingDistance)
				return Fail(outError, fmt::format("{}: StreamingDistance must be from 0 to {} m", what, c_MaxStreamingDistance));
			for (size_t other = 0; other < index; other++)
			{
				if (Layers[other].ID == layer.ID)
					return Fail(outError, fmt::format("{}: the ID {} is used by layer '{}' too", what, layer.ID, Layers[other].Name));
				if (Layers[other].Name == layer.Name)
					return Fail(outError, fmt::format("{}: another layer has the same name", what));
			}
		}

		if (Cells.size() > c_MaxCells)
			return Fail(outError, fmt::format("A world has at most {} cells", c_MaxCells));
		std::unordered_set<uint64_t> keys;
		std::unordered_set<UUID> scenes;
		keys.reserve(Cells.size());
		scenes.reserve(Cells.size());
		for (const WorldCell& cell : Cells)
		{
			const WorldLayer* layer = FindLayer(cell.Layer);
			if (!layer)
				return Fail(outError, fmt::format("Cell (layer {}, {}, {}): the world has no layer with that ID", cell.Layer, cell.X, cell.Z));
			const std::string what = DescribeCell(*this, cell.GetKey());
			if (std::abs(cell.X) > WorldCellKey::c_MaxCoordinate || std::abs(cell.Z) > WorldCellKey::c_MaxCoordinate)
				return Fail(outError, fmt::format("{}: it lies outside the world", what));
			const WorldCellSquare square = WorldGrid::CellBounds(*layer, GridOrigin, cell.X, cell.Z);
			const double farthest = std::max({ std::abs(square.Min.x), std::abs(square.Min.y), std::abs(square.Max.x), std::abs(square.Max.y) });
			if (farthest > c_MaxExtent)
				return Fail(outError, fmt::format("{}: it lies {:.0f} km from the origin, beyond the {:.0f} km a world may reach", what, farthest / 1000.0, c_MaxExtent / 1000.0));
			if (!keys.insert(cell.GetKey().Pack()).second)
				return Fail(outError, fmt::format("{}: the world lists it twice", what));
			if (!cell.Scene.IsValid())
				return Fail(outError, fmt::format("{}: it has no scene", what));
			if (cell.Scene == PersistentScene)
				return Fail(outError, fmt::format("{}: its scene is the world's persistent scene", what));
			if (!scenes.insert(cell.Scene).second)
				return Fail(outError, fmt::format("{}: its scene {} belongs to another cell too", what, cell.Scene.ToString()));
		}

		if (Index)
		{
			if (Index->Version != WorldIndex::c_Version)
				return Fail(outError, fmt::format("The world's index has the unsupported version {} (expected {})", Index->Version, WorldIndex::c_Version));
			if (Index->Cells.size() > Cells.size())
				return Fail(outError, "The world's index lists more cells than the world has");
			std::unordered_set<uint64_t> indexed;
			indexed.reserve(Index->Cells.size());
			for (const WorldCellSummary& summary : Index->Cells)
			{
				const std::string what = fmt::format("The index's {}", DescribeCell(*this, summary.GetKey()));
				if (std::abs(summary.X) > WorldCellKey::c_MaxCoordinate || std::abs(summary.Z) > WorldCellKey::c_MaxCoordinate || !keys.contains(summary.GetKey().Pack()))
					return Fail(outError, fmt::format("{}: the world has no such cell", what));
				if (!indexed.insert(summary.GetKey().Pack()).second)
					return Fail(outError, fmt::format("{}: the index lists it twice", what));
				for (int axis = 0; axis < 3; axis++)
				{
					if (!std::isfinite(summary.BoundsMin[axis]) || !std::isfinite(summary.BoundsMax[axis]) || summary.BoundsMin[axis] > summary.BoundsMax[axis])
						return Fail(outError, fmt::format("{}: its bounds must be finite, with Min at most Max", what));
				}
				if (summary.Stamp.size() > 256 || HasControlCharacter(summary.Stamp))
					return Fail(outError, fmt::format("{}: its stamp must have at most 256 characters and no control characters", what));
			}
		}
		return true;
	}

	nlohmann::json WorldData::ToJson() const
	{
		nlohmann::json layers = nlohmann::json::array();
		for (const WorldLayer& layer : Layers)
		{
			layers.push_back({
				{ "ID", layer.ID },
				{ "Name", layer.Name },
				{ "CellSize", layer.CellSize },
				{ "StreamingDistance", layer.StreamingDistance },
				{ "EnabledByDefault", layer.EnabledByDefault },
				{ "LoadBeforeStart", layer.LoadBeforeStart } });
		}
		nlohmann::json cells = nlohmann::json::array();
		for (const WorldCell& cell : Cells)
			cells.push_back({ { "Layer", cell.Layer }, { "X", cell.X }, { "Z", cell.Z }, { "Scene", UUIDToJson(cell.Scene) } });

		nlohmann::json world = {
			{ "Name", Name },
			{ "PersistentScene", PersistentScene.IsValid() ? UUIDToJson(PersistentScene) : nlohmann::json(nullptr) },
			{ "GridOrigin", { GridOrigin.x, GridOrigin.y } },
			{ "Layers", std::move(layers) },
			{ "Cells", std::move(cells) } };
		if (Index)
		{
			nlohmann::json summaries = nlohmann::json::array();
			for (const WorldCellSummary& summary : Index->Cells)
			{
				summaries.push_back({
					{ "Layer", summary.Layer },
					{ "X", summary.X },
					{ "Z", summary.Z },
					{ "Stamp", summary.Stamp },
					{ "Entities", summary.Entities },
					{ "Bodies", summary.Bodies },
					{ "Bounds", { { "Min", { summary.BoundsMin.x, summary.BoundsMin.y, summary.BoundsMin.z } }, { "Max", { summary.BoundsMax.x, summary.BoundsMax.y, summary.BoundsMax.z } } } },
					{ "ClosureBytes", summary.ClosureBytes } });
			}
			world["Index"] = {
				{ "Version", Index->Version },
				{ "MaxBodiesInRadius", Index->MaxBodiesInRadius },
				{ "PersistentBodies", Index->PersistentBodies },
				{ "Cells", std::move(summaries) } };
		}
		return world;
	}

	const WorldLayer* WorldData::FindLayer(uint8_t id) const
	{
		for (const WorldLayer& layer : Layers)
		{
			if (layer.ID == id)
				return &layer;
		}
		return nullptr;
	}

	const WorldLayer* WorldData::FindLayer(std::string_view name) const
	{
		for (const WorldLayer& layer : Layers)
		{
			if (layer.Name == name)
				return &layer;
		}
		return nullptr;
	}

	Ref<WorldAsset> WorldAsset::Create(WorldData data, std::string* outError)
	{
		if (!data.Validate(outError))
			return nullptr;
		Ref<WorldAsset> world = CreateRef<WorldAsset>();
		world->m_CellIndex.reserve(data.Cells.size());
		for (size_t index = 0; index < data.Cells.size(); index++)
			world->m_CellIndex.emplace(data.Cells[index].GetKey().Pack(), static_cast<int32_t>(index));
		if (data.Index)
		{
			world->m_SummaryIndex.reserve(data.Index->Cells.size());
			for (size_t index = 0; index < data.Index->Cells.size(); index++)
				world->m_SummaryIndex.emplace(data.Index->Cells[index].GetKey().Pack(), static_cast<int32_t>(index));
		}
		world->m_Data = std::move(data);
		return world;
	}

	Ref<WorldAsset> WorldAsset::FromJson(const nlohmann::json& document, std::string* outError)
	{
		const nlohmann::json* header = JsonUtils::Find(document, "Strata");
		if (!header || !header->is_object() || JsonUtils::GetString(*header, "Format") != c_DocumentKey)
		{
			Fail(outError, "Not a Strata world document");
			return nullptr;
		}
		const uint64_t version = JsonUtils::GetUInt(*header, "Version", 0);
		if (version == 0 || version > WorldData::c_FormatVersion)
		{
			Fail(outError, fmt::format("Unsupported world version {}", version));
			return nullptr;
		}
		const nlohmann::json* content = JsonUtils::Find(document, c_DocumentKey);
		if (!content || !content->is_object())
		{
			Fail(outError, "The document has no \"World\" object");
			return nullptr;
		}
		std::optional<WorldData> data = WorldData::FromJson(*content, outError);
		if (!data)
			return nullptr;
		return Create(std::move(*data), outError);
	}

	Ref<WorldAsset> WorldAsset::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		const std::optional<nlohmann::json> json = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), outError);
		if (!json)
			return nullptr;
		return FromJson(*json, outError);
	}

	nlohmann::json WorldAsset::ToDocument(const WorldData& data)
	{
		nlohmann::json document;
		document["Strata"] = { { "Format", c_DocumentKey }, { "Version", WorldData::c_FormatVersion } };
		document[std::string(c_DocumentKey)] = data.ToJson();
		return document;
	}

	AssetMemoryUsage WorldAsset::GetMemoryUsage() const
	{
		// Hash map nodes: the pair and about two pointers of bookkeeping each, plus the bucket array.
		constexpr uint64_t c_MapNodeBytes = sizeof(std::pair<const uint64_t, int32_t>) + 2 * sizeof(void*);
		uint64_t bytes = sizeof(WorldAsset) + m_Data.Name.capacity() + m_Data.Layers.capacity() * sizeof(WorldLayer) + m_Data.Cells.capacity() * sizeof(WorldCell);
		for (const WorldLayer& layer : m_Data.Layers)
			bytes += layer.Name.capacity();
		if (m_Data.Index)
		{
			bytes += m_Data.Index->Cells.capacity() * sizeof(WorldCellSummary);
			for (const WorldCellSummary& summary : m_Data.Index->Cells)
				bytes += summary.Stamp.capacity();
		}
		bytes += (m_CellIndex.size() + m_SummaryIndex.size()) * c_MapNodeBytes + (m_CellIndex.bucket_count() + m_SummaryIndex.bucket_count()) * sizeof(void*);
		return AssetMemoryUsage { bytes };
	}

	int32_t WorldAsset::FindCell(const WorldCellKey& key) const
	{
		if (std::abs(key.X) > WorldCellKey::c_MaxCoordinate || std::abs(key.Z) > WorldCellKey::c_MaxCoordinate)
			return -1;
		const auto it = m_CellIndex.find(key.Pack());
		return it != m_CellIndex.end() ? it->second : -1;
	}

	const WorldCellSummary* WorldAsset::FindSummary(const WorldCellKey& key) const
	{
		if (!m_Data.Index || std::abs(key.X) > WorldCellKey::c_MaxCoordinate || std::abs(key.Z) > WorldCellKey::c_MaxCoordinate)
			return nullptr;
		const auto it = m_SummaryIndex.find(key.Pack());
		return it != m_SummaryIndex.end() ? &m_Data.Index->Cells[static_cast<size_t>(it->second)] : nullptr;
	}

}
