#include "stpch.h"
#include "Strata/World/WorldRegistration.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Assert.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/SceneSystem.h"
#include "Strata/World/WorldAsset.h"
#include "Strata/World/WorldComponents.h"
#include "Strata/World/WorldStreamingSystem.h"

namespace Strata
{

	namespace
	{

		PropertyOptions Range(float min, float max, float speed, std::string tooltip)
		{
			PropertyOptions options;
			options.Min = min;
			options.Max = max;
			options.Speed = speed;
			options.Tooltip = std::move(tooltip);
			return options;
		}

	}

	void RegisterWorldModule()
	{
		// Worlds are stored as their JSON source and validated fully when they load.
		AssetLoaderRegistry::Register(AssetType::World, [](const AssetMetadata&, AssetLoadData& data, std::string* outError) -> Ref<Asset>
		{
			return WorldAsset::Deserialize(data.GetBytes(), outError);
		});

		ComponentRegistry::Register<WorldComponent>("World")
			.Category("World")
			.Description("Makes the scene a world's persistent scene: its cells stream around the streaming sources while it runs")
			.AssetProperty("World", &WorldComponent::World, AssetType::World, { .Tooltip = "The world whose cells stream (a .stworld asset)" })
			.Property("DistanceScale", &WorldComponent::DistanceScale,
				Range(0.1f, 10.0f, 0.01f, "Multiplies every layer's streaming distance: lower on devices with less memory"));
		ComponentRegistry::Register<StreamingSourceComponent>("StreamingSource")
			.DisplayName("Streaming Source")
			.Category("World")
			.Description("A point the world's cells stream around; without an active one, the primary camera is")
			.Property("Enabled", &StreamingSourceComponent::Enabled, { .Tooltip = "Whether the source streams" })
			.Property("Radius", &StreamingSourceComponent::Radius,
				Range(0.0f, 1'000'000.0f, 1.0f, "Streams the cells within this distance on every layer; 0: each layer's streaming distance"))
			.Property("Layers", &StreamingSourceComponent::Layers, { .Tooltip = "The layers it streams, separated by commas; empty: every enabled layer" })
			.Property("Priority", &StreamingSourceComponent::Priority, Range(-100.0f, 100.0f, 1.0f, "Higher-priority sources' cells load first"))
			.Property("ViewBias", &StreamingSourceComponent::ViewBias,
				Range(0.0f, 1.0f, 0.01f, "Cells ahead of the source (its -Z axis) load as if this share of the radius nearer"));

		// After the scripts, which move sources and request cells; before physics, so that cells it unloads leave the step.
		// It runs in simulate mode too: a simulated world streams around its camera.
		SceneSystemDescriptor descriptor = MakeSceneSystemDescriptor<WorldStreamingSystem>("World", true, [](Scene& scene) { return CreateScope<WorldStreamingSystem>(scene); });
		descriptor.After = { "Scripting" };
		descriptor.Before = { "Physics" };
		descriptor.ProfileStage = "World.Streaming";
		// A loaded world's start cells load while its load prepares it, before physics builds their bodies.
		descriptor.PrepareLoadedScene = &WorldStreamingSystem::PrepareLoadedScene;
		const bool registered = SceneSystemRegistry::Register(std::move(descriptor));
		ST_CORE_VERIFY(registered, "The world module could not register its scene system");
	}

}
