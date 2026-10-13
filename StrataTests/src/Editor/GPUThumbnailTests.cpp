#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ProjectThumbnail.h"
#include "Editor/ThumbnailCache.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Panels/WelcomePanel.h"
#include "Renderer/GPUTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/ImageReader.h>
#include <Strata/Renderer/Renderer.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;

namespace
{

	struct ThumbnailHarness
	{
		Tests::GPUContext& GPU;
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		std::filesystem::path Project;

		ThumbnailHarness(Tests::GPUContext& gpu, const std::filesystem::path& project)
			: GPU(gpu), Project(project)
		{
			Run("project.open", { { "path", FileSystem::ToUTF8(Project) } });
			REQUIRE(Context.GetThumbnails());
		}

		// Runs a command; one that answers in a later frame (project.open opens its start scene in the background) is completed
		// over editor frames, as the runner completes it.
		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::RunCommandToCompletion(Context, Commands, name, parameters);
		}

		AssetHandle Find(const std::string& path)
		{
			const AssetHandle handle = Context.GetAssetManager()->FindAssetByPath(path);
			REQUIRE_MESSAGE(handle.IsValid(), path);
			return handle;
		}

		ThumbnailCache& Thumbnails() { return *Context.GetThumbnails(); }

		// One editor frame on the device, as the application runs it.
		void Frame()
		{
			REQUIRE(GPU.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			Context.Update(Timestep(1.0f / 60.0f));
			GPU.GetDevice().EndFrame();
		}

		// Asks for the thumbnails every frame (as the Content Browser does while they are on screen) until their work ended.
		void RequestUntilIdle(const std::vector<AssetHandle>& handles)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
			do
			{
				for (AssetHandle handle : handles)
					Thumbnails().Request(handle);
				Frame();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			while (Thumbnails().IsBusy() && std::chrono::steady_clock::now() < deadline);
			REQUIRE_FALSE(Thumbnails().IsBusy());
			for (AssetHandle handle : handles)
				Thumbnails().Request(handle);
		}

		// Frames until the index of the thumbnail files is written.
		void WaitForIndex()
		{
			const std::filesystem::path index = Project / ".strata" / "Thumbnails" / "Index.json";
			for (int frame = 0; frame < 2000; frame++)
			{
				Frame();
				if (const std::optional<std::string> text = FileSystem::ReadText(index); text && text->find("Keys") != std::string::npos)
					return;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			FAIL("The thumbnail index was not written");
		}
	};

	// The center and a corner pixel of a thumbnail's texture.
	std::pair<glm::u8vec4, glm::u8vec4> ReadCenterAndCorner(nvrhi::ITexture* texture)
	{
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(texture, image));
		return { Tests::GetPixelRGBA8(image, image.Width / 2, image.Height / 2), Tests::GetPixelRGBA8(image, 1, 1) };
	}

}

TEST_SUITE("GPU.Editor.Thumbnails")
{
	TEST_CASE("Thumbnails render from mip tails and small scenes, are kept on disk, and the next session reads them back")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path project = Tests::CopyFeatureProject(Tests::CreateTemporaryDirectory("GPUThumbnails") / "Project").parent_path();
		// A texture whose mip chain is far larger than the levels a thumbnail reads.
		REQUIRE(FileSystem::WriteBytes(project / "Assets" / "Textures" / "Large.png", Tests::CreateSolidPNG(1024, 1024, 40, 90, 200)));
		// And one without mips.
		REQUIRE(FileSystem::WriteBytes(project / "Assets" / "Textures" / "Wide.png", Tests::CreateSolidPNG(512, 256, 200, 90, 40)));
		REQUIRE(FileSystem::WriteText(project / "Assets" / "Textures" / "Wide.png.meta", "{ \"Handle\": \"57AD57AD57AD57AD\", \"ImportSettings\": { \"GenerateMips\": false } }"));
		std::vector<AssetHandle> all;
		{
			ThumbnailHarness harness(gpu, project);
			EditorAssetManager& assets = *harness.Context.GetAssetManager();
			const AssetHandle checker = harness.Find("Textures/Checker.png");
			const AssetHandle sky = harness.Find("Textures/Sky.hdr");
			const AssetHandle large = harness.Find("Textures/Large.png");
			const AssetHandle material = harness.Find("Materials/Feature.stmat");
			const AssetHandle model = harness.Find("Models/Platform.gltf");
			const AssetHandle prefab = harness.Find("Prefabs/Crate.stprefab");
			AssetHandle mesh = UUID::Null();
			for (AssetHandle subAsset : assets.GetImportInfo(model).SubAssets)
			{
				if (!mesh.IsValid() && assets.GetAssetType(subAsset) == AssetType::Mesh)
					mesh = subAsset;
			}
			REQUIRE(mesh.IsValid());

			// Textures: from the mip tail, never loading the texture. The start scene's load brought in the textures it uses
			// (Checker, Sky); the thumbnails change no load state, and the one no scene uses stays unloaded.
			const AssetState checkerState = assets.GetAssetState(checker);
			const AssetState skyState = assets.GetAssetState(sky);
			harness.RequestUntilIdle({ checker, sky, large });
			for (AssetHandle texture : { checker, sky, large })
			{
				CHECK(harness.Thumbnails().GetState(texture) == ThumbnailState::Ready);
				nvrhi::ITexture* thumbnail = harness.Thumbnails().Request(texture);
				REQUIRE(thumbnail);
				CHECK(thumbnail->getDesc().width <= ThumbnailCache::c_Size);
				CHECK(thumbnail->getDesc().height <= ThumbnailCache::c_Size);
			}
			CHECK(assets.GetAssetState(checker) == checkerState);
			CHECK(assets.GetAssetState(sky) == skyState);
			CHECK(assets.GetAssetState(large) == AssetState::Unloaded);
			const ThumbnailCacheStats textures = harness.Thumbnails().GetStats();
			CHECK(textures.TexturesDecoded == 3);
			// The large texture's header and levels up to 128x128 (a sixteenth of the chain and less), the small ones whole.
			CHECK(textures.StoredBytesRead < assets.GetMetadata(large)->StoredSize / 10);

			// A texture without mips has no level that small: its one level is read (after the header) and made smaller.
			const AssetHandle wide = harness.Find("Textures/Wide.png");
			harness.RequestUntilIdle({ wide });
			CHECK(harness.Thumbnails().GetState(wide) == ThumbnailState::Ready);
			REQUIRE(harness.Thumbnails().Request(wide));
			CHECK(harness.Thumbnails().Request(wide)->getDesc().width == ThumbnailCache::c_Size);
			CHECK(harness.Thumbnails().Request(wide)->getDesc().height == ThumbnailCache::c_Size / 2);
			CHECK(assets.GetAssetState(wide) == AssetState::Unloaded);
			CHECK(harness.Thumbnails().GetStats().StoredBytesRead - textures.StoredBytesRead <= assets.GetMetadata(wide)->StoredSize + 4096);

			// Rendered: a material on a sphere, a mesh, a model and a prefab.
			harness.RequestUntilIdle({ material, mesh, model, prefab });
			for (AssetHandle rendered : { material, mesh, model, prefab })
			{
				CAPTURE(rendered.ToString());
				CHECK(harness.Thumbnails().GetState(rendered) == ThumbnailState::Ready);
				nvrhi::ITexture* thumbnail = harness.Thumbnails().Request(rendered);
				REQUIRE(thumbnail);
				CHECK(thumbnail->getDesc().width == ThumbnailCache::c_Size);
				// Something stands in front of the sky.
				const auto [center, corner] = ReadCenterAndCorner(thumbnail);
				CHECK(center.a == 255);
				CHECK(center != corner);
			}
			CHECK(harness.Thumbnails().GetStats().ScenesRendered == 4);
			CHECK(harness.Thumbnails().GetStats().Failed == 0);

			// Kept on disk with their keys.
			harness.WaitForIndex();
			all = { checker, sky, large, wide, material, mesh, model, prefab };
			for (AssetHandle handle : all)
				CHECK(FileSystem::IsRegularFile(project / ".strata" / "Thumbnails" / (UUIDToJson(handle).get<std::string>() + ".png")));
			CHECK(harness.Thumbnails().GetStats().FilesWritten == all.size());
		}
		{
			// The next session reads them instead of making them again.
			ThumbnailHarness harness(gpu, project);
			harness.RequestUntilIdle(all);
			const ThumbnailCacheStats stats = harness.Thumbnails().GetStats();
			CHECK(stats.LoadedFromDisk == all.size());
			CHECK(stats.ScenesRendered == 0);
			CHECK(stats.TexturesDecoded == 0);
			for (AssetHandle handle : all)
				CHECK(harness.Thumbnails().Request(handle) != nullptr);
		}
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A rendered thumbnail waits for the mip levels its textures stream in, not only for their tails")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path project = Tests::CopyFeatureProject(Tests::CreateTemporaryDirectory("GPUThumbnailMips") / "Project").parent_path();
		// Black and white stripes 16 texels wide: each texel of the 32x32 mip tail covers one black and one white stripe and is
		// gray, while the levels a 128-pixel sphere needs keep them apart.
		constexpr uint32_t size = 1024;
		std::vector<uint8_t> pixels(size * size * 4, 255);
		for (uint32_t y = 0; y < size; y++)
		{
			for (uint32_t x = 0; x < size; x++)
			{
				const uint8_t value = (x / 16) % 2 == 0 ? 0 : 255;
				for (uint32_t channel = 0; channel < 3; channel++)
					pixels[(static_cast<size_t>(y) * size + x) * 4 + channel] = value;
			}
		}
		REQUIRE(FileSystem::WriteBytes(project / "Assets" / "Textures" / "Stripes.png", Tests::EncodePNG(size, size, pixels)));
		ThumbnailHarness harness(gpu, project);
		const AssetHandle stripes = harness.Find("Textures/Stripes.png");
		const nlohmann::json created = harness.Run("material.create", { { "path", "Materials/Stripes.stmat" },
			{ "properties", { { "BaseColorMap", UUIDToJson(stripes) }, { "Unlit", true } } } });
		const std::optional<UUID> material = UUIDFromJson(created["asset"]);
		REQUIRE(material);

		harness.RequestUntilIdle({ *material });
		REQUIRE(harness.Thumbnails().GetState(*material) == ThumbnailState::Ready);
		// The texture loaded as its tail; the image waited for the finer levels the view drew it at.
		CHECK(harness.Thumbnails().GetStats().MipWaitFrames > 0);
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(harness.Thumbnails().Request(*material), image));
		// Across the middle of the sphere the stripes show: black and white, not the tail's gray.
		int darkest = 255;
		int brightest = 0;
		for (uint32_t x = image.Width * 3 / 8; x < image.Width * 5 / 8; x++)
		{
			const glm::u8vec4 pixel = Tests::GetPixelRGBA8(image, x, image.Height / 2);
			darkest = std::min(darkest, static_cast<int>(pixel.r));
			brightest = std::max(brightest, static_cast<int>(pixel.r));
		}
		CAPTURE(darkest);
		CAPTURE(brightest);
		CHECK(brightest - darkest > 100);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A skinned model and its skinned mesh show in their bind pose")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path project = Tests::CopyFeatureProject(Tests::CreateTemporaryDirectory("GPUThumbnailSkin") / "Project").parent_path();
		ThumbnailHarness harness(gpu, project);
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		// The feature project's arm: a tube 2 units tall along +Y, skinned to three joints, with clips that bend it.
		const AssetHandle model = harness.Find("Models/Arm.gltf");
		AssetHandle mesh = UUID::Null();
		for (AssetHandle subAsset : assets.GetImportInfo(model).SubAssets)
		{
			if (!mesh.IsValid() && assets.GetAssetType(subAsset) == AssetType::Mesh)
				mesh = subAsset;
		}
		REQUIRE(mesh.IsValid());

		harness.RequestUntilIdle({ model, mesh });
		for (AssetHandle rendered : { model, mesh })
		{
			CAPTURE(rendered.ToString());
			REQUIRE(harness.Thumbnails().GetState(rendered) == ThumbnailState::Ready);
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(harness.Thumbnails().Request(rendered), image));
			// Upright and unbent, as no animator poses it: the tube covers the middle column from a quarter to three quarters
			// of the height, while the sky shows a quarter of the width to its side on the same rows (where a bent arm would
			// reach).
			const auto differs = [](const glm::u8vec4& a, const glm::u8vec4& b)
			{
				int difference = 0;
				for (int channel = 0; channel < 3; channel++)
					difference += std::abs(static_cast<int>(a[channel]) - static_cast<int>(b[channel]));
				return difference > 24;
			};
			for (uint32_t row : { image.Height / 4, image.Height / 2, image.Height * 3 / 4 })
			{
				CAPTURE(row);
				const glm::u8vec4 sky = Tests::GetPixelRGBA8(image, 1, row);
				CHECK(differs(Tests::GetPixelRGBA8(image, image.Width / 2, row), sky));
				CHECK_FALSE(differs(Tests::GetPixelRGBA8(image, image.Width / 4, row), sky));
				CHECK_FALSE(differs(Tests::GetPixelRGBA8(image, image.Width * 3 / 4, row), sky));
			}
		}
		CHECK(harness.Thumbnails().GetStats().Failed == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A saved change of a material renders its thumbnail again; a failed import shows the icon")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path project = Tests::CopyFeatureProject(Tests::CreateTemporaryDirectory("GPUThumbnailChanges") / "Project").parent_path();
		REQUIRE(FileSystem::WriteBytes(project / "Assets" / "Textures" / "Broken.png", std::vector<uint8_t> { 1, 2, 3, 4, 5 }));
		ThumbnailHarness harness(gpu, project);
		const AssetHandle material = harness.Find("Materials/Feature.stmat");
		const AssetHandle broken = harness.Find("Textures/Broken.png");

		harness.RequestUntilIdle({ material, broken });
		CHECK(harness.Thumbnails().GetState(broken) == ThumbnailState::Unavailable);
		CHECK(harness.Thumbnails().Request(broken) == nullptr);
		REQUIRE(harness.Thumbnails().Request(material));
		const glm::u8vec4 before = ReadCenterAndCorner(harness.Thumbnails().Request(material)).first;
		const uint64_t rendered = harness.Thumbnails().GetStats().ScenesRendered;

		// Loads of what a thumbnail shows change nothing; a saved edit makes it again, once it settled.
		harness.Run("material.set", { { "material", UUIDToJson(material) }, { "properties", { { "BaseColor", { 0.0, 1.0, 0.0, 1.0 } } } } });
		for (int frame = 0; frame < 200 && harness.Thumbnails().GetStats().ScenesRendered == rendered; frame++)
		{
			harness.Thumbnails().Request(material);
			harness.Frame();
		}
		harness.RequestUntilIdle({ material });
		CHECK(harness.Thumbnails().GetStats().ScenesRendered == rendered + 1);
		const glm::u8vec4 after = ReadCenterAndCorner(harness.Thumbnails().Request(material)).first;
		CHECK(after != before);
		CHECK(after.g > after.r);
		// Nothing changed since: no further render.
		harness.RequestUntilIdle({ material });
		for (int frame = 0; frame < 60; frame++)
		{
			harness.Thumbnails().Request(material);
			harness.Frame();
		}
		CHECK(harness.Thumbnails().GetStats().ScenesRendered == rendered + 1);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The Asset tab's preview renders at the size it covers, before the thumbnails that wait, and again after a change")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path project = Tests::CopyFeatureProject(Tests::CreateTemporaryDirectory("GPUThumbnailPreview") / "Project").parent_path();
		ThumbnailHarness harness(gpu, project);
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		const AssetHandle material = harness.Find("Materials/Feature.stmat");
		const AssetHandle model = harness.Find("Models/Platform.gltf");
		const AssetHandle prefab = harness.Find("Prefabs/Crate.stprefab");
		const AssetHandle checker = harness.Find("Textures/Checker.png");
		AssetHandle mesh = UUID::Null();
		for (AssetHandle subAsset : assets.GetImportInfo(model).SubAssets)
		{
			if (!mesh.IsValid() && assets.GetAssetType(subAsset) == AssetType::Mesh)
				mesh = subAsset;
		}
		REQUIRE(mesh.IsValid());
		ThumbnailCache& thumbnails = harness.Thumbnails();

		// Asked for with thumbnails that all need rendering, the preview is rendered first; the thumbnail stands in meanwhile.
		auto requestAll = [&](AssetHandle previewed, uint32_t size)
		{
			for (AssetHandle handle : { model, prefab, mesh })
				thumbnails.Request(handle);
			return thumbnails.RequestPreview(previewed, size);
		};
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
		while (thumbnails.GetStats().PreviewsRendered == 0 && std::chrono::steady_clock::now() < deadline)
		{
			requestAll(material, 300);
			harness.Frame();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE(thumbnails.GetStats().PreviewsRendered == 1);
		CHECK(thumbnails.GetStats().ScenesRendered == 0);
		auto settle = [&](AssetHandle previewed, uint32_t size)
		{
			const auto limit = std::chrono::steady_clock::now() + std::chrono::minutes(2);
			do
			{
				requestAll(previewed, size);
				harness.Frame();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			while (thumbnails.IsBusy() && std::chrono::steady_clock::now() < limit);
			REQUIRE_FALSE(thumbnails.IsBusy());
			return requestAll(previewed, size);
		};

		// The preview is drawn at the size asked for (rounded up to the step), the thumbnail stays small.
		nvrhi::ITexture* preview = settle(material, 300);
		REQUIRE(preview);
		CHECK(preview->getDesc().width == 320);
		CHECK(preview->getDesc().height == 320);
		CHECK(thumbnails.Request(material)->getDesc().width == ThumbnailCache::c_Size);
		const auto [center, corner] = ReadCenterAndCorner(preview);
		CHECK(center.a == 255);
		CHECK(center != corner);
		CHECK(thumbnails.GetStats().ScenesRendered == 4); // Thumbnails of the material, the model, the prefab and the mesh
		CHECK(thumbnails.GetStats().PreviewsRendered == 1);

		// A size within the same step keeps it; another one renders it again. Sizes are limited.
		CHECK(settle(material, 310) == preview);
		CHECK(thumbnails.GetStats().PreviewsRendered == 1);
		CHECK(settle(material, 2000)->getDesc().width == ThumbnailCache::c_MaxPreviewSize);
		CHECK(settle(material, 10)->getDesc().width == ThumbnailCache::c_Size);
		CHECK(thumbnails.GetStats().PreviewsRendered == 3);

		// A saved change of the material renders it again once it settled.
		const glm::u8vec4 before = ReadCenterAndCorner(settle(material, 224)).first;
		harness.Run("material.set", { { "material", UUIDToJson(material) }, { "properties", { { "BaseColor", { 0.0, 1.0, 0.0, 1.0 } } } } });
		const uint64_t rendered = thumbnails.GetStats().PreviewsRendered;
		for (int frame = 0; frame < 200 && thumbnails.GetStats().PreviewsRendered == rendered; frame++)
		{
			requestAll(material, 224);
			harness.Frame();
		}
		const glm::u8vec4 after = ReadCenterAndCorner(settle(material, 224)).first;
		CHECK(thumbnails.GetStats().PreviewsRendered == rendered + 1);
		CHECK(after != before);
		CHECK(after.g > after.r);

		// Another asset gets its own; textures show their thumbnail (the Asset tab draws the texture itself).
		nvrhi::ITexture* modelPreview = settle(model, 224);
		REQUIRE(modelPreview);
		CHECK(modelPreview->getDesc().width == 224);
		CHECK(settle(checker, 224) == thumbnails.Request(checker));
		CHECK(thumbnails.GetStats().Failed == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A scene save writes the project's picture for the launcher, which its card shows")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("GPUProjectThumbnail") / "Game";
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		const EditorCommandResult created = commands.Execute(context, "project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" },
			{ "template", "basic3d" } });
		INFO(created.Error);
		REQUIRE(created.Success);
		const auto frameUntil = [&](const std::function<bool()>& condition)
		{
			for (int frame = 0; frame < 2000 && !condition(); frame++)
			{
				REQUIRE(gpu.GetDevice().BeginFrame());
				Renderer::BeginFrame();
				context.Update(Timestep(1.0f / 60.0f));
				gpu.GetDevice().EndFrame();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return condition();
		};
		// The new project's start scene was saved: its picture follows.
		REQUIRE(frameUntil([&]() { return !context.GetProjectThumbnail().IsPending(); }));
		const uint64_t written = context.GetProjectThumbnail().GetWrittenCount();
		CHECK(written >= 1);
		const EditorCommandResult saved = commands.Execute(context, "scene.save", nlohmann::json::object());
		REQUIRE(saved.Success);
		CHECK(context.GetProjectThumbnail().IsPending());
		REQUIRE(frameUntil([&]() { return !context.GetProjectThumbnail().IsPending(); }));
		CHECK(context.GetProjectThumbnail().GetWrittenCount() == written + 1);

		const std::filesystem::path file = ProjectThumbnail::GetFile(directory);
		const std::optional<std::vector<uint8_t>> png = FileSystem::ReadBytes(file);
		REQUIRE(png);
		const std::optional<ReadbackImage> image = ImageReader::DecodeRGBA8(*png);
		REQUIRE(image);
		CHECK(image->Width == ProjectThumbnail::c_Width);
		CHECK(image->Height == ProjectThumbnail::c_Height);
		// The scene from its camera: sky above, ground below, not a black frame.
		const glm::u8vec4 top = Tests::GetPixelRGBA8(*image, image->Width / 2, 4);
		const glm::u8vec4 bottom = Tests::GetPixelRGBA8(*image, image->Width / 2, image->Height - 4);
		CHECK(top != bottom);
		CHECK(static_cast<int>(top.r) + top.g + top.b > 30);

		// The launcher's card reads it back as a texture of its size.
		context.CloseProject();
		WelcomePanel welcome;
		nvrhi::ITexture* picture = nullptr;
		for (int attempt = 0; attempt < 2000 && !picture; attempt++)
		{
			picture = welcome.GetCardImage(directory / "Game.stproj");
			if (!picture)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE(picture);
		CHECK(picture->getDesc().width == ProjectThumbnail::c_Width);
		// A project without a picture has none.
		CHECK(welcome.GetCardImage(directory.parent_path() / "Missing" / "Missing.stproj") == nullptr);
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The project's picture shows the levels its textures stream in, and the game's text as a game window shows it")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const std::filesystem::path project = Tests::CopyFeatureProject(Tests::CreateTemporaryDirectory("GPUProjectPicture") / "Project").parent_path();
		// Black and white stripes 16 texels wide, which the 32x32 mip tail averages to gray (see the thumbnail test above).
		constexpr uint32_t size = 1024;
		std::vector<uint8_t> pixels(size * size * 4, 255);
		for (uint32_t y = 0; y < size; y++)
		{
			for (uint32_t x = 0; x < size; x++)
			{
				const uint8_t value = (x / 16) % 2 == 0 ? 0 : 255;
				for (uint32_t channel = 0; channel < 3; channel++)
					pixels[(static_cast<size_t>(y) * size + x) * 4 + channel] = value;
			}
		}
		REQUIRE(FileSystem::WriteBytes(project / "Assets" / "Textures" / "Stripes.png", Tests::EncodePNG(size, size, pixels)));
		ThumbnailHarness harness(gpu, project);
		const AssetHandle stripes = harness.Find("Textures/Stripes.png");
		const nlohmann::json material = harness.Run("material.create", { { "path", "Materials/Stripes.stmat" },
			{ "properties", { { "BaseColorMap", UUIDToJson(stripes) }, { "Unlit", true } } } })["asset"];

		// A wall of the stripes filling the camera's view and the game's score over it, in red: a HUD sized in pixels.
		harness.Run("scene.new", { { "name", "Picture" }, { "force", true } });
		harness.Run("entity.create", { { "name", "Camera" }, { "components", { { "Transform", { { "Translation", { 0.0, 0.0, 2.0 } } } },
			{ "Camera", { { "Primary", true } } } } } });
		harness.Run("entity.create", { { "name", "Wall" }, { "components", { { "Transform", { { "Scale", { 8.0, 8.0, 1.0 } } } },
			{ "MeshRenderer", { { "Mesh", "Builtin/Quad" }, { "Material", material } } } } } });
		harness.Run("entity.create", { { "name", "Score" }, { "components", { { "Text", { { "Text", "SCORE" }, { "Color", { 1.0, 0.0, 0.0, 1.0 } },
			{ "FontSize", 48.0 }, { "ScreenSpace", true }, { "ScreenAnchor", { 0.5, 0.25 } } } } } } });
		harness.Run("scene.saveAs", { { "path", "Scenes/Picture.stscene" } });
		for (int frame = 0; frame < 3000 && harness.Context.GetProjectThumbnail().IsPending(); frame++)
		{
			harness.Frame();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		REQUIRE_FALSE(harness.Context.GetProjectThumbnail().IsPending());
		const std::optional<std::vector<uint8_t>> png = FileSystem::ReadBytes(ProjectThumbnail::GetFile(project));
		REQUIRE(png);
		const std::optional<ReadbackImage> image = ImageReader::DecodeRGBA8(*png);
		REQUIRE(image);
		REQUIRE(image->Width == ProjectThumbnail::c_Width);
		REQUIRE(image->Height == ProjectThumbnail::c_Height);

		// The stripes, not the tail's gray, across the lower middle of the picture.
		int darkest = 255;
		int brightest = 0;
		for (uint32_t x = image->Width * 3 / 8; x < image->Width * 5 / 8; x++)
		{
			const glm::u8vec4 pixel = Tests::GetPixelRGBA8(*image, x, image->Height * 3 / 4);
			darkest = std::min(darkest, static_cast<int>(pixel.g));
			brightest = std::max(brightest, static_cast<int>(pixel.g));
		}
		CAPTURE(darkest);
		CAPTURE(brightest);
		CHECK(brightest - darkest > 50); // Exposed by the scene's mean: white shows near 90, and the tail as one gray as bright

		// The score's capitals span about 0.7 of their 48-pixel em in a window 810 pixels tall, 4% of the picture's height
		// (11 rows of 270); rendered at the picture's own 270 rows they would cover 12% (34 rows).
		uint32_t firstRow = image->Height;
		uint32_t lastRow = 0;
		for (uint32_t y = 0; y < image->Height; y++)
		{
			for (uint32_t x = 0; x < image->Width; x++)
			{
				const glm::u8vec4 pixel = Tests::GetPixelRGBA8(*image, x, y);
				if (pixel.r > 150 && pixel.g < 90 && pixel.b < 90)
				{
					firstRow = std::min(firstRow, y);
					lastRow = std::max(lastRow, y);
				}
			}
		}
		REQUIRE(firstRow <= lastRow);
		const uint32_t textRows = lastRow - firstRow + 1;
		CAPTURE(textRows);
		CHECK(textRows >= 6);
		CHECK(textRows <= 18);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Closing the project never waits for texture thumbnails whose reads have not started")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// One I/O thread, which the test keeps busy (as long reads of other work would): the thumbnail's read stays queued.
		Tests::ScopedJobSystem jobs(2, 1);
		const std::filesystem::path project = Tests::CopyFeatureProject(Tests::CreateTemporaryDirectory("GPUThumbnailClose") / "Project").parent_path();
		ThumbnailHarness harness(gpu, project);
		const AssetHandle checker = harness.Find("Textures/Checker.png");
		harness.RequestUntilIdle({}); // The index of the cached thumbnails is read (on the I/O thread) before any work starts
		auto started = std::make_shared<std::atomic<bool>>(false);
		auto release = std::make_shared<std::atomic<bool>>(false);
		JobSystem::SubmitIO([started, release]()
		{
			started->store(true);
			// Lets go after a while on its own, so a close that waits for the read ends too (and fails the check below).
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!release->load() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		});
		REQUIRE(Tests::WaitUntil([&]() { return started->load(); }, std::chrono::seconds(10)));
		harness.Thumbnails().Request(checker);
		harness.Frame();
		REQUIRE(harness.Thumbnails().IsBusy());

		const auto start = std::chrono::steady_clock::now();
		harness.Context.CloseProject();
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		release->store(true);
		CHECK_MESSAGE(seconds < 2.0, "Closing the project took ", seconds, " s");
		gpu.GetDevice().WaitForIdle();
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
