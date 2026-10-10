#include "Perf/StressProject.h"

#include "Renderer/GPUTestUtils.h"
#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Timer.h"
#include "Strata/Math/Math.h"
#include "Strata/Project/Project.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <spdlog/fmt/fmt.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <numbers>
#include <span>

namespace Strata::Tests
{

	namespace
	{

		// Fixed handles, so that every run (and every machine) produces the same project and pack.
		constexpr uint64_t c_TextureHandleBase = 0x5A00000000001000ull;
		constexpr uint64_t c_MaterialHandleBase = 0x5A00000000002000ull;
		constexpr uint64_t c_GridModelHandle = 0x5A00000000003000ull;
		constexpr uint64_t c_WorldSceneHandle = 0x5A00000000004000ull;
		constexpr uint64_t c_SceneEntityHandleBase = 0x5AE0000000000000ull; // Camera, sun, terrain
		constexpr uint64_t c_ObjectHandleBase = 0x5AF0000000000010ull;

		// Terrain hills: amplitude in world units and one undulation every c_TerrainWavelength units.
		constexpr float c_TerrainAmplitude = 4.0f;
		constexpr float c_TerrainWavelength = 400.0f;
		constexpr float c_ObjectScale = 4.0f; // Edge of the cubes (diameter of the spheres)

		float GetTerrainHeight(float x, float z)
		{
			const float frequency = 2.0f * std::numbers::pi_v<float> / c_TerrainWavelength;
			return c_TerrainAmplitude * std::sin(x * frequency) * std::cos(z * frequency);
		}

		glm::vec3 GetTerrainNormal(float x, float z)
		{
			const float frequency = 2.0f * std::numbers::pi_v<float> / c_TerrainWavelength;
			const float slopeX = c_TerrainAmplitude * frequency * std::cos(x * frequency) * std::cos(z * frequency);
			const float slopeZ = -c_TerrainAmplitude * frequency * std::sin(x * frequency) * std::sin(z * frequency);
			return glm::normalize(glm::vec3(-slopeX, 1.0f, -slopeZ));
		}

		// Districts: a grid of columns x rows cells over the world, as square as the material count allows; cell
		// (column, row) has material (row * columns + column) modulo the material count.
		struct DistrictGrid
		{
			uint32_t Columns = 1;
			uint32_t Rows = 1;
		};

		DistrictGrid GetDistrictGrid(const StressProjectSpec& spec)
		{
			const uint32_t materials = std::max(1u, spec.Materials);
			DistrictGrid grid;
			grid.Columns = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(materials))));
			grid.Rows = (materials + grid.Columns - 1) / grid.Columns;
			return grid;
		}

		uint32_t GetDistrictMaterial(const StressProjectSpec& spec, const DistrictGrid& grid, float x, float z)
		{
			const auto cell = [](float coordinate, uint32_t cells)
			{
				const float normalized = (coordinate + c_StressWorldSize * 0.5f) / c_StressWorldSize;
				return std::min(cells - 1, static_cast<uint32_t>(std::max(0.0f, normalized) * static_cast<float>(cells)));
			};
			return (cell(z, grid.Rows) * grid.Columns + cell(x, grid.Columns)) % std::max(1u, spec.Materials);
		}

		glm::vec3 GetDistrictCenter(const DistrictGrid& grid, uint32_t column, uint32_t row)
		{
			const float width = c_StressWorldSize / static_cast<float>(grid.Columns);
			const float depth = c_StressWorldSize / static_cast<float>(grid.Rows);
			const float x = -c_StressWorldSize * 0.5f + (static_cast<float>(column) + 0.5f) * width;
			const float z = -c_StressWorldSize * 0.5f + (static_cast<float>(row) + 0.5f) * depth;
			return glm::vec3(x, GetTerrainHeight(x, z), z);
		}

		// The point the camera looks at when it holds at a stop: the stops are spread evenly along a serpentine walk over
		// the district cells (row by row, alternating direction), from the first cell to the last.
		glm::vec3 GetStopTarget(const StressProjectSpec& spec, const StressSweepPath& path, uint32_t stop)
		{
			const DistrictGrid grid = GetDistrictGrid(spec);
			const uint32_t cells = grid.Columns * grid.Rows;
			const uint32_t stops = std::max(1u, path.Stops);
			const uint32_t walkIndex = stops == 1 ? 0
				: static_cast<uint32_t>(std::lround(static_cast<double>(stop) * static_cast<double>(cells - 1) / static_cast<double>(stops - 1)));
			const uint32_t row = walkIndex / grid.Columns;
			const uint32_t step = walkIndex % grid.Columns;
			const uint32_t column = row % 2 == 0 ? step : grid.Columns - 1 - step;
			return GetDistrictCenter(grid, column, row);
		}

		// xorshift32: deterministic noise, the same on every platform.
		uint32_t NextRandom(uint32_t& state)
		{
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			return state;
		}

		// RGB noise over a gradient: the gradient differs per texture (seed), the noise keeps the PNG from compressing to
		// nothing and the cooked mips from being trivial.
		std::vector<uint8_t> EncodeStressPNG(uint32_t size, uint32_t seed)
		{
			std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 3);
			for (uint32_t y = 0; y < size; y++)
			{
				uint32_t random = 0x9E3779B9u ^ (seed * 0x85EBCA6Bu) ^ (y * 0xC2B2AE35u);
				if (random == 0)
					random = 1;
				uint8_t* row = pixels.data() + static_cast<size_t>(y) * size * 3;
				for (uint32_t x = 0; x < size; x++)
				{
					const uint32_t noise = NextRandom(random);
					const uint32_t gradientX = static_cast<uint32_t>(static_cast<uint64_t>(x) * 256 / size);
					const uint32_t gradientY = static_cast<uint32_t>(static_cast<uint64_t>(y) * 256 / size);
					row[x * 3 + 0] = static_cast<uint8_t>(((gradientX + seed * 53) & 0xFF) ^ (noise & 0x1F));
					row[x * 3 + 1] = static_cast<uint8_t>(((gradientY + seed * 97) & 0xFF) ^ ((noise >> 8) & 0x1F));
					row[x * 3 + 2] = static_cast<uint8_t>((((gradientX + gradientY) / 2 + seed * 29) & 0xFF) ^ ((noise >> 16) & 0x1F));
				}
			}

			std::vector<uint8_t> encoded;
			auto write = [](void* context, void* data, int bytes)
			{
				auto* output = static_cast<std::vector<uint8_t>*>(context);
				output->insert(output->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + bytes);
			};
			if (!stbi_write_png_to_func(write, &encoded, static_cast<int>(size), static_cast<int>(size), 3, pixels.data(), static_cast<int>(size * 3)))
				return {};
			return encoded;
		}

		// "<prefix>_<two-digit index><extension>"
		std::filesystem::path NumberedFileName(std::string_view prefix, uint32_t index, std::string_view extension)
		{
			return FileSystem::FromUTF8(fmt::format("{}_{:02}{}", prefix, index, extension));
		}

		bool WriteJson(const std::filesystem::path& path, const nlohmann::json& json)
		{
			return FileSystem::WriteText(path, JsonUtils::Dump(json, 1, '\t') + "\n");
		}

		// The sidecar the editor would write: the handle, the type and the importer's default settings.
		bool WriteMeta(const std::filesystem::path& path, AssetHandle handle, std::string& error)
		{
			const AssetImporter* importer = AssetImporterRegistry::FindByExtension(FileSystem::ToUTF8(path.extension()));
			if (!importer)
			{
				error = fmt::format("No importer for '{}'", FileSystem::ToUTF8(path));
				return false;
			}
			nlohmann::json meta = nlohmann::json::object();
			meta["Strata"] = { { "Format", "AssetMeta" }, { "Version", 1 } };
			meta["Handle"] = UUIDToJson(handle);
			meta["Type"] = AssetTypeToString(importer->GetType());
			meta["ImportSettings"] = importer->GetDefaultSettings(path);
			std::filesystem::path metaPath = path;
			metaPath += ".meta";
			if (!WriteJson(metaPath, meta))
			{
				error = fmt::format("Could not write '{}'", FileSystem::ToUTF8(metaPath));
				return false;
			}
			return true;
		}

		bool WriteAsset(const std::filesystem::path& path, std::span<const uint8_t> data, AssetHandle handle, std::string& error)
		{
			if (!FileSystem::WriteBytes(path, data))
			{
				error = fmt::format("Could not write '{}'", FileSystem::ToUTF8(path));
				return false;
			}
			return WriteMeta(path, handle, error);
		}

		bool WriteJsonAsset(const std::filesystem::path& path, const nlohmann::json& json, AssetHandle handle, std::string& error)
		{
			const std::string text = JsonUtils::Dump(json, 1, '\t') + "\n";
			return WriteAsset(path, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()), handle, error);
		}

		// The terrain: a grid of resolution^2 vertices over the whole world, positions in world units, as glTF with an
		// external buffer (positions, normals, texture coordinates, then 32-bit indices).
		bool WriteGridModel(const std::filesystem::path& modelsDirectory, uint32_t resolution, AssetHandle handle, std::string& error)
		{
			const size_t vertexCount = static_cast<size_t>(resolution) * resolution;
			std::vector<float> positions;
			std::vector<float> normals;
			std::vector<float> uvs;
			positions.reserve(vertexCount * 3);
			normals.reserve(vertexCount * 3);
			uvs.reserve(vertexCount * 2);
			const float lastIndex = static_cast<float>(resolution - 1);
			const float spacing = c_StressWorldSize / lastIndex;
			for (uint32_t row = 0; row < resolution; row++)
			{
				for (uint32_t column = 0; column < resolution; column++)
				{
					const float x = -c_StressWorldSize * 0.5f + static_cast<float>(column) * spacing;
					const float z = -c_StressWorldSize * 0.5f + static_cast<float>(row) * spacing;
					const glm::vec3 normal = GetTerrainNormal(x, z);
					positions.insert(positions.end(), { x, GetTerrainHeight(x, z), z });
					normals.insert(normals.end(), { normal.x, normal.y, normal.z });
					uvs.insert(uvs.end(), { static_cast<float>(column) / lastIndex, static_cast<float>(row) / lastIndex });
				}
			}
			// Counter-clockwise seen from above (+Y), the glTF front face.
			std::vector<uint32_t> indices;
			indices.reserve(static_cast<size_t>(resolution - 1) * (resolution - 1) * 6);
			for (uint32_t row = 0; row + 1 < resolution; row++)
			{
				for (uint32_t column = 0; column + 1 < resolution; column++)
				{
					const uint32_t i0 = row * resolution + column;
					const uint32_t i1 = i0 + 1;
					const uint32_t i2 = i0 + resolution;
					const uint32_t i3 = i2 + 1;
					indices.insert(indices.end(), { i0, i2, i1, i1, i2, i3 });
				}
			}

			const size_t positionBytes = positions.size() * sizeof(float);
			const size_t normalBytes = normals.size() * sizeof(float);
			const size_t uvBytes = uvs.size() * sizeof(float);
			const size_t indexBytes = indices.size() * sizeof(uint32_t);
			std::vector<uint8_t> buffer(positionBytes + normalBytes + uvBytes + indexBytes);
			std::memcpy(buffer.data(), positions.data(), positionBytes);
			std::memcpy(buffer.data() + positionBytes, normals.data(), normalBytes);
			std::memcpy(buffer.data() + positionBytes + normalBytes, uvs.data(), uvBytes);
			std::memcpy(buffer.data() + positionBytes + normalBytes + uvBytes, indices.data(), indexBytes);
			if (!FileSystem::WriteBytes(modelsDirectory / "Grid.bin", buffer))
			{
				error = "Could not write the terrain's buffer";
				return false;
			}

			constexpr int c_ArrayBuffer = 34962;
			constexpr int c_ElementArrayBuffer = 34963;
			constexpr int c_Float = 5126;
			constexpr int c_UnsignedInt = 5125;
			const float half = c_StressWorldSize * 0.5f;
			nlohmann::json gltf = nlohmann::json::object();
			gltf["asset"] = { { "version", "2.0" } };
			gltf["scene"] = 0;
			gltf["scenes"] = nlohmann::json::array({ { { "nodes", { 0 } } } });
			gltf["nodes"] = nlohmann::json::array({ { { "mesh", 0 }, { "name", "Grid" } } });
			gltf["meshes"] = nlohmann::json::array({ {
				{ "name", "Grid" },
				{ "primitives", nlohmann::json::array({ { { "attributes", { { "POSITION", 0 }, { "NORMAL", 1 }, { "TEXCOORD_0", 2 } } }, { "indices", 3 } } }) }
			} });
			gltf["buffers"] = nlohmann::json::array({ { { "uri", "Grid.bin" }, { "byteLength", buffer.size() } } });
			gltf["bufferViews"] = nlohmann::json::array({
				{ { "buffer", 0 }, { "byteOffset", 0 }, { "byteLength", positionBytes }, { "target", c_ArrayBuffer } },
				{ { "buffer", 0 }, { "byteOffset", positionBytes }, { "byteLength", normalBytes }, { "target", c_ArrayBuffer } },
				{ { "buffer", 0 }, { "byteOffset", positionBytes + normalBytes }, { "byteLength", uvBytes }, { "target", c_ArrayBuffer } },
				{ { "buffer", 0 }, { "byteOffset", positionBytes + normalBytes + uvBytes }, { "byteLength", indexBytes }, { "target", c_ElementArrayBuffer } }
			});
			gltf["accessors"] = nlohmann::json::array({
				{ { "bufferView", 0 }, { "componentType", c_Float }, { "count", vertexCount }, { "type", "VEC3" },
					{ "min", { -half, -c_TerrainAmplitude, -half } }, { "max", { half, c_TerrainAmplitude, half } } },
				{ { "bufferView", 1 }, { "componentType", c_Float }, { "count", vertexCount }, { "type", "VEC3" } },
				{ { "bufferView", 2 }, { "componentType", c_Float }, { "count", vertexCount }, { "type", "VEC2" } },
				{ { "bufferView", 3 }, { "componentType", c_UnsignedInt }, { "count", indices.size() }, { "type", "SCALAR" } }
			});
			return WriteJsonAsset(modelsDirectory / "Grid.gltf", gltf, handle, error);
		}

		nlohmann::json CreateWorldScene(const StressProjectSpec& spec, const StressProject& project)
		{
			Scene scene("World");

			// The overview of the streaming audit: from above the south edge, the whole world in view.
			Entity camera = scene.CreateEntityWithUUID(UUID(c_SceneEntityHandleBase + 1), "Camera");
			TransformComponent& cameraTransform = camera.GetComponent<TransformComponent>();
			cameraTransform.Translation = glm::vec3(0.0f, 60.0f, 260.0f);
			cameraTransform.Rotation = glm::angleAxis(glm::radians(-20.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			CameraComponent& cameraComponent = camera.AddComponent<CameraComponent>();
			cameraComponent.PerspectiveFar = 5000.0f;

			Entity sun = scene.CreateEntityWithUUID(UUID(c_SceneEntityHandleBase + 2), "Sun");
			sun.GetComponent<TransformComponent>().Rotation = Math::LookRotation(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f)));
			DirectionalLightComponent& light = sun.AddComponent<DirectionalLightComponent>();
			light.Intensity = 4.0f;
			light.CastShadows = true;

			Entity terrain = scene.CreateEntityWithUUID(UUID(c_SceneEntityHandleBase + 3), "Terrain");
			MeshRendererComponent& terrainRenderer = terrain.AddComponent<MeshRendererComponent>();
			terrainRenderer.Mesh = project.GridMesh;
			terrainRenderer.CastShadows = false;

			// Objects on an even grid over the world, each with the material of its district.
			const DistrictGrid districts = GetDistrictGrid(spec);
			const uint32_t side = std::max(1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(spec.Entities)))));
			const float spacing = c_StressWorldSize / static_cast<float>(side);
			for (uint32_t index = 0; index < spec.Entities; index++)
			{
				const float x = -c_StressWorldSize * 0.5f + (static_cast<float>(index % side) + 0.5f) * spacing;
				const float z = -c_StressWorldSize * 0.5f + (static_cast<float>(index / side) + 0.5f) * spacing;
				Entity object = scene.CreateEntityWithUUID(UUID(c_ObjectHandleBase + index), fmt::format("Object {}", index));
				TransformComponent& transform = object.GetComponent<TransformComponent>();
				transform.Translation = glm::vec3(x, GetTerrainHeight(x, z) + c_ObjectScale * 0.5f, z);
				transform.Scale = glm::vec3(c_ObjectScale);
				MeshRendererComponent& renderer = object.AddComponent<MeshRendererComponent>();
				renderer.Mesh = index % 2 == 0 ? BuiltinAssets::CubeMesh : BuiltinAssets::SphereMesh;
				renderer.Material = project.Materials[GetDistrictMaterial(spec, districts, x, z)];
			}
			return SceneSerializer::Serialize(scene);
		}

		// Reads an unsigned member of a JSON object; false (with the reason in error) when it is missing or not one.
		bool ReadUInt(const nlohmann::json& object, std::string_view key, uint64_t& outValue, std::string& error)
		{
			const nlohmann::json* value = JsonUtils::Find(object, key);
			if (!value || !value->is_number_unsigned())
			{
				error = fmt::format("'{}' is missing or not an unsigned number", key);
				return false;
			}
			outValue = value->get<uint64_t>();
			return true;
		}

		bool ReadUInt32(const nlohmann::json& object, std::string_view key, uint32_t& outValue, std::string& error)
		{
			uint64_t value = 0;
			if (!ReadUInt(object, key, value, error))
				return false;
			if (value > std::numeric_limits<uint32_t>::max())
			{
				error = fmt::format("'{}' is out of range", key);
				return false;
			}
			outValue = static_cast<uint32_t>(value);
			return true;
		}

		bool ReadFloat(const nlohmann::json& object, std::string_view key, float& outValue, std::string& error)
		{
			const nlohmann::json* value = JsonUtils::Find(object, key);
			if (!value || !value->is_number())
			{
				error = fmt::format("'{}' is missing or not a number", key);
				return false;
			}
			outValue = value->get<float>();
			return true;
		}

		template<typename T>
		bool ReadNumbers(const nlohmann::json& object, std::string_view key, std::vector<T>& outValues, std::string& error)
		{
			const nlohmann::json* values = JsonUtils::Find(object, key);
			if (!values || !values->is_array())
			{
				error = fmt::format("'{}' is missing or not an array", key);
				return false;
			}
			outValues.clear();
			outValues.reserve(values->size());
			for (const nlohmann::json& value : *values)
			{
				if (!value.is_number())
				{
					error = fmt::format("'{}' holds something other than numbers", key);
					return false;
				}
				outValues.push_back(value.get<T>());
			}
			return true;
		}

		// Makes the sweep's manager the active one for as long as it lives.
		struct ScopedActiveAssetManager
		{
			explicit ScopedActiveAssetManager(const Ref<AssetManagerBase>& manager)
			{
				AssetManager::SetActive(manager);
			}

			~ScopedActiveAssetManager()
			{
				AssetManager::SetActive(nullptr);
			}

			ScopedActiveAssetManager(const ScopedActiveAssetManager&) = delete;
			ScopedActiveAssetManager& operator=(const ScopedActiveAssetManager&) = delete;
		};

	}

	uint64_t GetStressTextureBytes(uint32_t size)
	{
		uint64_t bytes = 0;
		for (uint32_t level = size; level > 0; level /= 2)
			bytes += static_cast<uint64_t>(level) * level * 4;
		return bytes;
	}

	std::optional<StressProject> WriteStressProject(const std::filesystem::path& directory, const StressProjectSpec& spec, std::string& error)
	{
		const uint32_t textureCount = spec.Textures2k + spec.Textures4k;
		if (textureCount == 0 || spec.Materials == 0 || spec.GridResolution < 2 || spec.SmallTextureSize == 0 || spec.LargeTextureSize == 0)
		{
			error = "A stress project needs textures, materials, a grid of at least 2x2 vertices and texture sizes above 0";
			return std::nullopt;
		}

		Ref<Project> projectFile = Project::Create(directory, "Stress", &error);
		if (!projectFile)
			return std::nullopt;

		StressProject project;
		project.Directory = projectFile->GetProjectDirectory();
		project.WorldScene = UUID(c_WorldSceneHandle);
		project.GridModel = UUID(c_GridModelHandle);
		// The importer names a model's meshes "Mesh/<index>" (GltfImporter); the grid has one.
		project.GridMesh = DeriveSubAssetHandle(project.GridModel, "Mesh/0");
		const std::filesystem::path assets = projectFile->GetAssetDirectory();
		for (const char* subdirectory : { "Textures", "Materials", "Models", "Scenes" })
		{
			if (!FileSystem::CreateDirectories(assets / subdirectory))
			{
				error = fmt::format("Could not create '{}'", FileSystem::ToUTF8(assets / subdirectory));
				return std::nullopt;
			}
		}

		// Textures, encoded in parallel when the job system runs (PNG compression is the slow part).
		std::vector<std::filesystem::path> texturePaths(textureCount);
		for (uint32_t index = 0; index < textureCount; index++)
		{
			const bool large = index >= spec.Textures2k;
			texturePaths[index] = assets / "Textures" / (large ? NumberedFileName("Large", index - spec.Textures2k, ".png") : NumberedFileName("Small", index, ".png"));
			project.Textures.push_back(UUID(c_TextureHandleBase + index));
			const uint64_t bytes = GetStressTextureBytes(large ? spec.LargeTextureSize : spec.SmallTextureSize);
			project.TextureBytes += bytes;
			project.LargestTextureBytes = std::max(project.LargestTextureBytes, bytes);
		}
		std::mutex errorMutex;
		std::string textureError;
		JobSystem::ParallelFor(textureCount, 1, [&](uint32_t begin, uint32_t end)
		{
			for (uint32_t index = begin; index < end; index++)
			{
				const uint32_t size = index >= spec.Textures2k ? spec.LargeTextureSize : spec.SmallTextureSize;
				const std::vector<uint8_t> png = EncodeStressPNG(size, index + 1);
				std::string writeError = png.empty() ? fmt::format("Could not encode texture {}", index) : std::string();
				if (writeError.empty())
					WriteAsset(texturePaths[index], png, project.Textures[index], writeError);
				if (!writeError.empty())
				{
					std::scoped_lock<std::mutex> lock(errorMutex);
					textureError = std::move(writeError);
				}
			}
		});
		if (!textureError.empty())
		{
			error = textureError;
			return std::nullopt;
		}

		for (uint32_t index = 0; index < spec.Materials; index++)
		{
			MaterialProperties properties;
			properties.Roughness = 0.6f;
			properties.BaseColorMap = project.Textures[index % textureCount];
			const AssetHandle handle = UUID(c_MaterialHandleBase + index);
			if (!WriteJsonAsset(assets / "Materials" / NumberedFileName("District", index, ".stmat"), Material::Create(properties)->Serialize(), handle, error))
				return std::nullopt;
			project.Materials.push_back(handle);
		}

		if (!WriteGridModel(assets / "Models", spec.GridResolution, project.GridModel, error))
			return std::nullopt;
		if (!WriteJsonAsset(assets / "Scenes" / "World.stscene", CreateWorldScene(spec, project), project.WorldScene, error))
			return std::nullopt;

		projectFile->GetConfig().StartScene = project.WorldScene;
		if (!projectFile->Save(&error))
			return std::nullopt;
		return project;
	}

	bool BuildStressPack(const StressProject& project, const std::filesystem::path& packPath, std::string& error)
	{
		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = project.Directory / "Assets";
		specification.CacheDirectory = project.Directory / ".strata" / "Cache";
		specification.WatchFiles = false;
		Ref<EditorAssetManager> manager = CreateRef<EditorAssetManager>(specification);
		manager->Scan();

		std::vector<AssetHandle> sources = project.Textures;
		sources.insert(sources.end(), project.Materials.begin(), project.Materials.end());
		sources.push_back(project.GridModel);
		sources.push_back(project.WorldScene);
		for (AssetHandle source : sources)
		{
			const AssetImportInfo import = manager->GetImportInfo(source);
			if (!manager->IsHandleValid(source) || !import.Imported || !import.Error.empty() || !import.Warnings.empty())
			{
				const std::optional<AssetMetadata> metadata = manager->GetMetadata(source);
				error = fmt::format("'{}' did not import cleanly: {}", metadata ? metadata->Path : source.ToString(),
					!import.Error.empty() ? import.Error : (import.Warnings.empty() ? std::string("not imported") : import.Warnings.front()));
				return false;
			}
		}
		if (!manager->IsHandleValid(project.GridMesh))
		{
			error = "The terrain grid has no mesh";
			return false;
		}
		return manager->BuildAssetPack(packPath, &error);
	}

	StressSweepFrame GetStressSweepFrame(const StressProjectSpec& spec, const StressSweepPath& path, uint32_t frame, float aspectRatio)
	{
		const uint32_t stops = std::max(1u, path.Stops);
		const uint32_t segmentFrames = std::max(1u, path.MoveFrames + path.HoldFrames);
		const uint32_t segment = std::min(frame / segmentFrames, stops - 1);
		const uint32_t frameInSegment = frame - std::min(frame, segment * segmentFrames);

		StressSweepFrame sweep;
		sweep.Stop = segment;
		glm::vec3 target = GetStopTarget(spec, path, segment);
		if (segment == 0)
		{
			// The sweep starts at rest above the first stop.
			sweep.Holding = true;
			sweep.FramesHolding = frameInSegment;
		}
		else if (frameInSegment < path.MoveFrames)
		{
			// Smoothstep easing: the camera leaves the previous stop and arrives at this one at rest.
			const float t = static_cast<float>(frameInSegment) / static_cast<float>(path.MoveFrames);
			target = glm::mix(GetStopTarget(spec, path, segment - 1), target, t * t * (3.0f - 2.0f * t));
		}
		else
		{
			sweep.Holding = true;
			sweep.FramesHolding = frameInSegment - path.MoveFrames;
		}

		// Looking down at the target (toward -Z) from Height above the ground.
		const float pitch = glm::radians(std::clamp(path.PitchDegrees, 1.0f, 89.0f));
		const glm::vec3 position = target + glm::vec3(0.0f, path.Height, path.Height / std::tan(pitch));
		constexpr float c_NearClip = 0.5f;
		SceneCamera& camera = sweep.Camera;
		camera.View = glm::lookAt(position, target, glm::vec3(0.0f, 1.0f, 0.0f));
		camera.VerticalFOV = glm::radians(path.VerticalFOVDegrees);
		camera.Projection = Math::PerspectiveReverseZ(camera.VerticalFOV, aspectRatio, c_NearClip, path.FarClip);
		camera.Position = position;
		camera.Near = c_NearClip;
		camera.Far = path.FarClip;
		return sweep;
	}

	StreamingSweepResult RunStreamingSweep(GraphicsDevice& device, const StreamingSweepSettings& settings)
	{
		StreamingSweepResult result;
		result.ValidationEnabled = device.GetInfo().ValidationEnabled;
		result.TextureBudget = settings.TextureBudget;
		if (!Renderer::IsInitialized() || settings.Width == 0 || settings.Height == 0)
		{
			result.Error = "The sweep needs an initialized renderer and a viewport";
			return result;
		}
		const uint32_t errorsBefore = device.GetErrorCount();

		std::string error;
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(settings.Pack, &error);
		if (!manager)
		{
			result.Error = fmt::format("Could not open the pack: {}", error);
			return result;
		}
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.GpuTextures = settings.TextureBudget;
		manager->SetResidencyBudgets(budgets);
		ScopedActiveAssetManager active(manager);

		Ref<Scene> scene;
		{
			// Only the scene's entities are kept: the scene document is an asset like any other.
			Ref<SceneAsset> sceneAsset = AssetManager::LoadAssetSync<SceneAsset>(settings.Scene);
			if (!sceneAsset)
			{
				result.Error = fmt::format("Could not load the scene: {}", manager->GetAssetError(settings.Scene));
				return result;
			}
			scene = sceneAsset->CreateScene(&error);
			if (!scene)
			{
				result.Error = fmt::format("The scene is invalid: {}", error);
				return result;
			}
		}

		{
			SceneRenderer renderer;
			renderer.SetViewportSize(settings.Width, settings.Height);
			const float aspectRatio = static_cast<float>(settings.Width) / static_cast<float>(settings.Height);
			const uint32_t frames = settings.Path.GetFrameCount();
			result.FramesToSettle.assign(std::max(1u, settings.Path.Stops), c_StopNeverSettled);
			result.ResidentTextureBytes.reserve(frames);
			result.PendingAssets.reserve(frames);
			result.FinalizeMs.reserve(frames);
			result.PrivateBytes.reserve(frames);
			result.DeviceBytes.reserve(frames);
			FramePacer pacer(settings.FrameRate);
			for (uint32_t frame = 0; frame < frames; frame++)
			{
				pacer.WaitForNextFrame();
				if (!device.BeginFrame())
				{
					result.Error = fmt::format("Frame {} could not begin (device lost?)", frame);
					break;
				}
				Renderer::BeginFrame();
				manager->Update();
				const AssetManagerStats stats = manager->GetStats();
				const StressSweepFrame sweep = GetStressSweepFrame(settings.Spec, settings.Path, frame, aspectRatio);
				const bool rendered = renderer.Render(*scene, sweep.Camera);
				device.EndFrame();
				if (!rendered)
				{
					result.Error = fmt::format("Frame {} was not rendered", frame);
					break;
				}

				const uint32_t pending = renderer.GetStats().PendingAssets;
				const std::optional<ProcessMemoryInfo> memory = Platform::GetProcessMemory();
				result.ResidentTextureBytes.push_back(stats.Resident.GpuTextures);
				result.PendingAssets.push_back(pending);
				result.FinalizeMs.push_back(stats.FinalizeMsLastFrame);
				result.PrivateBytes.push_back(memory ? memory->PrivateBytes : 0);
				result.DeviceBytes.push_back(device.GetMemoryBudget().Usage);
				result.MaxResidentTextureBytes = std::max(result.MaxResidentTextureBytes, stats.Resident.GpuTextures);
				result.MaxUploadedBytes = std::max(result.MaxUploadedBytes, stats.UploadedBytesLastFrame);
				result.MaxFinalizeMs = std::max(result.MaxFinalizeMs, stats.FinalizeMsLastFrame);
				if (sweep.Holding && pending == 0 && sweep.Stop < result.FramesToSettle.size() && result.FramesToSettle[sweep.Stop] == c_StopNeverSettled)
					result.FramesToSettle[sweep.Stop] = sweep.FramesHolding;
				result.Frames++;
			}
			device.WaitForIdle();
		}

		const AssetManagerStats stats = manager->GetStats();
		result.FailedAssets = stats.FailedAssets;
		result.LoadsCompleted = stats.TotalLoadsCompleted;
		result.Evictions = stats.Evictions;
		result.StagingReleases = stats.StagingReleases;
		result.MaxInFlightBytes = stats.InFlightBytesHighWater;
		if (const std::optional<ProcessMemoryInfo> memory = Platform::GetProcessMemory())
		{
			result.PeakPrivateBytes = memory->PeakPrivateBytes;
			result.PeakWorkingSetBytes = memory->PeakWorkingSet;
		}
		result.NewErrors = device.GetErrorCount() - errorsBefore;
		result.Completed = result.Error.empty() && result.Frames == settings.Path.GetFrameCount();
		return result;
	}

	nlohmann::json ToJson(const StreamingSweepSettings& settings)
	{
		nlohmann::json json = nlohmann::json::object();
		json["Pack"] = FileSystem::ToUTF8(settings.Pack);
		json["Scene"] = static_cast<uint64_t>(settings.Scene);
		json["TextureBudget"] = settings.TextureBudget;
		json["Width"] = settings.Width;
		json["Height"] = settings.Height;
		json["FrameRate"] = settings.FrameRate;
		json["Spec"] = {
			{ "Textures2k", settings.Spec.Textures2k }, { "Textures4k", settings.Spec.Textures4k }, { "Materials", settings.Spec.Materials },
			{ "Entities", settings.Spec.Entities }, { "GridResolution", settings.Spec.GridResolution },
			{ "SmallTextureSize", settings.Spec.SmallTextureSize }, { "LargeTextureSize", settings.Spec.LargeTextureSize }
		};
		json["Path"] = {
			{ "Stops", settings.Path.Stops }, { "MoveFrames", settings.Path.MoveFrames }, { "HoldFrames", settings.Path.HoldFrames },
			{ "Height", settings.Path.Height }, { "PitchDegrees", settings.Path.PitchDegrees },
			{ "VerticalFOVDegrees", settings.Path.VerticalFOVDegrees }, { "FarClip", settings.Path.FarClip }
		};
		return json;
	}

	std::optional<StreamingSweepSettings> StreamingSweepSettingsFromJson(const nlohmann::json& json, std::string& error)
	{
		StreamingSweepSettings settings;
		const nlohmann::json* spec = JsonUtils::Find(json, "Spec");
		const nlohmann::json* path = JsonUtils::Find(json, "Path");
		const nlohmann::json* pack = JsonUtils::Find(json, "Pack");
		if (!spec || !path || !pack || !pack->is_string())
		{
			error = "The settings need a Pack, a Spec and a Path";
			return std::nullopt;
		}
		settings.Pack = FileSystem::FromUTF8(pack->get<std::string>());
		uint64_t scene = 0;
		const bool valid = ReadUInt(json, "Scene", scene, error) && ReadUInt(json, "TextureBudget", settings.TextureBudget, error)
			&& ReadUInt32(json, "Width", settings.Width, error) && ReadUInt32(json, "Height", settings.Height, error)
			&& ReadUInt32(json, "FrameRate", settings.FrameRate, error)
			&& ReadUInt32(*spec, "Textures2k", settings.Spec.Textures2k, error) && ReadUInt32(*spec, "Textures4k", settings.Spec.Textures4k, error)
			&& ReadUInt32(*spec, "Materials", settings.Spec.Materials, error) && ReadUInt32(*spec, "Entities", settings.Spec.Entities, error)
			&& ReadUInt32(*spec, "GridResolution", settings.Spec.GridResolution, error)
			&& ReadUInt32(*spec, "SmallTextureSize", settings.Spec.SmallTextureSize, error)
			&& ReadUInt32(*spec, "LargeTextureSize", settings.Spec.LargeTextureSize, error)
			&& ReadUInt32(*path, "Stops", settings.Path.Stops, error) && ReadUInt32(*path, "MoveFrames", settings.Path.MoveFrames, error)
			&& ReadUInt32(*path, "HoldFrames", settings.Path.HoldFrames, error) && ReadFloat(*path, "Height", settings.Path.Height, error)
			&& ReadFloat(*path, "PitchDegrees", settings.Path.PitchDegrees, error)
			&& ReadFloat(*path, "VerticalFOVDegrees", settings.Path.VerticalFOVDegrees, error) && ReadFloat(*path, "FarClip", settings.Path.FarClip, error);
		if (!valid)
			return std::nullopt;
		settings.Scene = UUID(scene);
		return settings;
	}

	nlohmann::json ToJson(const StreamingSweepResult& result)
	{
		nlohmann::json json = nlohmann::json::object();
		json["Completed"] = result.Completed;
		json["Error"] = result.Error;
		json["ValidationEnabled"] = result.ValidationEnabled;
		json["Frames"] = result.Frames;
		json["TextureBudget"] = result.TextureBudget;
		json["MaxResidentTextureBytes"] = result.MaxResidentTextureBytes;
		json["MaxInFlightBytes"] = result.MaxInFlightBytes;
		json["MaxUploadedBytes"] = result.MaxUploadedBytes;
		json["MaxFinalizeMs"] = result.MaxFinalizeMs;
		json["FramesToSettle"] = result.FramesToSettle;
		json["FailedAssets"] = result.FailedAssets;
		json["LoadsCompleted"] = result.LoadsCompleted;
		json["Evictions"] = result.Evictions;
		json["StagingReleases"] = result.StagingReleases;
		json["PeakPrivateBytes"] = result.PeakPrivateBytes;
		json["PeakWorkingSetBytes"] = result.PeakWorkingSetBytes;
		json["NewErrors"] = result.NewErrors;
		json["ResidentTextureBytes"] = result.ResidentTextureBytes;
		json["PendingAssets"] = result.PendingAssets;
		json["FinalizeMs"] = result.FinalizeMs;
		json["PrivateBytes"] = result.PrivateBytes;
		json["DeviceBytes"] = result.DeviceBytes;
		return json;
	}

	std::optional<StreamingSweepResult> StreamingSweepResultFromJson(const nlohmann::json& json, std::string& error)
	{
		StreamingSweepResult result;
		const nlohmann::json* completed = JsonUtils::Find(json, "Completed");
		const nlohmann::json* validation = JsonUtils::Find(json, "ValidationEnabled");
		if (!completed || !completed->is_boolean() || !validation || !validation->is_boolean())
		{
			error = "The result has no Completed or ValidationEnabled flag";
			return std::nullopt;
		}
		result.Completed = completed->get<bool>();
		result.ValidationEnabled = validation->get<bool>();
		result.Error = JsonUtils::GetString(json, "Error");
		const bool valid = ReadUInt32(json, "Frames", result.Frames, error) && ReadUInt(json, "TextureBudget", result.TextureBudget, error)
			&& ReadUInt(json, "MaxResidentTextureBytes", result.MaxResidentTextureBytes, error)
			&& ReadUInt(json, "MaxInFlightBytes", result.MaxInFlightBytes, error) && ReadUInt(json, "MaxUploadedBytes", result.MaxUploadedBytes, error)
			&& ReadFloat(json, "MaxFinalizeMs", result.MaxFinalizeMs, error) && ReadNumbers(json, "FramesToSettle", result.FramesToSettle, error)
			&& ReadUInt32(json, "FailedAssets", result.FailedAssets, error) && ReadUInt(json, "LoadsCompleted", result.LoadsCompleted, error)
			&& ReadUInt(json, "Evictions", result.Evictions, error) && ReadUInt(json, "StagingReleases", result.StagingReleases, error)
			&& ReadUInt(json, "PeakPrivateBytes", result.PeakPrivateBytes, error) && ReadUInt(json, "PeakWorkingSetBytes", result.PeakWorkingSetBytes, error)
			&& ReadUInt32(json, "NewErrors", result.NewErrors, error)
			&& ReadNumbers(json, "ResidentTextureBytes", result.ResidentTextureBytes, error)
			&& ReadNumbers(json, "PendingAssets", result.PendingAssets, error) && ReadNumbers(json, "FinalizeMs", result.FinalizeMs, error)
			&& ReadNumbers(json, "PrivateBytes", result.PrivateBytes, error) && ReadNumbers(json, "DeviceBytes", result.DeviceBytes, error);
		if (!valid)
			return std::nullopt;
		return result;
	}

	int RunStreamingSweepProcess(int argc, char** argv)
	{
		// <settings file> <result file>
		if (argc < 4)
		{
			std::fprintf(stderr, "Usage: --strata-test-helper=streaming-sweep <settings file> <result file>\n");
			return 2;
		}
		const std::filesystem::path settingsFile = FileSystem::FromUTF8(argv[2]);
		const std::filesystem::path resultFile = FileSystem::FromUTF8(argv[3]);
		std::string error;
		const std::optional<std::string> settingsText = FileSystem::ReadText(settingsFile);
		const std::optional<nlohmann::json> settingsJson = settingsText ? JsonUtils::Parse(*settingsText, &error) : std::nullopt;
		const std::optional<StreamingSweepSettings> settings = settingsJson ? StreamingSweepSettingsFromJson(*settingsJson, error) : std::nullopt;
		if (!settings)
		{
			std::fprintf(stderr, "Invalid sweep settings '%s': %s\n", argv[2], error.c_str());
			return 2;
		}

		LogSpecification logSpecification;
		logSpecification.Level = LogLevel::Warn;
		Log::Init(logSpecification);
		// Like an application: worker and I/O pools with the default sizes.
		JobSystem::Init();
		StreamingSweepResult result;
		{
			GPUContext gpu;
			if (gpu.IsValid())
				result = RunStreamingSweep(gpu.GetDevice(), *settings);
			else
				result.Error = "No GPU device";
		}
		JobSystem::Shutdown();
		GPUContext::ShutdownShared();
		Log::Shutdown();

		if (!FileSystem::WriteText(resultFile, JsonUtils::Dump(ToJson(result))))
		{
			std::fprintf(stderr, "Could not write '%s'\n", argv[3]);
			return 1;
		}
		return 0;
	}

}
