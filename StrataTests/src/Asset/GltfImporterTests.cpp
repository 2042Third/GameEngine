#include <doctest/doctest.h>

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/GltfImporter.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "TestHelpers.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	template<typename T>
	void Append(std::vector<uint8_t>& buffer, const std::vector<T>& values)
	{
		const auto* bytes = reinterpret_cast<const uint8_t*>(values.data());
		buffer.insert(buffer.end(), bytes, bytes + values.size() * sizeof(T));
	}

	std::string EncodeBase64(const std::vector<uint8_t>& data)
	{
		static constexpr char c_Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string result;
		for (size_t index = 0; index < data.size(); index += 3)
		{
			const uint32_t chunk = (static_cast<uint32_t>(data[index]) << 16) | (index + 1 < data.size() ? data[index + 1] << 8 : 0)
				| (index + 2 < data.size() ? data[index + 2] : 0);
			result += c_Alphabet[(chunk >> 18) & 63];
			result += c_Alphabet[(chunk >> 12) & 63];
			result += index + 1 < data.size() ? c_Alphabet[(chunk >> 6) & 63] : '=';
			result += index + 2 < data.size() ? c_Alphabet[chunk & 63] : '=';
		}
		return result;
	}

	// Geometry shared by every test model: a unit quad facing +Z (indexed list) and the same quad as a strip.
	std::vector<uint8_t> CreateGeometryBuffer()
	{
		std::vector<uint8_t> buffer;
		Append(buffer, std::vector<float> { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0 });      // Positions, offset 0
		Append(buffer, std::vector<float> { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 });      // Normals, offset 48
		Append(buffer, std::vector<float> { 0, 1, 1, 1, 1, 0, 0, 0 });                  // UVs, offset 96
		Append(buffer, std::vector<uint16_t> { 0, 1, 2, 0, 2, 3 });                     // List indices, offset 128
		Append(buffer, std::vector<uint16_t> { 0, 1, 3, 2 });                           // Strip indices, offset 140
		return buffer;                                                                   // 148 bytes
	}

	struct ModelOptions
	{
		std::string ImageUri;          // Empty: no image; "embedded": image in the binary buffer (GLB) or data URI
		std::vector<std::string> ExtensionsRequired;
		std::string BufferUri;         // Overrides the buffer URI (.gltf only)
	};

	nlohmann::json CreateDocument(const ModelOptions& options, size_t bufferLength, const std::string& bufferUri, std::optional<size_t> imageView)
	{
		nlohmann::json document = {
			{ "asset", { { "version", "2.0" } } },
			{ "scene", 0 },
			{ "scenes", nlohmann::json::array({ { { "nodes", { 0, 2, 3 } } } }) },
			{ "nodes", nlohmann::json::array({
				{ { "name", "Body" }, { "mesh", 0 }, { "translation", { 1.0, 2.0, 3.0 } }, { "children", { 1 } } },
				{ { "name", "Lamp" }, { "extensions", { { "KHR_lights_punctual", { { "light", 0 } } } } } },
				{ { "name", "Cam" }, { "camera", 0 }, { "rotation", { 0.0, 0.7071068, 0.0, 0.7071068 } } },
				{ { "name", "Strip" }, { "mesh", 1 }, { "matrix", { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 5, 0, 0, 1 } } } }) },
			{ "meshes", nlohmann::json::array({
				{ { "name", "Quad" }, { "primitives", nlohmann::json::array({ { { "attributes", { { "POSITION", 0 }, { "NORMAL", 1 }, { "TEXCOORD_0", 2 } } }, { "indices", 3 }, { "material", 0 } } }) } },
				{ { "name", "StripQuad" }, { "primitives", nlohmann::json::array({ { { "attributes", { { "POSITION", 0 } } }, { "indices", 4 }, { "mode", 5 } } }) } } }) },
			{ "materials", nlohmann::json::array({ {
				{ "name", "Painted" },
				{ "pbrMetallicRoughness", { { "baseColorFactor", { 0.5, 0.25, 1.0, 0.75 } }, { "metallicFactor", 0.2 }, { "roughnessFactor", 0.6 } } },
				{ "emissiveFactor", { 1.0, 0.5, 0.0 } },
				{ "alphaMode", "MASK" },
				{ "alphaCutoff", 0.3 },
				{ "doubleSided", true },
				{ "extensions", { { "KHR_materials_emissive_strength", { { "emissiveStrength", 4.0 } } } } } } }) },
			{ "cameras", nlohmann::json::array({ { { "type", "perspective" }, { "perspective", { { "yfov", 0.8 }, { "znear", 0.05 }, { "zfar", 500.0 } } } } }) },
			{ "extensions", { { "KHR_lights_punctual", { { "lights", nlohmann::json::array({ { { "type", "point" }, { "color", { 1.0, 0.5, 0.25 } }, { "intensity", 4.0 } } }) } } } } },
			{ "extensionsUsed", { "KHR_lights_punctual", "KHR_materials_emissive_strength" } },
			{ "buffers", nlohmann::json::array({ { { "byteLength", bufferLength } } }) },
			{ "bufferViews", nlohmann::json::array({
				{ { "buffer", 0 }, { "byteOffset", 0 }, { "byteLength", 48 }, { "target", 34962 } },
				{ { "buffer", 0 }, { "byteOffset", 48 }, { "byteLength", 48 }, { "target", 34962 } },
				{ { "buffer", 0 }, { "byteOffset", 96 }, { "byteLength", 32 }, { "target", 34962 } },
				{ { "buffer", 0 }, { "byteOffset", 128 }, { "byteLength", 12 }, { "target", 34963 } },
				{ { "buffer", 0 }, { "byteOffset", 140 }, { "byteLength", 8 }, { "target", 34963 } } }) },
			{ "accessors", nlohmann::json::array({
				{ { "bufferView", 0 }, { "componentType", 5126 }, { "count", 4 }, { "type", "VEC3" }, { "min", { 0, 0, 0 } }, { "max", { 1, 1, 0 } } },
				{ { "bufferView", 1 }, { "componentType", 5126 }, { "count", 4 }, { "type", "VEC3" } },
				{ { "bufferView", 2 }, { "componentType", 5126 }, { "count", 4 }, { "type", "VEC2" } },
				{ { "bufferView", 3 }, { "componentType", 5123 }, { "count", 6 }, { "type", "SCALAR" } },
				{ { "bufferView", 4 }, { "componentType", 5123 }, { "count", 4 }, { "type", "SCALAR" } } }) }
		};
		if (!bufferUri.empty())
			document["buffers"][0]["uri"] = bufferUri;

		if (!options.ImageUri.empty())
		{
			nlohmann::json image = nlohmann::json::object();
			if (imageView)
			{
				image["bufferView"] = *imageView;
				image["mimeType"] = "image/png";
			}
			else
			{
				image["uri"] = options.ImageUri;
			}
			document["images"] = nlohmann::json::array({ image });
			document["samplers"] = nlohmann::json::array({ { { "magFilter", 9728 }, { "wrapS", 33071 }, { "wrapT", 33071 } } });
			document["textures"] = nlohmann::json::array({ { { "source", 0 }, { "sampler", 0 } } });
			document["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"] = { { "index", 0 } };
			document["materials"][0]["pbrMetallicRoughness"]["metallicRoughnessTexture"] = { { "index", 0 } };
		}
		if (!options.ExtensionsRequired.empty())
		{
			document["extensionsRequired"] = options.ExtensionsRequired;
			for (const std::string& extension : options.ExtensionsRequired)
				document["extensionsUsed"].push_back(extension);
		}
		return document;
	}

	std::vector<uint8_t> CreateGltf(const ModelOptions& options)
	{
		const std::vector<uint8_t> buffer = CreateGeometryBuffer();
		const std::string bufferUri = options.BufferUri.empty() ? "data:application/octet-stream;base64," + EncodeBase64(buffer) : options.BufferUri;
		ModelOptions resolved = options;
		if (resolved.ImageUri == "embedded")
			resolved.ImageUri = "data:image/png;base64," + EncodeBase64(Tests::CreateSolidPNG(4, 4, 200, 100, 50));
		const std::string text = JsonUtils::Dump(CreateDocument(resolved, buffer.size(), bufferUri, std::nullopt));
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	std::vector<uint8_t> CreateGlb(const ModelOptions& options)
	{
		std::vector<uint8_t> binary = CreateGeometryBuffer();
		std::optional<size_t> imageView;
		nlohmann::json extraView;
		if (!options.ImageUri.empty())
		{
			const std::vector<uint8_t> png = Tests::CreateSolidPNG(4, 4, 200, 100, 50);
			extraView = { { "buffer", 0 }, { "byteOffset", binary.size() }, { "byteLength", png.size() } };
			binary.insert(binary.end(), png.begin(), png.end());
			imageView = 5;
		}
		while (binary.size() % 4 != 0)
			binary.push_back(0);

		nlohmann::json document = CreateDocument(options, binary.size(), {}, imageView);
		if (imageView)
			document["bufferViews"].push_back(extraView);
		std::string json = JsonUtils::Dump(document);
		while (json.size() % 4 != 0)
			json.push_back(' ');

		std::vector<uint8_t> glb;
		auto writeU32 = [&glb](uint32_t value) { Append(glb, std::vector<uint32_t> { value }); };
		writeU32(0x46546C67); // "glTF"
		writeU32(2);
		writeU32(static_cast<uint32_t>(12 + 8 + json.size() + 8 + binary.size()));
		writeU32(static_cast<uint32_t>(json.size()));
		writeU32(0x4E4F534A); // "JSON"
		glb.insert(glb.end(), json.begin(), json.end());
		writeU32(static_cast<uint32_t>(binary.size()));
		writeU32(0x004E4942); // "BIN"
		glb.insert(glb.end(), binary.begin(), binary.end());
		return glb;
	}

	struct ImportOutput
	{
		bool Success = false;
		std::string Error;
		ImportResult Result;
		AssetHandle Handle = UUID(0xABCDEF12345ull);

		const ImportedSubAsset* Find(std::string_view keyPrefix) const
		{
			for (const ImportedSubAsset& subAsset : Result.SubAssets)
			{
				if (subAsset.Key.rfind(keyPrefix, 0) == 0)
					return &subAsset;
			}
			return nullptr;
		}

		AssetHandle HandleOf(std::string_view keyPrefix) const
		{
			const ImportedSubAsset* subAsset = Find(keyPrefix);
			return subAsset ? DeriveSubAssetHandle(Handle, subAsset->Key) : UUID::Null();
		}
	};

	ImportOutput RunImport(const std::filesystem::path& assetDirectory, const std::string& relativePath, const std::vector<uint8_t>& file, nlohmann::json settings = {})
	{
		const std::filesystem::path path = assetDirectory / FileSystem::FromUTF8(relativePath);
		REQUIRE(FileSystem::WriteBytes(path, file));

		GltfImporter importer;
		nlohmann::json effectiveSettings = importer.GetDefaultSettings(path);
		for (const auto& [key, value] : settings.items())
			effectiveSettings[key] = value;

		ImportOutput output;
		ImportContext context;
		context.Handle = output.Handle;
		context.SourcePath = path;
		context.AssetDirectory = assetDirectory;
		context.Settings = effectiveSettings;
		output.Success = importer.Import(context, output.Result, &output.Error);
		return output;
	}

	Ref<Scene> InstantiateModel(const ImportOutput& output, Entity& outRoot)
	{
		std::string error;
		Ref<Model> model = Model::Deserialize(output.Result.Data, &error);
		REQUIRE_MESSAGE(model, error);
		Ref<Scene> scene = CreateRef<Scene>();
		std::vector<Entity> roots = model->Instantiate(*scene);
		REQUIRE(roots.size() == 1);
		outRoot = roots[0];
		return scene;
	}

	Entity FindChild(Entity parent, const std::string& name)
	{
		for (Entity child : parent.GetChildren())
		{
			if (child.GetName() == name)
				return child;
		}
		return {};
	}

	nlohmann::json ParseDocument(const std::vector<uint8_t>& file)
	{
		std::optional<nlohmann::json> document = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(file.data()), file.size()));
		REQUIRE(document);
		return *document;
	}

	std::vector<uint8_t> ToBytes(const nlohmann::json& document)
	{
		const std::string text = JsonUtils::Dump(document);
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	bool HasWarningContaining(const ImportResult& result, std::string_view text)
	{
		return std::any_of(result.Warnings.begin(), result.Warnings.end(), [text](const std::string& warning) { return warning.find(text) != std::string::npos; });
	}

	// A model with one node and one triangle-list primitive built from the given float streams.
	std::vector<uint8_t> CreatePrimitiveModel(const std::vector<glm::vec3>& positions, const std::vector<glm::vec3>& normals, const std::vector<glm::vec2>& texCoords,
		const std::vector<uint16_t>& indices)
	{
		std::vector<uint8_t> buffer;
		nlohmann::json views = nlohmann::json::array();
		nlohmann::json accessors = nlohmann::json::array();
		nlohmann::json attributes = nlohmann::json::object();
		auto addStream = [&](const void* data, size_t size, size_t count, const char* type, int componentType)
		{
			views.push_back({ { "buffer", 0 }, { "byteOffset", buffer.size() }, { "byteLength", size } });
			const auto* bytes = static_cast<const uint8_t*>(data);
			buffer.insert(buffer.end(), bytes, bytes + size);
			while (buffer.size() % 4 != 0)
				buffer.push_back(0);
			accessors.push_back({ { "bufferView", views.size() - 1 }, { "componentType", componentType }, { "count", count }, { "type", type } });
			return accessors.size() - 1;
		};

		attributes["POSITION"] = addStream(positions.data(), positions.size() * sizeof(glm::vec3), positions.size(), "VEC3", 5126);
		if (!normals.empty())
			attributes["NORMAL"] = addStream(normals.data(), normals.size() * sizeof(glm::vec3), normals.size(), "VEC3", 5126);
		if (!texCoords.empty())
			attributes["TEXCOORD_0"] = addStream(texCoords.data(), texCoords.size() * sizeof(glm::vec2), texCoords.size(), "VEC2", 5126);
		const size_t indexAccessor = addStream(indices.data(), indices.size() * sizeof(uint16_t), indices.size(), "SCALAR", 5123);

		const nlohmann::json document = {
			{ "asset", { { "version", "2.0" } } },
			{ "scenes", nlohmann::json::array({ { { "nodes", { 0 } } } }) },
			{ "nodes", nlohmann::json::array({ { { "mesh", 0 } } }) },
			{ "meshes", nlohmann::json::array({ { { "primitives", nlohmann::json::array({ { { "attributes", attributes }, { "indices", indexAccessor } } }) } } }) },
			{ "buffers", nlohmann::json::array({ { { "byteLength", buffer.size() }, { "uri", "data:application/octet-stream;base64," + EncodeBase64(buffer) } } }) },
			{ "bufferViews", views },
			{ "accessors", accessors }
		};
		return ToBytes(document);
	}

	glm::vec3 FaceNormal(const Mesh& mesh, size_t corner)
	{
		const std::vector<glm::vec3>& positions = mesh.GetPositions();
		const std::vector<uint32_t>& indices = mesh.GetIndices();
		return glm::normalize(glm::cross(positions[indices[corner + 1]] - positions[indices[corner]], positions[indices[corner + 2]] - positions[indices[corner]]));
	}

}

TEST_SUITE("Asset.Gltf")
{
	TEST_CASE("glTF models import meshes, materials, textures and the node hierarchy")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfImport");
		const ImportOutput output = RunImport(assets, "Models/Sample.gltf", CreateGltf({ .ImageUri = "embedded" }));
		REQUIRE_MESSAGE(output.Success, output.Error);

		// Meshes
		const ImportedSubAsset* quadData = output.Find("Mesh/0");
		REQUIRE(quadData);
		CHECK(quadData->Name == "Quad");
		Ref<Mesh> quad = Mesh::Deserialize(quadData->Data);
		REQUIRE(quad);
		REQUIRE(quad->GetSubmeshes().size() == 1);
		const Submesh& submesh = quad->GetSubmeshes()[0];
		CHECK(submesh.VertexCount == 4);
		CHECK(submesh.LODs[0].IndexCount == 6);
		CHECK(submesh.Material == output.HandleOf("Material/0"));
		for (const MeshVertexAttributes& attribute : quad->GetAttributes())
		{
			CHECK(attribute.Normal == glm::vec3(0, 0, 1));
			CHECK(glm::length(glm::vec3(attribute.Tangent)) == doctest::Approx(1.0f));
		}

		Ref<Mesh> strip = Mesh::Deserialize(output.Find("Mesh/1")->Data);
		REQUIRE(strip);
		CHECK(strip->GetSubmeshes()[0].Material == BuiltinAssets::DefaultMaterial);
		REQUIRE(strip->GetSubmeshes()[0].LODs[0].IndexCount == 6);
		const std::vector<uint32_t>& indices = strip->GetIndices();
		const std::vector<glm::vec3>& positions = strip->GetPositions();
		for (size_t triangle = 0; triangle < 6; triangle += 3)
		{
			// Strip triangles keep the counter-clockwise winding of the first triangle.
			const glm::vec3 normal = glm::cross(positions[indices[triangle + 1]] - positions[indices[triangle]], positions[indices[triangle + 2]] - positions[indices[triangle]]);
			CHECK(normal.z > 0.0f);
		}
		for (const MeshVertexAttributes& attribute : strip->GetAttributes())
			CHECK(attribute.Normal.z == doctest::Approx(1.0f)); // Generated

		// Material
		const ImportedSubAsset* materialData = output.Find("Material/0");
		REQUIRE(materialData);
		CHECK(materialData->Name == "Painted");
		Ref<Material> material = Material::Deserialize(materialData->Data);
		REQUIRE(material);
		const MaterialProperties& properties = material->GetProperties();
		CHECK(properties.BaseColor == glm::vec4(0.5f, 0.25f, 1.0f, 0.75f));
		CHECK(properties.Metallic == doctest::Approx(0.2f));
		CHECK(properties.Roughness == doctest::Approx(0.6f));
		CHECK(properties.EmissiveColor == glm::vec3(1.0f, 0.5f, 0.0f));
		CHECK(properties.EmissiveIntensity == doctest::Approx(4.0f));
		CHECK(properties.AlphaMode == MaterialAlphaMode::Mask);
		CHECK(properties.AlphaCutoff == doctest::Approx(0.3f));
		CHECK(properties.DoubleSided);

		// The same image is imported once per color space.
		const ImportedSubAsset* colorTexture = output.Find("Texture/0/Color");
		const ImportedSubAsset* dataTexture = output.Find("Texture/0/Data");
		REQUIRE(colorTexture);
		REQUIRE(dataTexture);
		CHECK(properties.BaseColorMap == output.HandleOf("Texture/0/Color"));
		CHECK(properties.MetallicRoughnessMap == output.HandleOf("Texture/0/Data"));
		Ref<Texture> color = Texture::Deserialize(colorTexture->Data);
		Ref<Texture> data = Texture::Deserialize(dataTexture->Data);
		REQUIRE(color);
		REQUIRE(data);
		CHECK(color->GetSpecification().Format == TextureFormat::RGBA8SRGB);
		CHECK(data->GetSpecification().Format == TextureFormat::RGBA8);
		CHECK(color->GetSpecification().Wrap == TextureWrap::Clamp);
		CHECK(color->GetSpecification().Filter == TextureFilter::Nearest);
		CHECK(color->GetWidth() == 4);

		// Hierarchy
		Entity root;
		Ref<Scene> scene = InstantiateModel(output, root);
		CHECK(root.GetName() == "Sample");
		Entity body = FindChild(root, "Body");
		REQUIRE(body);
		CHECK(body.GetComponent<TransformComponent>().Translation == glm::vec3(1, 2, 3));
		REQUIRE(body.HasComponent<MeshRendererComponent>());
		CHECK(body.GetComponent<MeshRendererComponent>().Mesh == output.HandleOf("Mesh/0"));

		Entity lamp = FindChild(body, "Lamp");
		REQUIRE(lamp);
		REQUIRE(lamp.HasComponent<PointLightComponent>());
		const PointLightComponent& light = lamp.GetComponent<PointLightComponent>();
		CHECK(light.Intensity == doctest::Approx(4.0f));
		CHECK(light.Color == glm::vec3(1.0f, 0.5f, 0.25f));
		CHECK(light.Range == doctest::Approx(20.0f)); // No glTF range: where intensity drops below 1%

		Entity camera = FindChild(root, "Cam");
		REQUIRE(camera);
		REQUIRE(camera.HasComponent<CameraComponent>());
		const CameraComponent& cameraComponent = camera.GetComponent<CameraComponent>();
		CHECK_FALSE(cameraComponent.Primary);
		CHECK(cameraComponent.PerspectiveFOV == doctest::Approx(glm::degrees(0.8f)));
		CHECK(cameraComponent.PerspectiveNear == doctest::Approx(0.05f));
		CHECK(cameraComponent.PerspectiveFar == doctest::Approx(500.0f));
		CHECK(std::abs(glm::dot(camera.GetComponent<TransformComponent>().Rotation, glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 1, 0)))) == doctest::Approx(1.0f).epsilon(1e-4));

		Entity stripEntity = FindChild(root, "Strip");
		REQUIRE(stripEntity);
		const TransformComponent& stripTransform = stripEntity.GetComponent<TransformComponent>();
		CHECK(stripTransform.Translation.x == doctest::Approx(5.0f));
		CHECK(stripTransform.Scale.x == doctest::Approx(2.0f));
		CHECK(stripEntity.GetComponent<MeshRendererComponent>().Mesh == output.HandleOf("Mesh/1"));
	}

	TEST_CASE("Binary glTF files import like text files and imports are deterministic")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfBinary");
		const ImportOutput first = RunImport(assets, "Sample.glb", CreateGlb({ .ImageUri = "embedded" }));
		REQUIRE_MESSAGE(first.Success, first.Error);
		CHECK(first.Find("Mesh/0"));
		CHECK(first.Find("Texture/0/Color"));

		const ImportOutput second = RunImport(assets, "Sample.glb", CreateGlb({ .ImageUri = "embedded" }));
		REQUIRE(second.Success);
		CHECK(second.Result.Data == first.Result.Data);
		REQUIRE(second.Result.SubAssets.size() == first.Result.SubAssets.size());
		for (size_t index = 0; index < first.Result.SubAssets.size(); index++)
		{
			CHECK(second.Result.SubAssets[index].Key == first.Result.SubAssets[index].Key);
			CHECK(second.Result.SubAssets[index].Data == first.Result.SubAssets[index].Data);
		}
	}

	TEST_CASE("glTF import settings control scale, LODs, cameras and lights")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfSettings");
		const ImportOutput output = RunImport(assets, "Sample.gltf", CreateGltf({}),
			{ { "Scale", 0.01 }, { "GenerateLODs", false }, { "ImportCameras", false }, { "ImportLights", false }, { "ImportMaterials", false } });
		REQUIRE_MESSAGE(output.Success, output.Error);
		CHECK_FALSE(output.Find("Material/"));

		Ref<Mesh> quad = Mesh::Deserialize(output.Find("Mesh/0")->Data);
		REQUIRE(quad);
		CHECK(quad->GetSubmeshes()[0].LODs.size() == 1);
		CHECK(quad->GetSubmeshes()[0].Material == BuiltinAssets::DefaultMaterial);

		Entity root;
		Ref<Scene> scene = InstantiateModel(output, root);
		CHECK(root.GetComponent<TransformComponent>().Scale == glm::vec3(0.01f));
		CHECK_FALSE(FindChild(root, "Cam").HasComponent<CameraComponent>());
		CHECK_FALSE(FindChild(FindChild(root, "Body"), "Lamp").HasComponent<PointLightComponent>());

		const ImportOutput badScale = RunImport(assets, "Sample.gltf", CreateGltf({}), { { "Scale", -2.0 } });
		REQUIRE(badScale.Success);
		CHECK_FALSE(badScale.Result.Warnings.empty());
	}

	TEST_CASE("Referenced files must be inside the asset directory")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("GltfFiles");
		const std::filesystem::path assets = root / "Assets";
		REQUIRE(FileSystem::WriteBytes(assets / "Models" / "Textures" / "Albedo.png", Tests::CreateSolidPNG(2, 2, 10, 20, 30)));
		REQUIRE(FileSystem::WriteBytes(root / "Outside.png", Tests::CreateSolidPNG(2, 2, 10, 20, 30)));
		REQUIRE(FileSystem::WriteBytes(assets / "Models" / "Geometry.bin", CreateGeometryBuffer()));
		REQUIRE(FileSystem::WriteBytes(root / "Outside.bin", CreateGeometryBuffer()));

		// External buffer and image next to the model.
		const ImportOutput external = RunImport(assets, "Models/External.gltf", CreateGltf({ .ImageUri = "Textures/Albedo.png", .BufferUri = "Geometry.bin" }));
		REQUIRE_MESSAGE(external.Success, external.Error);
		CHECK(external.Find("Texture/0/Color"));

		// An image outside the asset directory is skipped with a warning; the model still imports.
		const ImportOutput outsideImage = RunImport(assets, "Models/OutsideImage.gltf", CreateGltf({ .ImageUri = "../../Outside.png" }));
		REQUIRE_MESSAGE(outsideImage.Success, outsideImage.Error);
		CHECK_FALSE(outsideImage.Find("Texture/"));
		CHECK_FALSE(outsideImage.Result.Warnings.empty());

		// A buffer outside the asset directory fails the import.
		const ImportOutput outsideBuffer = RunImport(assets, "Models/OutsideBuffer.gltf", CreateGltf({ .BufferUri = "../../Outside.bin" }));
		CHECK_FALSE(outsideBuffer.Success);
		CHECK(outsideBuffer.Error.find("OutsideBuffer.gltf") != std::string::npos);
	}

	TEST_CASE("Invalid and unsupported glTF files are rejected")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfInvalid");

		const ImportOutput draco = RunImport(assets, "Draco.gltf", CreateGltf({ .ExtensionsRequired = { "KHR_draco_mesh_compression" } }));
		CHECK_FALSE(draco.Success);
		CHECK(draco.Error.find("KHR_draco_mesh_compression") != std::string::npos);

		const std::string notJson = "{ \"asset\": ";
		CHECK_FALSE(RunImport(assets, "Broken.gltf", std::vector<uint8_t>(notJson.begin(), notJson.end())).Success);
		const std::string legacy = "{ \"asset\": { \"version\": \"1.0\" } }";
		CHECK_FALSE(RunImport(assets, "Legacy.gltf", std::vector<uint8_t>(legacy.begin(), legacy.end())).Success);

		const std::vector<uint8_t> glb = CreateGlb({});
		for (size_t length = 0; length < glb.size(); length += 13)
		{
			CAPTURE(length);
			const ImportOutput truncated = RunImport(assets, "Truncated.glb", std::vector<uint8_t>(glb.begin(), glb.begin() + static_cast<std::ptrdiff_t>(length)));
			CHECK_FALSE(truncated.Success);
		}

		// Out-of-range accessors are caught by validation.
		const std::vector<uint8_t> text = CreateGltf({});
		std::optional<nlohmann::json> parsed = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
		REQUIRE(parsed);
		nlohmann::json document = *parsed;
		document["accessors"][0]["count"] = 1000;
		const std::string tooShort = JsonUtils::Dump(document);
		CHECK_FALSE(RunImport(assets, "TooShort.gltf", std::vector<uint8_t>(tooShort.begin(), tooShort.end())).Success);
	}

	TEST_CASE("External buffers must hold their declared length and are reported as dependencies")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfExternal");
		std::vector<uint8_t> geometry = CreateGeometryBuffer();
		const std::vector<uint8_t> model = CreateGltf({ .ImageUri = "Albedo.png", .BufferUri = "Geometry.bin" });
		REQUIRE(FileSystem::WriteBytes(assets / "Albedo.png", Tests::CreateSolidPNG(2, 2, 10, 20, 30)));

		// Longer than declared: only the declared bytes are used.
		std::vector<uint8_t> padded = geometry;
		padded.resize(padded.size() + 64, 0xCD);
		REQUIRE(FileSystem::WriteBytes(assets / "Geometry.bin", padded));
		const ImportOutput valid = RunImport(assets, "Model.gltf", model);
		REQUIRE_MESSAGE(valid.Success, valid.Error);
		REQUIRE(valid.Result.Dependencies.size() == 2);
		const ImportDependency& buffer = valid.Result.Dependencies[0];
		CHECK(buffer.Path == (assets / "Geometry.bin").lexically_normal());
		CHECK(buffer.Exists);
		CHECK(buffer.Size == padded.size());
		CHECK(buffer.Hash == Hash::FNV1a(std::span<const uint8_t>(padded)));
		CHECK(valid.Result.Dependencies[1].Path == (assets / "Albedo.png").lexically_normal());

		// Shorter than declared: the import fails instead of reading past the data.
		geometry.resize(geometry.size() - 4);
		REQUIRE(FileSystem::WriteBytes(assets / "Geometry.bin", geometry));
		const ImportOutput truncated = RunImport(assets, "Model.gltf", model);
		CHECK_FALSE(truncated.Success);
		CHECK(truncated.Error.find("data too short") != std::string::npos);

		// A missing file is a dependency too: creating it must retry the import.
		REQUIRE(FileSystem::Remove(assets / "Geometry.bin"));
		const ImportOutput missing = RunImport(assets, "Model.gltf", model);
		CHECK_FALSE(missing.Success);
		REQUIRE(missing.Result.Dependencies.size() == 1);
		CHECK_FALSE(missing.Result.Dependencies[0].Exists);

		// A link leading out of the asset directory is not read, and never counts as current (retried).
		REQUIRE(FileSystem::WriteBytes(assets.parent_path() / "OutsideGeometry.bin", CreateGeometryBuffer()));
		std::error_code error;
		std::filesystem::create_symlink(assets.parent_path() / "OutsideGeometry.bin", assets / "Geometry.bin", error);
		if (!error)
		{
			const ImportOutput linked = RunImport(assets, "Model.gltf", model);
			CHECK_FALSE(linked.Success);
			REQUIRE(linked.Result.Dependencies.size() == 1);
			CHECK(linked.Result.Dependencies[0].Exists);
			CHECK(linked.Result.Dependencies[0].Size == ImportDependency::c_Unreadable);
		}
	}

	TEST_CASE("Accessor and buffer view layouts are validated without overflow")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfLayout");
		const nlohmann::json base = ParseDocument(CreateGltf({}));
		auto importModified = [&](const std::function<void(nlohmann::json&)>& modify)
		{
			nlohmann::json document = base;
			modify(document);
			return RunImport(assets, "Layout.gltf", ToBytes(document));
		};

		// Counts whose byte size wraps around 64 bits.
		ImportOutput result = importModified([](nlohmann::json& document) { document["accessors"][0]["count"] = 4611686018427387904ull; });
		CHECK_FALSE(result.Success);
		result = importModified([](nlohmann::json& document) { document["accessors"][0]["byteOffset"] = 4611686018427387904ull; });
		CHECK_FALSE(result.Success);
		result = importModified([](nlohmann::json& document) { document["bufferViews"][0]["byteOffset"] = 4611686018427387904ull; });
		CHECK_FALSE(result.Success);

		// Strides must be multiples of 4 between 4 and 252.
		result = importModified([](nlohmann::json& document) { document["bufferViews"][0]["byteStride"] = 6; });
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("stride") != std::string::npos);
		result = importModified([](nlohmann::json& document) { document["bufferViews"][0]["byteStride"] = 16; });
		CHECK_FALSE(result.Success); // 4 elements at stride 16 need 60 bytes of the 48-byte view

		// Accessors without a buffer view read as zeros: a few bytes of JSON could describe gigabytes of geometry.
		result = importModified([](nlohmann::json& document)
		{
			document["accessors"].push_back({ { "componentType", 5126 }, { "count", 1 << 23 }, { "type", "VEC3" } });
			document["meshes"][1]["primitives"][0]["attributes"]["POSITION"] = document["accessors"].size() - 1;
		});
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("more geometry") != std::string::npos);
		// Unused, they cost nothing and are fine.
		result = importModified([](nlohmann::json& document)
		{
			document["accessors"].push_back({ { "componentType", 5126 }, { "count", 1 << 23 }, { "type", "VEC3" } });
		});
		CHECK_MESSAGE(result.Success, result.Error);
		// Many primitives sharing one accessor expand it once each.
		result = importModified([](nlohmann::json& document)
		{
			document["accessors"].push_back({ { "componentType", 5126 }, { "count", 1 << 18 }, { "type", "VEC3" } });
			const size_t shared = document["accessors"].size() - 1;
			nlohmann::json primitives = nlohmann::json::array();
			for (int primitive = 0; primitive < 32; primitive++)
				primitives.push_back({ { "attributes", { { "POSITION", shared } } } });
			document["meshes"][1]["primitives"] = primitives;
		});
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("more geometry") != std::string::npos);

		// cgltf would read sparse values with the stride of an interleaved base view, past their data.
		result = importModified([](nlohmann::json& document)
		{
			document["bufferViews"].push_back({ { "buffer", 0 }, { "byteOffset", 0 }, { "byteLength", 48 }, { "byteStride", 16 } });
			document["accessors"].push_back({ { "bufferView", document["bufferViews"].size() - 1 }, { "componentType", 5126 }, { "count", 3 }, { "type", "VEC3" },
				{ "sparse", { { "count", 3 }, { "indices", { { "bufferView", 3 }, { "componentType", 5123 } } }, { "values", { { "bufferView", 1 } } } } } });
			document["meshes"][1]["primitives"][0]["attributes"]["POSITION"] = document["accessors"].size() - 1;
		});
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("interleaved") != std::string::npos);

		// Sparse accessors are bounded like dense ones.
		result = importModified([](nlohmann::json& document)
		{
			document["accessors"][1]["sparse"] = { { "count", 100 }, { "indices", { { "bufferView", 3 }, { "componentType", 5123 } } }, { "values", { { "bufferView", 1 } } } };
		});
		CHECK_FALSE(result.Success);
	}

	TEST_CASE("Node hierarchies are bounded, cycle-free and imported once per node")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfHierarchy");
		const nlohmann::json base = ParseDocument(CreateGltf({}));
		auto createChain = [&](size_t depth)
		{
			nlohmann::json document = base;
			document["scenes"][0]["nodes"] = { 0 };
			document["nodes"] = nlohmann::json::array();
			for (size_t index = 0; index < depth; index++)
			{
				nlohmann::json node = { { "name", "N" + std::to_string(index) } };
				if (index + 1 < depth)
					node["children"] = { index + 1 };
				document["nodes"].push_back(std::move(node));
			}
			return document;
		};

		// Deep files fail cleanly instead of exhausting the stack.
		const ImportOutput deep = RunImport(assets, "Deep.gltf", ToBytes(createChain(5000)));
		CHECK_FALSE(deep.Success);
		CHECK(deep.Error.find("deeper") != std::string::npos);

		const ImportOutput chain = RunImport(assets, "Chain.gltf", ToBytes(createChain(64)));
		REQUIRE_MESSAGE(chain.Success, chain.Error);
		Entity root;
		Ref<Scene> chainScene = InstantiateModel(chain, root);
		Entity node = root;
		for (size_t depth = 0; depth < 64; depth++)
		{
			REQUIRE(node.GetChildren().size() == 1);
			node = node.GetChildren()[0];
			CHECK(node.GetName() == "N" + std::to_string(depth));
		}

		nlohmann::json cyclic = base;
		cyclic["nodes"][1]["children"] = { 0 };
		const ImportOutput cycle = RunImport(assets, "Cycle.gltf", ToBytes(cyclic));
		CHECK_FALSE(cycle.Success);

		// A scene listing a root twice imports it once (cgltf rejects nodes with several parents itself).
		nlohmann::json repeated = base;
		repeated["scenes"][0]["nodes"] = { 0, 2, 3, 0 };
		const ImportOutput duplicates = RunImport(assets, "Repeated.gltf", ToBytes(repeated));
		REQUIRE_MESSAGE(duplicates.Success, duplicates.Error);
		CHECK(HasWarningContaining(duplicates.Result, "more than once"));
		Ref<Scene> scene = InstantiateModel(duplicates, root);
		CHECK(root.GetChildren().size() == 3);
		CHECK(FindChild(FindChild(root, "Body"), "Lamp"));
		CHECK_FALSE(FindChild(root, "Lamp"));
	}

	TEST_CASE("Files with more nodes than a scene holds are rejected")
	{
		// Empty nodes, all at the top level (without a scene every root node is part of the model): with the model's root
		// entity, one more entity than a scene holds.
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfNodeCount");
		std::string document = R"({"asset":{"version":"2.0"},"nodes":[{})";
		document.reserve(document.size() + Scene::c_MaxEntities * 3 + 2);
		for (size_t index = 1; index < Scene::c_MaxEntities; index++)
			document += ",{}";
		document += "]}";
		const ImportOutput output = RunImport(assets, "Crowd.gltf", std::vector<uint8_t>(document.begin(), document.end()));
		CHECK_FALSE(output.Success);
		CHECK(output.Error == "Crowd.gltf: the file has 1048575 nodes; a model holds at most 1048574");
	}

	TEST_CASE("Missing normals become flat normals and tangents follow UV seams")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfShading");

		// A folded quad without normals: both triangles get their own face normal.
		const std::vector<glm::vec3> folded = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 1 }, { 0, 1, 0 } };
		const std::vector<glm::vec2> uvs = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		const std::vector<uint16_t> quadIndices = { 0, 1, 2, 0, 2, 3 };
		for (bool withTexCoords : { true, false })
		{
			CAPTURE(withTexCoords);
			const ImportOutput output = RunImport(assets, "Folded.gltf", CreatePrimitiveModel(folded, {}, withTexCoords ? uvs : std::vector<glm::vec2>(), quadIndices));
			REQUIRE_MESSAGE(output.Success, output.Error);
			Ref<Mesh> mesh = Mesh::Deserialize(output.Find("Mesh/0")->Data);
			REQUIRE(mesh);
			CHECK(mesh->GetPositions().size() == 6); // The shared edge is split
			const std::vector<uint32_t>& indices = mesh->GetIndices();
			for (size_t corner = 0; corner < mesh->GetSubmeshes()[0].LODs[0].IndexCount; corner++)
			{
				const MeshVertexAttributes& attribute = mesh->GetAttributes()[indices[corner]];
				CHECK(glm::dot(attribute.Normal, FaceNormal(*mesh, corner - corner % 3)) == doctest::Approx(1.0f));
				CHECK(glm::abs(glm::dot(glm::vec3(attribute.Tangent), attribute.Normal)) < 1e-4f);
			}
		}

		// Mirrored UVs: the second triangle's U runs along -X. Vertices on the mirror line need both tangents.
		const std::vector<glm::vec3> flat = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } };
		const std::vector<glm::vec3> normals(4, glm::vec3(0, 0, 1));
		const std::vector<glm::vec2> mirrored = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } };
		const ImportOutput output = RunImport(assets, "Mirrored.gltf", CreatePrimitiveModel(flat, normals, mirrored, quadIndices));
		REQUIRE_MESSAGE(output.Success, output.Error);
		Ref<Mesh> mesh = Mesh::Deserialize(output.Find("Mesh/0")->Data);
		REQUIRE(mesh);
		const std::vector<uint32_t>& indices = mesh->GetIndices();
		const std::vector<glm::vec3>& positions = mesh->GetPositions();
		for (size_t corner = 0; corner < 6; corner++)
		{
			CAPTURE(corner);
			const MeshVertexAttributes& attribute = mesh->GetAttributes()[indices[corner]];
			// The triangle containing (1, 0) has U along +X; the other one along -X with flipped handedness.
			const size_t first = corner - corner % 3;
			bool hasBottomRight = false;
			for (size_t vertex = first; vertex < first + 3; vertex++)
				hasBottomRight = hasBottomRight || positions[indices[vertex]] == glm::vec3(1, 0, 0);
			CHECK(attribute.Tangent.x == doctest::Approx(hasBottomRight ? 1.0f : -1.0f));
			CHECK(attribute.Tangent.w == doctest::Approx(hasBottomRight ? 1.0f : -1.0f));
		}
	}

	TEST_CASE("Only PNG and JPEG images on TEXCOORD_0 are imported")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfImages");

		// Other decodable formats are not exposed to model files.
		const std::vector<uint8_t> gif = { 'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0, 0, 0, ',', 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 'D', 1, 0, ';' };
		const ImportOutput other = RunImport(assets, "Gif.gltf", CreateGltf({ .ImageUri = "data:image/png;base64," + EncodeBase64(gif) }));
		REQUIRE_MESSAGE(other.Success, other.Error);
		CHECK_FALSE(other.Find("Texture/"));
		CHECK(HasWarningContaining(other.Result, "not PNG or JPEG"));

		// Textures on other coordinate sets are skipped with a warning; the rest of the material imports.
		nlohmann::json document = ParseDocument(CreateGltf({ .ImageUri = "embedded" }));
		document["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]["texCoord"] = 1;
		const ImportOutput secondSet = RunImport(assets, "TexCoord1.gltf", ToBytes(document));
		REQUIRE_MESSAGE(secondSet.Success, secondSet.Error);
		CHECK_FALSE(secondSet.Find("Texture/0/Color"));
		CHECK(secondSet.Find("Texture/0/Data"));
		CHECK(HasWarningContaining(secondSet.Result, "TEXCOORD_1"));

		// KHR_texture_transform can select the coordinate set instead.
		document = ParseDocument(CreateGltf({ .ImageUri = "embedded" }));
		document["extensionsUsed"].push_back("KHR_texture_transform");
		document["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]["extensions"] = { { "KHR_texture_transform", { { "texCoord", 2 } } } };
		const ImportOutput transformed = RunImport(assets, "TransformSet.gltf", ToBytes(document));
		REQUIRE_MESSAGE(transformed.Success, transformed.Error);
		CHECK_FALSE(transformed.Find("Texture/0/Color"));
		CHECK(HasWarningContaining(transformed.Result, "TEXCOORD_2"));
	}

	TEST_CASE("The import scale applies to camera clip planes and light ranges")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfScale");
		nlohmann::json document = ParseDocument(CreateGltf({}));
		document["cameras"].push_back({ { "type", "orthographic" }, { "orthographic", { { "xmag", 4.0 }, { "ymag", 3.0 }, { "znear", 0.5 }, { "zfar", 50.0 } } } });
		document["cameras"].push_back({ { "type", "perspective" }, { "perspective", { { "yfov", 1.0 }, { "znear", 0.25 } } } }); // Infinite
		document["nodes"].push_back({ { "name", "Ortho" }, { "camera", 1 } });
		document["nodes"].push_back({ { "name", "Infinite" }, { "camera", 2 } });
		document["scenes"][0]["nodes"].push_back(4);
		document["scenes"][0]["nodes"].push_back(5);
		document["extensions"]["KHR_lights_punctual"]["lights"][0]["range"] = 7.0;
		document["extensions"]["KHR_lights_punctual"]["lights"].push_back({ { "type", "point" }, { "intensity", 4.0 } }); // No range
		document["nodes"].push_back({ { "name", "Lamp2" }, { "extensions", { { "KHR_lights_punctual", { { "light", 1 } } } } } });
		document["scenes"][0]["nodes"].push_back(6);

		const ImportOutput output = RunImport(assets, "Scaled.gltf", ToBytes(document), { { "Scale", 2.0 } });
		REQUIRE_MESSAGE(output.Success, output.Error);
		Entity root;
		Ref<Scene> scene = InstantiateModel(output, root);

		const Entity perspectiveCamera = FindChild(root, "Cam");
		const CameraComponent& perspective = perspectiveCamera.GetComponent<CameraComponent>();
		CHECK(perspective.PerspectiveNear == doctest::Approx(0.1f));
		CHECK(perspective.PerspectiveFar == doctest::Approx(1000.0f));
		const Entity orthographicCamera = FindChild(root, "Ortho");
		const CameraComponent& orthographic = orthographicCamera.GetComponent<CameraComponent>();
		CHECK(orthographic.Projection == ProjectionType::Orthographic);
		CHECK(orthographic.OrthographicSize == doctest::Approx(12.0f));
		CHECK(orthographic.OrthographicNear == doctest::Approx(1.0f));
		CHECK(orthographic.OrthographicFar == doctest::Approx(100.0f));
		const Entity infiniteCamera = FindChild(root, "Infinite");
		const CameraComponent& infinite = infiniteCamera.GetComponent<CameraComponent>();
		CHECK(infinite.PerspectiveNear == doctest::Approx(0.5f));
		CHECK(infinite.PerspectiveFar >= 500.0f);
		CHECK(std::isfinite(infinite.PerspectiveFar));

		CHECK(FindChild(FindChild(root, "Body"), "Lamp").GetComponent<PointLightComponent>().Range == doctest::Approx(14.0f));
		// A derived range is where the unscaled intensity fades out: world units, not scaled.
		CHECK(FindChild(root, "Lamp2").GetComponent<PointLightComponent>().Range == doctest::Approx(20.0f));

		// Tiny scales keep the far plane beyond the clamped near plane.
		nlohmann::json tiny = ParseDocument(CreateGltf({}));
		tiny["cameras"][0]["perspective"]["znear"] = 0.05;
		tiny["cameras"][0]["perspective"]["zfar"] = 0.08;
		const ImportOutput tinyOutput = RunImport(assets, "Tiny.gltf", ToBytes(tiny), { { "Scale", 0.01 } });
		REQUIRE_MESSAGE(tinyOutput.Success, tinyOutput.Error);
		Ref<Scene> tinyScene = InstantiateModel(tinyOutput, root);
		const Entity tinyCameraEntity = FindChild(root, "Cam");
		const CameraComponent& tinyCamera = tinyCameraEntity.GetComponent<CameraComponent>();
		CHECK(tinyCamera.PerspectiveFar > tinyCamera.PerspectiveNear);
	}

	TEST_CASE("Draco-compressed primitives import their uncompressed fallback")
	{
		const std::filesystem::path assets = Tests::CreateTemporaryDirectory("GltfDraco");
		nlohmann::json document = ParseDocument(CreateGltf({}));
		document["extensionsUsed"].push_back("KHR_draco_mesh_compression");
		document["meshes"][0]["primitives"][0]["extensions"] = { { "KHR_draco_mesh_compression", { { "bufferView", 2 }, { "attributes", { { "POSITION", 0 } } } } } };

		const ImportOutput fallback = RunImport(assets, "Fallback.gltf", ToBytes(document));
		REQUIRE_MESSAGE(fallback.Success, fallback.Error);
		CHECK(fallback.Find("Mesh/0"));
		CHECK(HasWarningContaining(fallback.Result, "KHR_draco_mesh_compression"));

		// Without fallback data (no buffer view on the positions) the primitive is skipped.
		document["accessors"][0].erase("bufferView");
		const ImportOutput compressedOnly = RunImport(assets, "Compressed.gltf", ToBytes(document));
		REQUIRE_MESSAGE(compressedOnly.Success, compressedOnly.Error);
		CHECK(HasWarningContaining(compressedOnly.Result, "Draco"));
		CHECK(compressedOnly.Find("Mesh/1")); // The other mesh is unaffected
	}
}
