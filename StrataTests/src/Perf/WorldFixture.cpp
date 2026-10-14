#include "Perf/WorldFixture.h"

#include "Perf/StressProject.h"
#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Math/Math.h"
#include "Strata/Project/Project.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"

#include <glm/gtc/quaternion.hpp>
#include <spdlog/fmt/fmt.h>
#include <stb_image_write.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <mutex>

namespace Strata::Tests
{

	namespace
	{

		// Fixed handles, so that every run (and every machine) writes the same projects.
		constexpr uint64_t c_WorldMaterialHandleBase = 0x5C00000000100000ull;
		constexpr uint64_t c_WorldStartSceneHandle = 0x5C00000000200000ull;
		constexpr uint64_t c_WorldSceneHandleBase = 0x5C00000000200001ull;
		constexpr uint64_t c_ManyMaterialHandleBase = 0x5D00000000100000ull;
		constexpr uint64_t c_ManySceneHandle = 0x5D00000000200000ull;

		// Entities generated in one job: a few milliseconds of work.
		constexpr uint32_t c_EntitiesPerChunk = 4096;
		// Chunks generated before they are written: bounds what the generator holds (about 70 MB of text for the worlds).
		constexpr uint32_t c_ChunksPerBatch = 64;

		// What the fixtures put on a generated entity.
		struct GeneratedEntity
		{
			UUID ID = UUID::Null();
			UUID Parent = UUID::Null();
			std::string Name;
			TransformComponent Transform;
			std::optional<CameraComponent> Camera;
			std::optional<DirectionalLightComponent> Light;
			std::optional<SkyLightComponent> Sky;
			std::optional<MeshRendererComponent> MeshRenderer;
			std::optional<RigidBodyComponent> Body;
			std::optional<BoxColliderComponent> Collider;
		};

		template<typename T>
		void AddComponentJson(nlohmann::json& components, const T& component)
		{
			const ComponentInfo* info = ComponentRegistry::Find<T>();
			ST_ASSERT(info, "Component type is not registered");
			components[info->Name] = ComponentAccess::Serialize(*info, &component);
		}

		// The entity as SceneSerializer writes it (keys sorted by nlohmann: Components, ID, Parent).
		nlohmann::json ToJson(const GeneratedEntity& entity)
		{
			nlohmann::json components = nlohmann::json::object();
			AddComponentJson(components, NameComponent { entity.Name });
			AddComponentJson(components, entity.Transform);
			if (entity.Camera)
				AddComponentJson(components, *entity.Camera);
			if (entity.Light)
				AddComponentJson(components, *entity.Light);
			if (entity.Sky)
				AddComponentJson(components, *entity.Sky);
			if (entity.MeshRenderer)
				AddComponentJson(components, *entity.MeshRenderer);
			if (entity.Body)
				AddComponentJson(components, *entity.Body);
			if (entity.Collider)
				AddComponentJson(components, *entity.Collider);

			nlohmann::json json = nlohmann::json::object();
			json["Components"] = std::move(components);
			json["ID"] = UUIDToJson(entity.ID);
			if (entity.Parent.IsValid())
				json["Parent"] = UUIDToJson(entity.Parent);
			return json;
		}

		// Creates the entity as loading its JSON would (components written right after they are added need no signal).
		bool CreateEntity(Scene& scene, const GeneratedEntity& description, std::string& error)
		{
			Entity entity = scene.CreateEntityWithUUID(description.ID, description.Name);
			if (!entity || entity.GetUUID() != description.ID)
			{
				error = fmt::format("Cannot create entity {} in scene '{}'", description.ID.ToString(), scene.GetName());
				return false;
			}
			entity.GetComponent<TransformComponent>() = description.Transform;
			if (description.Camera)
				entity.AddComponent<CameraComponent>(*description.Camera);
			if (description.Light)
				entity.AddComponent<DirectionalLightComponent>(*description.Light);
			if (description.Sky)
				entity.AddComponent<SkyLightComponent>(*description.Sky);
			if (description.MeshRenderer)
				entity.AddComponent<MeshRendererComponent>(*description.MeshRenderer);
			if (description.Body)
				entity.AddComponent<RigidBodyComponent>(*description.Body);
			if (description.Collider)
				entity.AddComponent<BoxColliderComponent>(*description.Collider);
			if (description.Parent.IsValid())
			{
				const Entity parent = scene.GetEntityByUUID(description.Parent);
				if (!parent || !scene.SetParent(entity, parent, false))
				{
					error = fmt::format("Cannot parent entity {} to {}", description.ID.ToString(), description.Parent.ToString());
					return false;
				}
			}
			return true;
		}

		// Entity IDs of a generated scene: the high bits from the scene's name, the low 40 bits its index plus one (never 0).
		UUID MakeEntityID(std::string_view sceneName, uint32_t index)
		{
			const uint64_t seed = Hash::FNV1a(sceneName) | (1ull << 63);
			return UUID((seed & 0xFFFFFF0000000000ull) | (static_cast<uint64_t>(index) + 1));
		}

		// A color of its own for material `index` (golden-ratio hues), in linear RGB.
		glm::vec4 GetMaterialColor(uint32_t index)
		{
			const float hue = std::fmod(static_cast<float>(index) * 0.6180339887f, 1.0f) * 6.0f;
			const float saturation = 0.55f;
			const float value = 0.8f;
			const float chroma = value * saturation;
			const float x = chroma * (1.0f - std::abs(std::fmod(hue, 2.0f) - 1.0f));
			glm::vec3 rgb(0.0f);
			switch (static_cast<int>(hue))
			{
				case 0: rgb = { chroma, x, 0.0f }; break;
				case 1: rgb = { x, chroma, 0.0f }; break;
				case 2: rgb = { 0.0f, chroma, x }; break;
				case 3: rgb = { 0.0f, x, chroma }; break;
				case 4: rgb = { x, 0.0f, chroma }; break;
				default: rgb = { chroma, 0.0f, x }; break;
			}
			rgb += value - chroma;
			return glm::vec4(glm::pow(rgb, glm::vec3(2.2f)), 1.0f);
		}

		// The camera at walking height looking along -Z, and the sun: the first entities of every generated scene.
		GeneratedEntity DescribeCamera(std::string_view sceneName)
		{
			GeneratedEntity camera;
			camera.ID = MakeEntityID(sceneName, 0);
			camera.Name = "Camera";
			camera.Transform.Translation = glm::vec3(0.0f, c_WorldCameraHeight, 0.0f);
			camera.Camera = CameraComponent {};
			return camera;
		}

		GeneratedEntity DescribeSun(std::string_view sceneName)
		{
			GeneratedEntity sun;
			sun.ID = MakeEntityID(sceneName, 1);
			sun.Name = "Sun";
			sun.Transform.Rotation = Math::LookRotation(glm::normalize(glm::vec3(-0.4f, -1.0f, -0.6f)));
			sun.Light = DirectionalLightComponent {};
			return sun;
		}

		// Where a world scene's districts lie: a grid of Columns x Rows districts of the cube grid, centred on the origin.
		struct WorldLayout
		{
			WorldSceneSpec Spec;
			std::vector<AssetHandle> Materials;
			uint32_t Districts = 0;
			uint32_t Columns = 1;
			uint32_t Rows = 1;
			float DistrictSize = static_cast<float>(c_WorldDistrictSide) * c_WorldCubeSpacing;
		};

		WorldLayout MakeWorldLayout(const WorldSceneSpec& spec, std::span<const AssetHandle> materials)
		{
			WorldLayout layout;
			layout.Spec = spec;
			layout.Materials.assign(materials.begin(), materials.end());
			layout.Districts = GetWorldDistrictCount(spec.Cubes);
			layout.Columns = std::max(1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(layout.Districts)))));
			layout.Rows = std::max(1u, (layout.Districts + layout.Columns - 1) / layout.Columns);
			return layout;
		}

		// The world scene's entity at `index` in hierarchy order: the camera, the sun, the sky and the ground, then each
		// district followed by its cubes.
		GeneratedEntity DescribeWorldEntity(const WorldLayout& layout, uint32_t index)
		{
			const std::string_view sceneName = layout.Spec.Name;
			const float width = static_cast<float>(layout.Columns) * layout.DistrictSize;
			const float depth = static_cast<float>(layout.Rows) * layout.DistrictSize;
			switch (index)
			{
				case 0: return DescribeCamera(sceneName);
				case 1: return DescribeSun(sceneName);
				case 2:
				{
					GeneratedEntity sky;
					sky.ID = MakeEntityID(sceneName, 2);
					sky.Name = "Sky";
					sky.Sky = SkyLightComponent {};
					sky.Sky->Source = SkyLightSource::Procedural;
					return sky;
				}
				case 3:
				{
					GeneratedEntity ground;
					ground.ID = MakeEntityID(sceneName, 3);
					ground.Name = "Ground";
					ground.Transform.Scale = glm::vec3(width, 1.0f, depth);
					ground.MeshRenderer = MeshRendererComponent {};
					ground.MeshRenderer->Mesh = BuiltinAssets::PlaneMesh;
					ground.MeshRenderer->CastShadows = false;
					return ground;
				}
				default:
					break;
			}

			// Every district but the last is full, so a district and its cubes take c_WorldDistrictCubes + 1 indices.
			const uint32_t offset = index - c_WorldFixedEntities;
			const uint32_t district = offset / (c_WorldDistrictCubes + 1);
			const uint32_t inDistrict = offset % (c_WorldDistrictCubes + 1);
			const UUID districtID = MakeEntityID(sceneName, c_WorldFixedEntities + district * (c_WorldDistrictCubes + 1));
			if (inDistrict == 0)
			{
				const uint32_t column = district % layout.Columns;
				const uint32_t row = district / layout.Columns;
				GeneratedEntity entity;
				entity.ID = districtID;
				entity.Name = fmt::format("District {}", district);
				entity.Transform.Translation = glm::vec3((static_cast<float>(column) + 0.5f) * layout.DistrictSize - width * 0.5f, 0.0f,
					(static_cast<float>(row) + 0.5f) * layout.DistrictSize - depth * 0.5f);
				return entity;
			}

			const uint32_t local = inDistrict - 1;
			const uint32_t cube = district * c_WorldDistrictCubes + local;
			GeneratedEntity entity;
			entity.ID = MakeEntityID(sceneName, index);
			entity.Parent = districtID;
			entity.Name = fmt::format("Cube {}", cube);
			// Cubes lie at odd multiples of half the spacing from their district's centre, so the camera at the world's centre
			// stands between them.
			const float half = layout.DistrictSize * 0.5f;
			entity.Transform.Translation = glm::vec3((static_cast<float>(local % c_WorldDistrictSide) + 0.5f) * c_WorldCubeSpacing - half, 0.5f,
				(static_cast<float>(local / c_WorldDistrictSide) + 0.5f) * c_WorldCubeSpacing - half);
			entity.MeshRenderer = MeshRendererComponent {};
			entity.MeshRenderer->Mesh = BuiltinAssets::CubeMesh;
			if (!layout.Materials.empty())
				entity.MeshRenderer->Material = layout.Materials[cube % layout.Materials.size()];
			if (layout.Spec.Colliders && cube % c_WorldColliderInterval == 0)
			{
				entity.Body = RigidBodyComponent {};
				entity.Body->Type = RigidBodyType::Static;
				entity.Collider = BoxColliderComponent {};
			}
			return entity;
		}

		// The empty start scene: the camera and the sun.
		GeneratedEntity DescribeEmptySceneEntity(uint32_t index)
		{
			return index == 0 ? DescribeCamera("Empty") : DescribeSun("Empty");
		}

		// The many-assets scene: the camera and the sun, then one cube per material on a square grid (roots).
		GeneratedEntity DescribeManyEntity(const std::vector<AssetHandle>& materials, uint32_t index)
		{
			if (index < 2)
				return index == 0 ? DescribeCamera("Many") : DescribeSun("Many");
			const uint32_t cube = index - 2;
			const uint32_t side = std::max(1u, static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(materials.size())))));
			const float half = static_cast<float>(side) * c_WorldCubeSpacing * 0.5f;
			GeneratedEntity entity;
			entity.ID = MakeEntityID("Many", index);
			entity.Name = fmt::format("Cube {}", cube);
			entity.Transform.Translation = glm::vec3((static_cast<float>(cube % side) + 0.5f) * c_WorldCubeSpacing - half, 0.5f,
				(static_cast<float>(cube / side) + 0.5f) * c_WorldCubeSpacing - half);
			entity.MeshRenderer = MeshRendererComponent {};
			entity.MeshRenderer->Mesh = BuiltinAssets::CubeMesh;
			entity.MeshRenderer->Material = materials[cube];
			return entity;
		}

		// Writes a scene document of `count` generated entities compact, on one line, as JsonUtils::Dump of
		// SceneSerializer::Serialize writes it, generating the entities chunk by chunk on the job system (written in order, so
		// the bytes never depend on the thread count). Returns the file's size.
		std::optional<uint64_t> WriteCompactScene(const std::filesystem::path& path, const std::string& name, uint32_t count,
			const std::function<GeneratedEntity(uint32_t index)>& describe, std::string& error)
		{
			// The document around the entities: an empty scene's, cut where its entity list is.
			const Scene empty(name);
			const std::string document = JsonUtils::Dump(SceneSerializer::Serialize(empty));
			constexpr std::string_view entityList = "\"Entities\":[]";
			const size_t listStart = document.find(entityList);
			if (listStart == std::string::npos)
			{
				error = "The scene document has no entity list";
				return std::nullopt;
			}
			const size_t listEnd = listStart + entityList.size() - 1; // At the list's ']'

			if (!FileSystem::CreateDirectories(path.parent_path()))
			{
				error = fmt::format("Could not create '{}'", FileSystem::ToUTF8(path.parent_path()));
				return std::nullopt;
			}
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			if (!file)
			{
				error = fmt::format("Could not open '{}' for writing", FileSystem::ToUTF8(path));
				return std::nullopt;
			}
			file.write(document.data(), static_cast<std::streamsize>(listEnd));

			const uint32_t chunks = (count + c_EntitiesPerChunk - 1) / c_EntitiesPerChunk;
			std::vector<std::string> texts(std::min(chunks, c_ChunksPerBatch));
			for (uint32_t firstChunk = 0; firstChunk < chunks; firstChunk += c_ChunksPerBatch)
			{
				const uint32_t batch = std::min(c_ChunksPerBatch, chunks - firstChunk);
				JobSystem::ParallelFor(batch, 1, [&](uint32_t begin, uint32_t end)
				{
					for (uint32_t chunk = begin; chunk < end; chunk++)
					{
						std::string& text = texts[chunk];
						text.clear();
						const uint32_t first = (firstChunk + chunk) * c_EntitiesPerChunk;
						const uint32_t last = std::min(count, first + c_EntitiesPerChunk);
						for (uint32_t index = first; index < last; index++)
						{
							if (index > 0)
								text += ',';
							text += JsonUtils::Dump(ToJson(describe(index)));
						}
					}
				});
				for (uint32_t chunk = 0; chunk < batch; chunk++)
					file.write(texts[chunk].data(), static_cast<std::streamsize>(texts[chunk].size()));
			}
			file.write(document.data() + listEnd, static_cast<std::streamsize>(document.size() - listEnd));
			file.put('\n');
			file.close();
			if (!file)
			{
				error = fmt::format("Could not write '{}'", FileSystem::ToUTF8(path));
				return std::nullopt;
			}
			const std::optional<uint64_t> size = FileSystem::GetFileSize(path);
			if (!size)
				error = fmt::format("Could not read the size of '{}'", FileSystem::ToUTF8(path));
			return size;
		}

		// The sidecar the editor writes: the handle, the type and the importer's default settings.
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
			if (!FileSystem::WriteText(metaPath, JsonUtils::Dump(meta, 1, '\t') + "\n"))
			{
				error = fmt::format("Could not write '{}'", FileSystem::ToUTF8(metaPath));
				return false;
			}
			return true;
		}

		bool WriteMaterial(const std::filesystem::path& path, uint32_t colorIndex, AssetHandle handle, bool withMeta, std::string& error)
		{
			MaterialProperties properties;
			properties.BaseColor = GetMaterialColor(colorIndex);
			properties.Roughness = 0.6f;
			if (!FileSystem::WriteText(path, JsonUtils::Dump(Material::Create(properties)->Serialize(), 1, '\t') + "\n"))
			{
				error = fmt::format("Could not write '{}'", FileSystem::ToUTF8(path));
				return false;
			}
			return !withMeta || WriteMeta(path, handle, error);
		}

		bool WriteSceneAsset(const std::filesystem::path& path, const std::string& name, uint32_t count, const std::function<GeneratedEntity(uint32_t)>& describe,
			AssetHandle handle, uint64_t* outBytes, std::string& error)
		{
			const std::optional<uint64_t> bytes = WriteCompactScene(path, name, count, describe, error);
			if (!bytes)
				return false;
			if (outBytes)
				*outBytes = *bytes;
			return WriteMeta(path, handle, error);
		}

		bool CreateAssetFolders(const std::filesystem::path& assets, std::initializer_list<const char*> folders, std::string& error)
		{
			for (const char* folder : folders)
			{
				if (!FileSystem::CreateDirectories(assets / folder))
				{
					error = fmt::format("Could not create '{}'", FileSystem::ToUTF8(assets / folder));
					return false;
				}
			}
			return true;
		}

		// Sets stb_image_write's PNG compression level (a global of the library) for as long as it lives.
		class ScopedPNGCompressionLevel
		{
		public:
			explicit ScopedPNGCompressionLevel(int level)
				: m_Previous(stbi_write_png_compression_level)
			{
				stbi_write_png_compression_level = level;
			}

			~ScopedPNGCompressionLevel()
			{
				stbi_write_png_compression_level = m_Previous;
			}

			ScopedPNGCompressionLevel(const ScopedPNGCompressionLevel&) = delete;
			ScopedPNGCompressionLevel& operator=(const ScopedPNGCompressionLevel&) = delete;
		private:
			int m_Previous = 8;
		};

		std::optional<uint32_t> ParseUInt(std::string_view text)
		{
			uint32_t value = 0;
			const std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), value);
			if (result.ec != std::errc() || result.ptr != text.data() + text.size())
				return std::nullopt;
			return value;
		}

		// "<name>:<cubes>[:colliders]"
		std::optional<WorldSceneSpec> ParseWorldSceneArgument(std::string_view argument)
		{
			const size_t first = argument.find(':');
			if (first == std::string_view::npos || first == 0)
				return std::nullopt;
			const std::string_view rest = argument.substr(first + 1);
			const size_t second = rest.find(':');
			const std::optional<uint32_t> cubes = ParseUInt(rest.substr(0, second));
			if (!cubes)
				return std::nullopt;
			WorldSceneSpec spec { std::string(argument.substr(0, first)), *cubes, false };
			if (second != std::string_view::npos)
			{
				if (rest.substr(second + 1) != "colliders")
					return std::nullopt;
				spec.Colliders = true;
			}
			return spec;
		}

	}

	uint32_t GetWorldDistrictCount(uint32_t cubes)
	{
		return (cubes + c_WorldDistrictCubes - 1) / c_WorldDistrictCubes;
	}

	uint32_t GetWorldEntityCount(const WorldSceneSpec& spec)
	{
		return c_WorldFixedEntities + GetWorldDistrictCount(spec.Cubes) + spec.Cubes;
	}

	UUID GetWorldEntityID(const WorldSceneSpec& spec, uint32_t index)
	{
		return MakeEntityID(spec.Name, index);
	}

	std::optional<WorldProject> WriteWorldProject(const std::filesystem::path& directory, const WorldProjectSpec& spec, std::string& error)
	{
		for (const WorldSceneSpec& scene : spec.Scenes)
		{
			if (scene.Name.empty() || scene.Name == "Empty" || GetWorldEntityCount(scene) > Scene::c_MaxEntities)
			{
				error = fmt::format("World scene '{}' needs a name other than 'Empty' and at most {} entities", scene.Name, Scene::c_MaxEntities);
				return std::nullopt;
			}
		}

		Ref<Project> projectFile = Project::Create(directory, "World", &error);
		if (!projectFile)
			return std::nullopt;
		WorldProject project;
		project.Directory = projectFile->GetProjectDirectory();
		project.ProjectFile = projectFile->GetProjectFile();
		const std::filesystem::path assets = projectFile->GetAssetDirectory();
		if (!CreateAssetFolders(assets, { "Materials", "Scenes" }, error))
			return std::nullopt;

		for (uint32_t index = 0; index < spec.Materials; index++)
		{
			const AssetHandle handle = UUID(c_WorldMaterialHandleBase + index);
			if (!WriteMaterial(assets / "Materials" / FileSystem::FromUTF8(fmt::format("World_{}.stmat", index)), index, handle, true, error))
				return std::nullopt;
			project.Materials.push_back(handle);
		}

		project.StartScene = UUID(c_WorldStartSceneHandle);
		if (!WriteSceneAsset(assets / "Scenes" / "Empty.stscene", "Empty", 2, DescribeEmptySceneEntity, project.StartScene, nullptr, error))
			return std::nullopt;

		for (size_t index = 0; index < spec.Scenes.size(); index++)
		{
			const WorldSceneSpec& sceneSpec = spec.Scenes[index];
			WorldSceneFile scene;
			scene.Name = sceneSpec.Name;
			scene.Handle = UUID(c_WorldSceneHandleBase + index);
			scene.AssetPath = fmt::format("Scenes/{}.stscene", sceneSpec.Name);
			scene.Path = assets / FileSystem::FromUTF8(scene.AssetPath);
			scene.Entities = GetWorldEntityCount(sceneSpec);
			const WorldLayout layout = MakeWorldLayout(sceneSpec, project.Materials);
			if (!WriteSceneAsset(scene.Path, sceneSpec.Name, scene.Entities, [&layout](uint32_t entity) { return DescribeWorldEntity(layout, entity); },
				scene.Handle, &scene.Bytes, error))
			{
				return std::nullopt;
			}
			project.Scenes.push_back(std::move(scene));
		}

		projectFile->GetConfig().StartScene = project.StartScene;
		if (!projectFile->Save(&error))
			return std::nullopt;
		return project;
	}

	bool BuildWorldScene(Scene& scene, const WorldSceneSpec& spec, std::span<const AssetHandle> materials, std::string& error)
	{
		const uint32_t count = GetWorldEntityCount(spec);
		if (scene.GetRegistryEntityCount() + count > Scene::c_MaxEntities)
		{
			error = fmt::format("Scene '{}' cannot hold the {} entities of world '{}'", scene.GetName(), count, spec.Name);
			return false;
		}
		const WorldLayout layout = MakeWorldLayout(spec, materials);
		for (uint32_t index = 0; index < count; index++)
		{
			if (!CreateEntity(scene, DescribeWorldEntity(layout, index), error))
				return false;
		}
		return true;
	}

	std::optional<ManyAssetsProject> WriteManyAssetsProject(const std::filesystem::path& directory, uint32_t materials, std::string& error, bool withMetaFiles)
	{
		if (materials == 0 || materials + 2 > Scene::c_MaxEntities)
		{
			error = fmt::format("A many-assets project needs 1 to {} materials", Scene::c_MaxEntities - 2);
			return std::nullopt;
		}
		Ref<Project> projectFile = Project::Create(directory, "Many", &error);
		if (!projectFile)
			return std::nullopt;
		ManyAssetsProject project;
		project.Directory = projectFile->GetProjectDirectory();
		project.ProjectFile = projectFile->GetProjectFile();
		const std::filesystem::path assets = projectFile->GetAssetDirectory();
		if (!CreateAssetFolders(assets, { "Materials", "Scenes" }, error))
			return std::nullopt;

		project.Materials.resize(materials);
		for (uint32_t index = 0; index < materials; index++)
			project.Materials[index] = UUID(c_ManyMaterialHandleBase + index);
		// Tens of thousands of small files: written in parallel when the job system runs.
		std::mutex errorMutex;
		std::string materialError;
		JobSystem::ParallelFor(materials, 64, [&](uint32_t begin, uint32_t end)
		{
			for (uint32_t index = begin; index < end; index++)
			{
				const std::filesystem::path folder = assets / "Materials" / FileSystem::FromUTF8(fmt::format("Folder_{:02}", index / c_ManyAssetsFolderSize));
				std::string writeError;
				if (!WriteMaterial(folder / FileSystem::FromUTF8(fmt::format("Material_{:05}.stmat", index)), index, project.Materials[index], withMetaFiles, writeError))
				{
					std::scoped_lock<std::mutex> lock(errorMutex);
					materialError = std::move(writeError);
				}
			}
		});
		if (!materialError.empty())
		{
			error = materialError;
			return std::nullopt;
		}

		project.Scene = UUID(c_ManySceneHandle);
		const std::vector<AssetHandle>& handles = project.Materials;
		if (!WriteSceneAsset(assets / "Scenes" / "Many.stscene", "Many", materials + 2, [&handles](uint32_t entity) { return DescribeManyEntity(handles, entity); },
			project.Scene, nullptr, error))
		{
			return std::nullopt;
		}

		projectFile->GetConfig().StartScene = project.Scene;
		if (!projectFile->Save(&error))
			return std::nullopt;
		return project;
	}

	std::optional<std::vector<std::filesystem::path>> WriteTextureFolder(const std::filesystem::path& directory, uint32_t count, uint32_t size,
		uint32_t seed, std::string& error)
	{
		if (size == 0 || seed == 0)
		{
			error = "Textures need a size above 0 and a seed from 1";
			return std::nullopt;
		}
		if (!FileSystem::CreateDirectories(directory))
		{
			error = fmt::format("Could not create '{}'", FileSystem::ToUTF8(directory));
			return std::nullopt;
		}
		std::vector<std::filesystem::path> paths(count);
		for (uint32_t index = 0; index < count; index++)
			paths[index] = directory / FileSystem::FromUTF8(fmt::format("Texture_{:04}.png", index));

		const ScopedPNGCompressionLevel compression(1);
		std::mutex errorMutex;
		std::string textureError;
		JobSystem::ParallelFor(count, 1, [&](uint32_t begin, uint32_t end)
		{
			for (uint32_t index = begin; index < end; index++)
			{
				const std::vector<uint8_t> pixels = CreateStressTexels(size, seed + index);
				std::vector<uint8_t> encoded;
				auto write = [](void* context, void* data, int bytes)
				{
					auto* output = static_cast<std::vector<uint8_t>*>(context);
					output->insert(output->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + bytes);
				};
				const bool written = stbi_write_png_to_func(write, &encoded, static_cast<int>(size), static_cast<int>(size), 3, pixels.data(),
					static_cast<int>(size * 3)) && FileSystem::WriteBytes(paths[index], encoded);
				if (!written)
				{
					std::scoped_lock<std::mutex> lock(errorMutex);
					textureError = fmt::format("Could not write '{}'", FileSystem::ToUTF8(paths[index]));
				}
			}
		});
		if (!textureError.empty())
		{
			error = textureError;
			return std::nullopt;
		}
		return paths;
	}

	std::optional<std::vector<std::filesystem::path>> WriteSmallTextureFolder(const std::filesystem::path& directory, std::string& error)
	{
		return WriteTextureFolder(directory, c_SmallTextureFolderCount, c_SmallTextureFolderSize, 1, error);
	}

	std::optional<int> RunWorldFixtureHelper(std::string_view mode, int argc, char** argv)
	{
		if (mode != "write-world-project" && mode != "write-many-assets-project" && mode != "write-texture-folder")
			return std::nullopt;
		if (argc < 3)
			return 2;
		const std::filesystem::path directory = FileSystem::FromUTF8(argv[2]);
		LogSpecification logSpecification;
		logSpecification.Level = LogLevel::Warn;
		Log::Init(logSpecification);
		JobSystem::Init();
		std::string error;
		bool written = false;
		if (mode == "write-world-project")
		{
			WorldProjectSpec spec;
			for (int index = 3; index < argc; index++)
			{
				const std::optional<WorldSceneSpec> scene = ParseWorldSceneArgument(argv[index]);
				if (!scene)
				{
					JobSystem::Shutdown();
					Log::Shutdown();
					return 2;
				}
				spec.Scenes.push_back(*scene);
			}
			written = WriteWorldProject(directory, spec, error).has_value();
		}
		else if (mode == "write-many-assets-project")
		{
			const std::optional<uint32_t> materials = argc == 4 ? ParseUInt(argv[3]) : std::nullopt;
			if (!materials)
			{
				JobSystem::Shutdown();
				Log::Shutdown();
				return 2;
			}
			written = WriteManyAssetsProject(directory, *materials, error).has_value();
		}
		else
		{
			const std::optional<uint32_t> count = argc == 5 ? ParseUInt(argv[3]) : std::nullopt;
			const std::optional<uint32_t> size = argc == 5 ? ParseUInt(argv[4]) : std::nullopt;
			if (!count || !size)
			{
				JobSystem::Shutdown();
				Log::Shutdown();
				return 2;
			}
			written = WriteTextureFolder(directory, *count, *size, 1, error).has_value();
		}
		JobSystem::Shutdown();
		Log::Shutdown();
		if (!written)
		{
			std::fprintf(stderr, "Cannot write '%s': %s\n", argv[2], error.c_str());
			return 1;
		}
		std::printf("Wrote '%s'\n", argv[2]);
		return 0;
	}

}
