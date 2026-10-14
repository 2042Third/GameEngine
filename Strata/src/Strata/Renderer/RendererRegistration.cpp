#include "stpch.h"
#include "Strata/Renderer/RendererRegistration.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/GPUProfiler.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/RendererUploadContext.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Renderer/TextureStreamer.h"

namespace Strata
{

	namespace
	{

		void RegisterBuiltinAsset(AssetHandle handle, BuiltinAssetFactory factory)
		{
			const bool registered = BuiltinAssets::RegisterFactory(handle, std::move(factory));
			ST_CORE_VERIFY(registered, "The renderer could not provide built-in asset {}", handle.ToString());
		}

	}

	void RegisterRendererModule()
	{
		// The asset layer finalizes GPU assets through this service: the running renderer's upload context, frames in flight
		// and memory budget (Asset/AssetUploadContext.h).
		EngineServices::Register<IAssetGraphics>(RendererAssetGraphics::Get());
		// The GPU stages, before anything reads the frame profiler's stages (a perf log's columns).
		RendererGPUStages::Get();

		// Textures and meshes are stored in their cooked binary form, materials and fonts as their source. Textures and
		// fonts keep their bytes: they take them over instead of copying them. Block-compressed textures are decoded here,
		// on the loading worker, for graphics devices that cannot sample them. A first load of a texture reads its header and
		// mip tail only (its prefix), and the texture streamer each manager gets reads finer levels as views need them.
		AssetLoaderRegistry::Register(AssetType::Texture, [](const AssetMetadata&, AssetLoadData& data, std::string* outError) -> Ref<Asset>
		{
			const uint64_t storedSize = data.GetStoredSize();
			Ref<Texture> texture = Texture::Deserialize(data.TakeBytes(), storedSize, outError);
			if (texture && IsBlockCompressedFormat(texture->GetSpecification().Format) && !Texture::IsBlockCompressionSupported())
				return texture->Decompress(outError);
			return texture;
		});
		AssetLoaderRegistry::RegisterPrefix(AssetType::Texture, [](std::span<const uint8_t> head, uint64_t storedSize)
		{
			return Texture::GetLoadPrefixSize(head, storedSize);
		});
		AssetManagerExtensionRegistry::Register([](AssetManagerBase& manager) -> Scope<AssetManagerExtension>
		{
			return CreateScope<TextureStreamer>(manager);
		});
		AssetLoaderRegistry::Register(AssetType::Mesh, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return Mesh::Deserialize(data, outError);
		});
		AssetLoaderRegistry::Register(AssetType::Material, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return Material::Deserialize(data, outError);
		});
		AssetLoaderRegistry::Register(AssetType::Font, [](const AssetMetadata&, AssetLoadData& data, std::string* outError) -> Ref<Asset>
		{
			return Font::Create(data.TakeBytes(), outError);
		});

		// The primitive meshes use the default material.
		RegisterBuiltinAsset(BuiltinAssets::DefaultMaterial, []() -> Ref<Asset> { return Material::Create(); });
		RegisterBuiltinAsset(BuiltinAssets::CubeMesh, []() -> Ref<Asset> { return MeshFactory::CreateCube(1.0f, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::SphereMesh, []() -> Ref<Asset> { return MeshFactory::CreateSphere(0.5f, 48, 24, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::PlaneMesh, []() -> Ref<Asset> { return MeshFactory::CreatePlane(1.0f, 1, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::QuadMesh, []() -> Ref<Asset> { return MeshFactory::CreateQuad(1.0f, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::CylinderMesh, []() -> Ref<Asset> { return MeshFactory::CreateCylinder(0.5f, 1.0f, 48, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::CapsuleMesh, []() -> Ref<Asset> { return MeshFactory::CreateCapsule(0.5f, 0.5f, 48, 12, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::ConeMesh, []() -> Ref<Asset> { return MeshFactory::CreateCone(0.5f, 1.0f, 48, BuiltinAssets::DefaultMaterial); });
		RegisterBuiltinAsset(BuiltinAssets::TorusMesh, []() -> Ref<Asset> { return MeshFactory::CreateTorus(0.375f, 0.125f, 48, 24, BuiltinAssets::DefaultMaterial); });
	}

}
