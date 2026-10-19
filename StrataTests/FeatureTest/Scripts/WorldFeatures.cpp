// The world's streaming (World, WorldCellRequest) on "Streamer", a streaming source of the feature scene, which is the
// persistent scene of FeatureWorld: a 2 x 2 world of 32 m Ground cells and 16 m Props cells from (200, 200). The Streamer
// starts in the middle and streams every cell. The script waits for them, requests the cells around a corner (Props cell
// (0, 0), which holds "Cell Script"), moves the Streamer out of the world so the other cells go, turns Props off (its cells
// go, the requested one too) and on again (the requested one comes back), then lets the request go and moves the
// Streamer back. WorldCellFeatures on "Cell Script" journals its OnCreate and OnDestroy as its cell comes and goes.
//
// Each step waits for what streams (cells load on other threads), so the steps take as many frames as the machine needs;
// Phase and RequestID are fields, so the hot reload keeps them.

#include "FeatureScript.h"

#include <cstdint>
#include <optional>

using namespace Strata;
using namespace FeatureTest;

namespace
{

	const glm::vec3 c_Middle(216.0f, 1.0f, 216.0f);
	const glm::vec3 c_Outside(600.0f, 1.0f, 600.0f);
	// The corner the request keeps: Props cell (0, 0) and Ground cell (0, 0).
	const glm::vec3 c_Corner(205.0f, 0.0f, 205.0f);
	constexpr float c_CornerRadius = 1.0f;

	bool Exists(const char* name)
	{
		return static_cast<bool>(Strata::Scene::FindEntityByName(name));
	}

}

class WorldFeatures : public FeatureScript
{
public:
	int32_t Phase = 0;
	int32_t RequestID = 0;

	void OnCreate() override
	{
		Journal(*this, "WorldFeatures", "OnCreate");
	}

	void OnUpdate(float) override
	{
		switch (Phase)
		{
			case 0:
				// Every cell around the Streamer (its radius of 30 m reaches the whole world) is in.
				if (World::IsLayerEnabled("Ground").has_value() && World::IsLoadedAround(c_Middle, 30.0f) && Exists("Cell Script") && Exists("Prop 1_1"))
				{
					Expect(World::IsLayerEnabled("Ground") == true && World::IsLayerEnabled("Props") == true, "the world's layers start enabled");
					Expect(!World::IsLayerEnabled("Rivers").has_value(), "a layer the world does not have");
					const WorldCellRequest request = World::RequestCellsAround(c_Corner, c_CornerRadius);
					Expect(request.IsValid(), "RequestCellsAround");
					RequestID = static_cast<int32_t>(request.GetID());
					GetEntity().GetTransform().SetWorldPosition(c_Outside);
					Phase = 1;
				}
				break;
			case 1:
				// The Streamer left: its cells go, but the request keeps the corner.
				if (!Exists("Prop 1_1"))
				{
					Expect(World::IsLoadedAround(c_Corner, c_CornerRadius) && Exists("Cell Script"), "the requested corner stays");
					Expect(World::SetLayerEnabled("Props", false) && World::IsLayerEnabled("Props") == false, "SetLayerEnabled turns Props off");
					Phase = 2;
				}
				break;
			case 2:
				// A disabled layer's cells go, the requested one too.
				if (!Exists("Cell Script"))
				{
					Expect(World::IsLoadedAround(c_Corner, c_CornerRadius), "without Props the corner is its Ground cell");
					Expect(World::SetLayerEnabled("Props", true) && World::IsLayerEnabled("Props") == true, "SetLayerEnabled turns Props on");
					Phase = 3;
				}
				break;
			case 3:
				// Enabled again, the requested Props cell comes back.
				if (Exists("Cell Script") && World::IsLoadedAround(c_Corner, c_CornerRadius))
				{
					const WorldCellRequest request(static_cast<uint64_t>(RequestID));
					Expect(request.Release(), "WorldCellRequest::Release");
					GetEntity().GetTransform().SetWorldPosition(c_Middle);
					Phase = 4;
				}
				break;
			case 4:
				// Back in the middle, the Streamer streams the world again.
				if (Exists("Prop 1_1"))
				{
					Phase = 5;
					Completed = true;
				}
				break;
			default:
				break;
		}
	}
};

ST_SCRIPT_CLASS(WorldFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(Phase);
	ST_SCRIPT_FIELD(RequestID);
}

// In Props cell (0, 0) of FeatureWorld: journals when its cell loads and unloads.
class WorldCellFeatures : public Strata::Script
{
public:
	void OnCreate() override
	{
		Journal(*this, "WorldCellFeatures", "OnCreate");
	}

	void OnDestroy() override
	{
		Journal(*this, "WorldCellFeatures", "OnDestroy");
	}
};

ST_SCRIPT_CLASS(WorldCellFeatures)
{
}
