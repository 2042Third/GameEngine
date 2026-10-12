#pragma once

#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneLoader.h"
#include "Strata/Scene/SceneSerializer.h"
#include "TestHelpers.h"

#include <chrono>
#include <string>
#include <vector>

// Scene loads from a fake asset manager (Scene.Loader, Scene.Streaming).

namespace Strata::Tests
{

	// Makes a manager the active one while it lives (SceneLoader loads from the active manager).
	class ScopedActiveManager
	{
	public:
		explicit ScopedActiveManager(const Ref<AssetManagerBase>& manager)
			: m_Previous(AssetManager::GetActive())
		{
			AssetManager::SetActive(manager);
		}

		~ScopedActiveManager()
		{
			AssetManager::SetActive(m_Previous);
		}

		ScopedActiveManager(const ScopedActiveManager&) = delete;
		ScopedActiveManager& operator=(const ScopedActiveManager&) = delete;
	private:
		Ref<AssetManagerBase> m_Previous;
	};

	inline std::vector<uint8_t> SceneToBytes(const Scene& scene)
	{
		const std::string text = JsonUtils::Dump(SceneSerializer::Serialize(scene));
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// A scene of `count` entities (entity i at x = i) named `name`; its document as an asset's stored form.
	inline std::vector<uint8_t> MakeSceneDocument(size_t count, const std::string& name = "Level")
	{
		Scene scene(name);
		for (size_t index = 0; index < count; index++)
			scene.CreateEntity("Entity " + std::to_string(index)).GetComponent<TransformComponent>().Translation.x = static_cast<float>(index);
		return SceneToBytes(scene);
	}

	// A project of one scene asset (0x100) that uses ten fake assets of 100 bytes each (0x200...), recorded as its
	// dependencies the way imports and packs record them.
	struct SceneLoaderFixture
	{
		ScopedJobSystem Jobs { 2, 1 };
		ScopedFakeLoader Loader;
		Ref<FakeAssetManager> Manager = CreateRef<FakeAssetManager>();
		ScopedActiveManager Active { Manager };
		AssetHandle SceneHandle = UUID::Null();
		std::vector<AssetHandle> Dependencies;

		explicit SceneLoaderFixture(std::vector<uint8_t> sceneDocument = MakeSceneDocument(5))
		{
			SceneHandle = Manager->AddStored(0x100, AssetType::Scene, std::move(sceneDocument), "Scenes/Level.stscene");
			for (uint64_t index = 0; index < 10; index++)
				Dependencies.push_back(Manager->Add(0x200 + index, MakeUsage(100), 100));
			Manager->SetDependencies(SceneHandle, Dependencies);
		}

		// Advances the manager and the request one frame at a time until the request is finished (REQUIREs it finishes).
		void Finish(SceneLoadRequest& request)
		{
			REQUIRE(WaitUntil([&]()
			{
				Manager->Update();
				request.Update();
				return request.IsFinished();
			}, std::chrono::milliseconds(10000)));
		}
	};

}
