// Scripts of the Scripting.World tests (StrataTests/src/Scripting/WorldScriptingTests.cpp). WorldAPI runs on "Tester" in
// the persistent scene of a generated 4 x 4 world (World/WorldTestUtils.h): a "Ground" layer of 32 m cells and a "Props"
// layer of 16 m cells, streamed around a primary camera at the origin. WorldWithoutWorld runs in a scene that is no world.

#include "TestScripts.h"

#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

using namespace Strata;
using namespace ScriptTests;

namespace
{

	// The far corner of the world: Ground cell (3, 3) and Props cells (6, 6) to (7, 7), out of the camera's reach.
	const glm::vec3 c_FarPoint(112.0f, 0.0f, 112.0f);
	constexpr float c_FarRadius = 8.0f;

}

// Once the world is loaded, prefetches the far corner, waits for it, then turns the Props layer off and on again and lets
// the request go. Phase counts the steps done (5 when everything ran).
class WorldAPI : public CheckingScript
{
public:
	int32_t Phase = 0;
	bool Done = false;

	void OnCreate() override
	{
		const float nan = std::numeric_limits<float>::quiet_NaN();
		Expect(!World::RequestCellsAround(glm::vec3(nan, 0.0f, 0.0f), 1.0f).IsValid(), "a non-finite position is refused");
		Expect(!World::RequestCellsAround(c_FarPoint, -1.0f).IsValid() && !World::IsLoadedAround(c_FarPoint, nan), "a negative or non-finite radius is refused");
		Expect(!WorldCellRequest().IsValid() && !WorldCellRequest().Release(), "an empty request is invalid and releases nothing");

		// An engine without the world functions: everything fails, nothing changes.
		StrataScriptHostAPI older = *Detail::GetHost();
		older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, WorldRequestCellsAround));
		{
			const ScopedHost scope(&older);
			Expect(!World::RequestCellsAround(c_FarPoint, c_FarRadius).IsValid() && !World::IsLoadedAround(glm::vec3(0.0f), 1.0f)
				&& !World::SetLayerEnabled("Props", false) && !World::IsLayerEnabled("Props").has_value() && !WorldCellRequest(1).Release(),
				"an engine without the world functions");
		}
	}

	void OnUpdate(float) override
	{
		switch (Phase)
		{
			case 0:
				// The World asset loads in the background: its layers are known once it is in.
				if (World::IsLayerEnabled("Ground").has_value())
				{
					Expect(World::IsLayerEnabled("Props") == true && World::IsLayerEnabled("Ground") == true, "layers start enabled (EnabledByDefault)");
					Expect(!World::IsLayerEnabled("Rivers").has_value() && !World::SetLayerEnabled("Rivers", false), "a layer the world does not have");
					m_Request = World::RequestCellsAround(c_FarPoint, c_FarRadius);
					Expect(m_Request.IsValid() && m_Request.GetID() != 0, "RequestCellsAround");
					Expect(!World::IsLoadedAround(c_FarPoint, c_FarRadius), "the far corner is not loaded yet");
					Phase = 1;
				}
				break;
			case 1:
				// The request streams the far corner although no source is near it.
				if (World::IsLoadedAround(c_FarPoint, c_FarRadius))
				{
					Expect(World::SetLayerEnabled("Props", false), "SetLayerEnabled");
					Expect(World::IsLayerEnabled("Props") == false, "the layer is off");
					Phase = 2;
				}
				break;
			case 2:
				// Without Props the far corner is whole with its Ground cell alone.
				Expect(World::IsLoadedAround(c_FarPoint, c_FarRadius), "a disabled layer's cells do not count");
				Expect(World::SetLayerEnabled("Props", true) && World::IsLayerEnabled("Props") == true, "the layer is on again");
				Phase = 3;
				break;
			case 3:
				if (World::IsLoadedAround(c_FarPoint, c_FarRadius))
				{
					Expect(m_Request.Release(), "Release");
					Expect(!m_Request.Release(), "a released request is not held");
					Phase = 4;
				}
				break;
			case 4:
				Phase = 5;
				Done = true;
				break;
			default:
				break;
		}
	}
private:
	WorldCellRequest m_Request;
};

ST_SCRIPT_CLASS(WorldAPI)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Phase);
	ST_SCRIPT_FIELD(Done);
}

// In a scene that streams no world, every call fails (and the engine reports it once per function).
class WorldWithoutWorld : public CheckingScript
{
public:
	bool Done = false;

	void OnCreate() override
	{
		const WorldCellRequest request = World::RequestCellsAround(glm::vec3(0.0f), 10.0f);
		Expect(!request.IsValid() && !World::RequestCellsAround(glm::vec3(0.0f), 10.0f).IsValid(), "RequestCellsAround fails");
		Expect(!WorldCellRequest(1).Release(), "Release fails");
		Expect(!World::IsLoadedAround(glm::vec3(0.0f), 10.0f), "IsLoadedAround fails");
		Expect(!World::SetLayerEnabled("Ground", false), "SetLayerEnabled fails");
		Expect(!World::IsLayerEnabled("Ground").has_value(), "IsLayerEnabled has no answer");
		Done = true;
	}
};

ST_SCRIPT_CLASS(WorldWithoutWorld)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Done);
}
