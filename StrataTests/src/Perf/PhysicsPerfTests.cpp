#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Physics/PhysicsScenes.h"
#include "Physics/PhysicsTestUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Physics/PhysicsRuntime.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Physics/PhysicsTaskSystem.h"
#include "Strata/Physics/PhysicsWorld.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneServices.h"
#include "TestHelpers.h"

#include <box3d/box3d.h>
#include <box3d/collision.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Warnings of the physics module and of box3d logged since `logStart` (a run that logs one has a defect).
	size_t CountPhysicsWarnings(uint64_t logStart)
	{
		return CountLogMessages(logStart, "Physics: ") + CountLogMessages(logStart, "box3d: ");
	}

	using Clock = std::chrono::steady_clock;

	constexpr float c_FrameTimestep = 1.0f / 60.0f;
	// ApplicationSpecification::MaxTimestep: the longest timestep an application gives a frame.
	constexpr float c_MaxTimestep = 0.25f;

	double MillisecondsSince(Clock::time_point start)
	{
		return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
	}

	// Plays frames the way an application capped at 60 frames per second runs them, and returns the time they took in
	// milliseconds (their work, as Application::GetLastFrameWorkTime counts it: without the cap's waits, which are not
	// slept here). Each frame advances the scene by the time since the previous one began: at least a 60 Hz frame (the cap
	// waits out the rest), more after a slow frame, at most c_MaxTimestep. So a slow frame makes the next one simulate more,
	// as in a game.
	double PlayFramesAt60Hz(Scene& scene, uint32_t frames)
	{
		double work = 0.0;
		float timestep = c_FrameTimestep;
		for (uint32_t frame = 0; frame < frames; frame++)
		{
			const Clock::time_point start = Clock::now();
			scene.OnUpdateRuntime(timestep);
			const double elapsed = MillisecondsSince(start);
			work += elapsed;
			timestep = std::clamp(static_cast<float>(elapsed / 1000.0), c_FrameTimestep, c_MaxTimestep);
		}
		return work;
	}

	// Whose contacts are tracked while box stacks land (see ICollisionEventSource).
	enum class ContactListening : uint8_t
	{
		Nobody,
		// A listener of watched entities that counts the floor's contacts, the floor watched: what a script with contact
		// callbacks on the floor does. Every bottom box's contact with the floor is tracked, no contact between boxes.
		Floor
	};

	// The medians of a step's parts over the timed frames (DeveloperGuide "Physics instruments"): Strata's (PhysicsStats'
	// parts of SyncTime and ContactTime), box3d's stages (PhysicsLibraryProfile) and what the instruments cost.
	struct StepParts
	{
		std::vector<double> Apply, Sync, Characters, CheckedPairs, Events, WriteBack, Pairs, Instruments;
		std::vector<double> LibraryPairs, Collide, Solve, Sensors, Constraints, Transforms, Refit, Continuous, SleepIslands, SplitIslands;

		void Add(const PhysicsStats& stats)
		{
			Apply.push_back(stats.ApplyTime);
			Sync.push_back(std::max(0.0, static_cast<double>(stats.SyncTime) - stats.ApplyTime - stats.CharacterTime - stats.CheckedPairTime));
			Characters.push_back(stats.CharacterTime);
			CheckedPairs.push_back(stats.CheckedPairTime);
			Events.push_back(stats.EventTime);
			WriteBack.push_back(stats.WriteBackTime);
			Pairs.push_back(std::max(0.0, static_cast<double>(stats.ContactTime) - stats.EventTime));
			Instruments.push_back(stats.InstrumentTime);
			const PhysicsLibraryProfile& profile = stats.LibraryProfile;
			LibraryPairs.push_back(profile.Pairs);
			Collide.push_back(profile.Collide);
			Solve.push_back(profile.Solve);
			Sensors.push_back(profile.Sensors);
			Constraints.push_back(profile.Constraints);
			Transforms.push_back(profile.Transforms);
			Refit.push_back(profile.Refit);
			Continuous.push_back(profile.Continuous);
			SleepIslands.push_back(profile.SleepIslands);
			SplitIslands.push_back(profile.SplitIslands);
		}

		static double Median(const std::vector<double>& values)
		{
			return values.empty() ? 0.0 : Perf::Summarize(values).MedianMs;
		}

		std::string Describe(double strataMedian) const
		{
			const double instruments = Median(Instruments);
			return fmt::format("Strata's parts (medians, ms): apply {:.3f}, sync {:.3f}, characters {:.3f}, checked pairs {:.3f}, events {:.3f}, "
				"write-back {:.3f}, pairs {:.3f}, instruments {:.4f} ({:.2f}% of Strata's step); box3d's stages: pairs {:.3f}, collide {:.3f}, solve {:.3f} "
				"(constraints {:.3f}, transforms {:.3f}, refit {:.3f}, continuous {:.3f}, sleep islands {:.3f}), sensors {:.3f}, split islands {:.3f}",
				Median(Apply), Median(Sync), Median(Characters), Median(CheckedPairs), Median(Events), Median(WriteBack), Median(Pairs), instruments,
				strataMedian > 0.0 ? 100.0 * instruments / strataMedian : 0.0, Median(LibraryPairs), Median(Collide), Median(Solve), Median(Constraints),
				Median(Transforms), Median(Refit), Median(Continuous), Median(SleepIslands), Median(Sensors), Median(SplitIslands));
		}
	};

	// Plays box stacks (Perf::CreateBoxStacks) while the boxes land on each other and measures that busy phase (see
	// MeasureFallingBoxes).
	Perf::MetricValues PlayFallingBoxes(size_t count, const std::string& metric, ContactListening listening)
	{
		Scene scene;
		const Perf::BoxStacks stacks = Perf::CreateBoxStacks(scene, count);
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(physics.GetStats().DynamicBodyCount == count);

		ICollisionEventSource* contacts = scene.GetService<ICollisionEventSource>();
		REQUIRE(contacts != nullptr);
		size_t floorContactBegins = 0;
		CollisionListenerID listener = c_InvalidCollisionListener;
		if (listening == ContactListening::Floor)
		{
			const UUID floor = stacks.Floor.GetUUID();
			listener = contacts->AddCollisionListener([&floorContactBegins, floor](const CollisionEvent& event)
			{
				if (event.Type == CollisionEventType::Begin && event.Involves(floor))
					floorContactBegins++;
			}, CollisionListenerScope::WatchedEntities);
			contacts->WatchCollisions(floor);
		}

		constexpr uint32_t c_Warmup = 5;
		constexpr uint32_t c_Frames = 40;
		std::vector<double> frames;
		std::vector<double> librarySteps;
		std::vector<double> strataSteps;
		StepParts parts;
		for (uint32_t frame = 0; frame < c_Warmup + c_Frames; frame++)
		{
			const Clock::time_point start = Clock::now();
			scene.OnUpdateRuntime(c_FrameTimestep);
			const double elapsed = MillisecondsSince(start);
			if (frame < c_Warmup)
				continue;
			// Read between the timed frames.
			const PhysicsStats stats = physics.GetStats();
			frames.push_back(elapsed);
			librarySteps.push_back(stats.LibraryStepTime);
			strataSteps.push_back(static_cast<double>(stats.LastStepTime) - static_cast<double>(stats.LibraryStepTime));
			parts.Add(stats);
		}
		const Perf::Measurement frame = Perf::Summarize(frames);
		const Perf::Measurement library = Perf::Summarize(librarySteps);
		const Perf::Measurement strata = Perf::Summarize(strataSteps);
		MESSAGE(metric, ": frame median ", frame.MedianMs, " ms (p95 ", frame.P95Ms, " ms); step: library ", library.MedianMs, " ms, Strata ", strata.MedianMs, " ms");
		MESSAGE(metric, ": ", parts.Describe(strata.MedianMs));

		// Still the busy phase: most boxes are awake.
		const PhysicsStats stats = physics.GetStats();
		CHECK(stats.ActiveBodyCount > count / 2);
		CHECK(stats.StepCount == c_Warmup + c_Frames);

		// box3d's counters over three more frames, read only now: they walk box3d's islands, so the timed frames ran without
		// them (PhysicsWorld::SetDetailedStats). The share of the awake contacts box3d recycled, and how the boxes settle.
		physics.SetDetailedStats(true);
		uint64_t recycled = 0;
		uint64_t awakeContacts = 0;
		for (uint32_t extra = 0; extra < 3; extra++)
		{
			scene.OnUpdateRuntime(c_FrameTimestep);
			const PhysicsStats extraStats = physics.GetStats();
			recycled += extraStats.RecycledContactCount;
			awakeContacts += extraStats.AwakeContactCount;
		}
		const PhysicsStats detailed = physics.GetStats();
		physics.SetDetailedStats(false);
		MESSAGE(metric, ": box3d's islands ", detailed.IslandCount, ", contacts ", detailed.ContactCount, " (", detailed.AwakeContactCount, " awake), recycled ",
			awakeContacts > 0 ? 100.0 * static_cast<double>(recycled) / static_cast<double>(awakeContacts) : 0.0, "% of the awake contacts over 3 steps; ",
			detailed.ActiveBodyCount, " bodies awake, ", detailed.SettlingBodyCount, " settling, the fastest at ", detailed.MaxBodySpeed, " m/s; ",
			detailed.LibraryWorkers, " box3d workers");
		CHECK(CountPhysicsWarnings(logStart) == 0);
		if (listening == ContactListening::Floor)
		{
			// Every bottom box landed (on the floor, for good), and only those contacts are tracked.
			const size_t columns = (count + Perf::c_BoxStackHeight - 1) / Perf::c_BoxStackHeight;
			CHECK(stats.ContactPairCount == columns);
			CHECK(floorContactBegins == columns);
			CHECK(contacts->RemoveCollisionListener(listener));
		}
		else
		{
			CHECK(stats.ContactPairCount == 0);
		}
		scene.OnRuntimeStop();
		REQUIRE(library.MedianMs > 0.0);
		Perf::MetricValues values { { metric + ".ActiveFrame", frame.MedianMs }, { metric + ".StrataStepTime", strata.MedianMs },
			{ metric + ".StrataToLibraryStep", strata.MedianMs / library.MedianMs } };
		// box3d's narrow phase at 70,000 boxes: what contact recycling saves.
		if (count == 70'000)
			values[metric + ".CollideMs"] = StepParts::Median(parts.Collide);
		return values;
	}

	// The busy phase of falling box stacks: the median 60 Hz frame (one fixed step each) against the budget of
	// `<metric>.ActiveFrame`, the median of Strata's part of a step (synchronization, write-back, contacts: the step minus
	// the physics library's own) against `<metric>.StrataStepTime`, and that median to the median of the library's part,
	// `<metric>.StrataToLibraryStep` (Strata's part should stay below the library's). Every attempt of the confirmed check
	// plays the stacks afresh.
	void MeasureFallingBoxes(size_t count, const std::string& metric, ContactListening listening = ContactListening::Nobody)
	{
		Perf::ScopedApplicationJobSystem jobs;
		std::vector<std::string> keys { metric + ".ActiveFrame", metric + ".StrataStepTime", metric + ".StrataToLibraryStep" };
		if (count == 70'000)
			keys.push_back(metric + ".CollideMs");
		Perf::CheckBudgetsConfirmed(keys, [&]
		{
			return PlayFallingBoxes(count, metric, listening);
		});
	}


	//////////////////////////////////////////////////////////////////////////
	// box3d alone (the library-only metrics): what the physics library itself takes for the work of a Strata metric, called
	// directly as Physics.Box3D does, so that the Strata metric splits into box3d's share and Strata's.
	//////////////////////////////////////////////////////////////////////////

	// A box3d world as PhysicsWorld makes one: Strata's task system, sleep and continuous collision on, the room split between
	// static and moving bodies, and the given gravity.
	class LibraryWorld
	{
	public:
		LibraryWorld(uint32_t staticRoom, uint32_t movingRoom, uint32_t contactRoom, const b3Vec3& gravity = b3Vec3 { 0.0f, -10.0f, 0.0f })
			: m_Tasks(&m_Counters)
		{
			b3WorldDef definition = b3DefaultWorldDef();
			definition.gravity = gravity;
			definition.enableSleep = true;
			definition.enableContinuous = true;
			definition.capacity.staticBodyCount = static_cast<int>(staticRoom);
			definition.capacity.staticShapeCount = static_cast<int>(staticRoom);
			definition.capacity.dynamicBodyCount = static_cast<int>(movingRoom);
			definition.capacity.dynamicShapeCount = static_cast<int>(movingRoom);
			definition.capacity.contactCount = static_cast<int>(contactRoom);
			m_Tasks.Configure(definition);
			m_World = b3CreateWorld(&definition);
			m_Hull = b3MakeBoxHull(0.5f, 0.5f, 0.5f);
		}

		~LibraryWorld()
		{
			b3DestroyWorld(m_World);
		}

		LibraryWorld(const LibraryWorld&) = delete;
		LibraryWorld& operator=(const LibraryWorld&) = delete;

		b3WorldId Get() const { return m_World; }

		// A 1 m box as InstallBody makes it (the shape shared, contacts and sensor events on, a dynamic body's mass from its
		// shape afterwards); a static body created after the world stepped finds its pairs itself.
		b3BodyId CreateBox(b3BodyType type, const b3Vec3& position)
		{
			b3BodyDef bodyDefinition = b3DefaultBodyDef();
			bodyDefinition.type = type;
			bodyDefinition.position = position;
			const b3BodyId body = b3CreateBody(m_World, &bodyDefinition);
			b3ShapeDef shapeDefinition = b3DefaultShapeDef();
			shapeDefinition.enableSensorEvents = true;
			shapeDefinition.enableContactEvents = true;
			shapeDefinition.invokeContactCreation = m_Stepped || type != b3_staticBody;
			shapeDefinition.updateBodyMass = type != b3_dynamicBody;
			b3CreateHullShape(body, &shapeDefinition, &m_Hull.base);
			if (type == b3_dynamicBody)
				b3Body_ApplyMassFromShapes(body);
			return body;
		}

		void Step()
		{
			b3World_Step(m_World, c_FrameTimestep, 4);
			m_Stepped = true;
		}
	private:
		PhysicsRuntimeReference m_Runtime; // First: box3d's hooks are installed before the world exists
		PhysicsTaskCounters m_Counters;
		PhysicsTaskSystem m_Tasks;
		b3WorldId m_World = b3_nullWorldId;
		b3BoxHull m_Hull = {};
		bool m_Stepped = false;
	};

	// The bodies of Grow65kMaxFrame in a world of box3d's own, reserved for `room` bodies (90% static as the scene's are) and
	// holding 65,536 of them: the median time of creating 65,536 more and stepping once, over 5 fresh worlds (after one to warm
	// up), as the scene's frame does.
	Perf::Measurement MeasureLibraryGrowth(uint32_t room)
	{
		constexpr uint32_t c_Bodies = 65'536;
		const auto createBodies = [](LibraryWorld& world, uint32_t first, uint32_t count)
		{
			for (uint32_t index = first; index < first + count; index++)
			{
				const b3Vec3 position { static_cast<float>(index % 256) * 3.0f, 0.0f, static_cast<float>(index / 256) * 3.0f };
				if (index % 10 == 0)
					world.CreateBox(b3_dynamicBody, b3Vec3 { position.x, 5.0f, position.z });
				else
					world.CreateBox(b3_staticBody, position);
			}
		};
		std::vector<double> samples;
		for (uint32_t round = 0; round < 6; round++)
		{
			const uint32_t movingRoom = room / 10;
			LibraryWorld world(room - movingRoom, movingRoom, 2 * room, b3Vec3 { 0.0f, 0.0f, 0.0f });
			createBodies(world, 0, c_Bodies);
			world.Step();
			const Clock::time_point start = Clock::now();
			createBodies(world, c_Bodies, c_Bodies);
			world.Step();
			const double elapsed = MillisecondsSince(start);
			if (round > 0)
				samples.push_back(elapsed);
		}
		return Perf::Summarize(samples);
	}

}

TEST_SUITE("Perf.Physics")
{
	TEST_CASE("30,000 falling boxes start playing without a frame spiral")
	{
		Perf::ScopedApplicationJobSystem jobs;
		// The first frames of a game capped at 60 frames per second that opens this scene: starting play (the world and its
		// bodies), then 30 frames, with the time dilation games run with (GameRuntimeOptions::TimeDilation). Their total time,
		// and the physics warnings the run logged (a count: physics engines with fixed contact buffers warned about dropped
		// contacts here; box3d grows its own, and any warning is a defect); every attempt opens the scene afresh.
		Perf::CheckBudgetsConfirmed({ "Perf.Physics.Boxes30k.StartAnd30Frames", "Perf.Physics.Boxes30k.DroppedContactWarnings" }, [&]
		{
			Scene scene;
			Perf::CreateBoxStacks(scene, 30'000);
			const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
			constexpr uint32_t c_Frames = 30;
			scene.SetTimeDilation(true);
			const Clock::time_point start = Clock::now();
			scene.OnRuntimeStart();
			const double startup = MillisecondsSince(start);
			const double elapsed = startup + PlayFramesAt60Hz(scene, c_Frames);
			const PhysicsStats stats = GetPhysics(scene).GetStats();
			MESSAGE("Started in ", startup, " ms and played ", c_Frames, " frames in ", elapsed - startup, " ms: ", stats.StepCount, " steps, ",
				stats.DroppedSteps, " dropped");
			CHECK(stats.StepCount >= c_Frames);
			const size_t droppedContacts = CountPhysicsWarnings(logStart);
			scene.OnRuntimeStop();
			return Perf::MetricValues { { "Perf.Physics.Boxes30k.StartAnd30Frames", elapsed },
				{ "Perf.Physics.Boxes30k.DroppedContactWarnings", static_cast<double>(droppedContacts) } };
		});
	}

	TEST_CASE("70,000 boxes all get a body and step within budget")
	{
		{
			Perf::ScopedApplicationJobSystem jobs;
			// The errors of bodies that did not fit (a count).
			Perf::CheckBudgetConfirmed("Perf.Physics.Boxes70k.BodyErrors", [&]
			{
				Scene scene;
				Perf::CreateBoxStacks(scene, 70'000);
				const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
				scene.OnRuntimeStart();
				const PhysicsStats stats = GetPhysics(scene).GetStats();
				const size_t bodyErrors = CountLogMessages(logStart, "cannot create a body");
				CHECK(stats.BodyCount == 70'001);
				CHECK(stats.PendingBodyCount == 0);
				scene.OnRuntimeStop();
				return static_cast<double>(bodyErrors);
			});
		}
		MeasureFallingBoxes(70'000, "Perf.Physics.Boxes70k");
	}

	TEST_CASE("10,000 falling boxes")
	{
		MeasureFallingBoxes(10'000, "Perf.Physics.Boxes10k");
	}

	TEST_CASE("30,000 falling boxes on a floor whose contacts are tracked")
	{
		MeasureFallingBoxes(30'000, "Perf.Physics.Boxes30kTracked", ContactListening::Floor);
	}

	TEST_CASE("A world at the largest capacities a scene may ask for reserves less than 1 GiB")
	{
		// What playing a scene reserves for its world: the private memory (on Windows, the commit charge: also memory not
		// touched yet) the process gained from the start of play to its second step.
		constexpr double c_MB = 1000.0 * 1000.0; // The unit of the perf lab's memory budgets
		Perf::ScopedApplicationJobSystem jobs;
		Scene scene;
		CreateGround(scene);
		CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 5.0f, 0.0f));
		// What the process gained, and the world's estimate of what box3d reserved (PhysicsWorld::GetReservedBytes: up to
		// c_MaxBodiesReservedAtCreation bodies, the rest of the room counts for growth only).
		struct Reserved
		{
			uint64_t Measured = 0;
			uint64_t Estimated = 0;
		};
		const auto measureReserved = [&](const PhysicsCapacitySettings& capacity)
		{
			scene.GetSettings().PhysicsCapacity = capacity;
			const std::optional<ProcessMemoryInfo> before = Platform::GetProcessMemory();
			REQUIRE(before);
			scene.OnRuntimeStart();
			StepScene(scene, 2);
			const std::optional<ProcessMemoryInfo> during = Platform::GetProcessMemory();
			REQUIRE(during);
			CHECK(GetPhysics(scene).GetStats().MaxBodies == capacity.MaxBodies);
			REQUIRE(GetPhysics(scene).GetWorld() != nullptr);
			Reserved reserved;
			reserved.Estimated = GetPhysics(scene).GetWorld()->GetReservedBytes();
			scene.OnRuntimeStop();
			reserved.Measured = during->PrivateBytes > before->PrivateBytes ? during->PrivateBytes - before->PrivateBytes : 0;
			return reserved;
		};

		PhysicsCapacitySettings largest;
		largest.MaxBodies = PhysicsCapacitySettings::c_MaxBodiesLimit;
		largest.MaxContactConstraints = PhysicsCapacitySettings::c_MaxContactConstraintsLimit;
		PhysicsCapacitySettings mostBodies;
		mostBodies.MaxBodies = PhysicsCapacitySettings::c_MaxBodiesLimit;
		// What the largest capacities reserve, and the most bodies with the contacts sized automatically for them: both budgets
		// are below 1 GiB (1073.7 MB), the physics workstream's acceptance limit.
		Perf::CheckBudgetsConfirmed({ "Perf.Physics.LargestCapacities.ReservedMemory", "Perf.Physics.MostBodies.ReservedMemory" }, [&]
		{
			const Reserved largestReserved = measureReserved(largest);
			const Reserved mostBodiesReserved = measureReserved(mostBodies);
			MESSAGE("Reserved for the largest capacities: ", static_cast<double>(largestReserved.Measured) / c_MB, " MB (estimated ",
				static_cast<double>(largestReserved.Estimated) / c_MB, " MB); for the most bodies: ", static_cast<double>(mostBodiesReserved.Measured) / c_MB,
				" MB (estimated ", static_cast<double>(mostBodiesReserved.Estimated) / c_MB, " MB)");
#if defined(ST_PLATFORM_WINDOWS)
			// Private bytes on Windows are the commit charge, also of storage not touched yet (other systems count only the pages
			// touched so far). The estimate is of a world whose room is used (fitted to box3d's count of its bytes): box3d's
			// reservation for an empty room commits at most that.
			CHECK(static_cast<double>(largestReserved.Measured) <= 1.15 * static_cast<double>(largestReserved.Estimated));
			CHECK(static_cast<double>(mostBodiesReserved.Measured) <= 1.15 * static_cast<double>(mostBodiesReserved.Estimated));
#endif
			return Perf::MetricValues { { "Perf.Physics.LargestCapacities.ReservedMemory", static_cast<double>(largestReserved.Measured) / c_MB },
				{ "Perf.Physics.MostBodies.ReservedMemory", static_cast<double>(mostBodiesReserved.Measured) / c_MB } };
		});
	}

	TEST_CASE("1,000 character controllers walking over steps")
	{
		// A crowd: 1,000 controllers in a grid walk along +X over a ground crossed by 0.3 m steps every 4 m (each walks up and
		// down steps, its stairs and floor queries run every step), with nobody listening to contacts. The median scene update
		// of one fixed step (60 Hz) against Perf.Physics.Characters1000Step (the workstream's acceptance limit is 2 ms).
		Perf::ScopedApplicationJobSystem jobs;
		Perf::CheckBudgetConfirmed("Perf.Physics.Characters1000Step", [&]
		{
			constexpr uint32_t c_Characters = 1'000;
			Scene scene;
			const SettleScene crowd = CreateCharacterCrowd(scene, c_Characters);
			const Entity first = scene.FindEntityByName("Character 0");
			REQUIRE(first);
			scene.OnRuntimeStart();
			REQUIRE(scene.GetService<ICharacterControl>() != nullptr);
			crowd.BeforeStep(scene, 0); // Every character walks along +X at 2 m/s

			constexpr uint32_t c_Warmup = 10;
			constexpr uint32_t c_Steps = 60;
			std::vector<double> samples;
			for (uint32_t step = 0; step < c_Warmup + c_Steps; step++)
			{
				const Clock::time_point start = Clock::now();
				scene.OnUpdateRuntime(scene.GetSettings().FixedTimestep);
				const double elapsed = MillisecondsSince(start);
				if (step >= c_Warmup)
					samples.push_back(elapsed);
			}
			const PhysicsStats stats = GetPhysics(scene).GetStats();
			CHECK(stats.CharacterControllerCount == c_Characters);
			// They walked on: past the first step at x = 2..4 for the first rows.
			CHECK(GetWorldPosition(scene, first).x > 1.5f);
			const Perf::Measurement step = Perf::Summarize(samples);
			MESSAGE("1,000 characters: median step ", step.MedianMs, " ms (p95 ", step.P95Ms, " ms); controllers ", stats.CharacterTime, " ms, library ",
				stats.LibraryStepTime, " ms");
			scene.OnRuntimeStop();
			return step.MedianMs;
		});
	}

	TEST_CASE("Creating 65,536 bodies in a world reserved for 65,536")
	{
		// A world reserved for 65,536 bodies (90% static, 10% dynamic ones awake) gets 65,536 more in one update: box3d grows
		// its storage while the bodies are created (PhysicsWorld, "Capacities"). Each fresh world times that one frame (the
		// longest of the growth by nature: the frame that creates the bodies is the one box3d grows in), and the median of 5
		// worlds goes against Perf.Physics.Grow65kMaxFrame (a provisional target of one 60 Hz frame); the slowest world's is
		// reported too.
		Perf::ScopedApplicationJobSystem jobs;
		constexpr uint32_t c_Bodies = 65'536;
		const auto createBodies = [](Scene& scene, uint32_t first, uint32_t count)
		{
			for (uint32_t index = first; index < first + count; index++)
			{
				const glm::vec3 position(static_cast<float>(index % 256) * 3.0f, 0.0f, static_cast<float>(index / 256) * 3.0f);
				if (index % 10 == 0)
					CreateDynamicBox(scene, "Dynamic", position + glm::vec3(0.0f, 5.0f, 0.0f));
				else
					CreateStaticBox(scene, "Static", position, glm::vec3(0.5f));
			}
		};

		Perf::CheckBudgetConfirmed("Perf.Physics.Grow65kMaxFrame", [&]
		{
			constexpr uint32_t c_Warmup = 1;
			constexpr uint32_t c_Samples = 5;
			std::vector<double> samples;
			for (uint32_t round = 0; round < c_Warmup + c_Samples; round++)
			{
				Scene scene("Grow65k");
				scene.GetSettings().Gravity = glm::vec3(0.0f);
				scene.GetSettings().PhysicsCapacity.MaxBodies = c_Bodies;
				createBodies(scene, 0, c_Bodies);
				scene.OnRuntimeStart();
				PhysicsWorld* world = GetPhysics(scene).GetWorld();
				REQUIRE(world != nullptr);
				REQUIRE(world->GetStats().BodyCount == c_Bodies);
				createBodies(scene, c_Bodies, c_Bodies);
				const Clock::time_point start = Clock::now();
				scene.OnUpdateRuntime(scene.GetSettings().FixedTimestep);
				const double frame = MillisecondsSince(start);
				const PhysicsStats stats = world->GetStats();
				CHECK(stats.BodyCount == 2 * c_Bodies);
				CHECK(stats.GrowthCount == 1);
				CHECK(stats.MaxBodies == 2 * c_Bodies);
				if (round >= c_Warmup)
					samples.push_back(frame);
				scene.OnRuntimeStop();
			}
			const Perf::Measurement growth = Perf::Summarize(samples);
			MESSAGE("Creating 65,536 bodies in a world reserved for 65,536: frame median ", growth.MedianMs, " ms, max ", growth.MaxMs, " ms");
			return growth.MedianMs;
		});
	}

	TEST_CASE("Creating 100,000 static bodies in one update")
	{
		// The play start of the scale audit's 1M world built 100,000 static bodies in one frame on the main thread. Here a
		// running scene gets 100,000 static boxes at once (a world's ground, a streamed region) and the next physics call
		// builds their bodies, on the job threads (PhysicsWorld, parallel creation). Only that call is timed; the boxes go
		// again before the next round. The median of 5 rounds.
		Perf::ScopedApplicationJobSystem jobs;
		constexpr uint32_t c_Bodies = 100'000;
		Scene scene("Create100k");
		scene.GetSettings().PhysicsCapacity.MaxBodies = 262'144; // Room for all of them: the build, not a growth, is measured
		CreateGround(scene);
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		scene.OnUpdateRuntime(c_FrameTimestep);

		Perf::CheckBudgetConfirmed("Perf.Physics.Create100kStaticBodies", [&]
		{
			constexpr uint32_t c_Warmup = 1;
			constexpr uint32_t c_Rounds = 5;
			std::vector<double> samples;
			for (uint32_t round = 0; round < c_Warmup + c_Rounds; round++)
			{
				std::vector<Entity> boxes;
				boxes.reserve(c_Bodies);
				for (uint32_t index = 0; index < c_Bodies; index++)
				{
					const glm::vec3 position(static_cast<float>(index % 316) * 2.0f, 0.5f, static_cast<float>(index / 316) * 2.0f);
					boxes.push_back(CreateStaticBox(scene, "Static", position, glm::vec3(0.5f)));
				}

				const Clock::time_point start = Clock::now();
				const bool built = physics.HasBody(boxes.back());
				const double elapsed = MillisecondsSince(start);
				CHECK(built);
				CHECK(physics.GetStats().StaticBodyCount == c_Bodies + 1);
				if (round >= c_Warmup)
					samples.push_back(elapsed);

				scene.DestroyEntities(boxes);
				scene.OnUpdateRuntime(c_FrameTimestep);
				CHECK(physics.GetStats().StaticBodyCount == 1);
			}
			const Perf::Measurement creation = Perf::Summarize(samples);
			MESSAGE("Creating 100,000 static bodies: median ", creation.MedianMs, " ms, max ", creation.MaxMs, " ms");
			return creation.MedianMs;
		});
		scene.OnRuntimeStop();
	}

	TEST_CASE("Creating 10,000 bodies in one frame")
	{
		Perf::ScopedApplicationJobSystem jobs;
		Scene scene;
		Entity floor = scene.CreateEntity("Floor");
		floor.GetTransform().Translation = glm::vec3(75.0f, -0.5f, 75.0f);
		floor.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(100.0f, 0.5f, 100.0f);
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		scene.OnUpdateRuntime(c_FrameTimestep);

		// A spawn of 10,000 boxes (a prefab burst, a streamed cell): the entities exist, the next physics call builds their
		// bodies on the main thread. Only that is timed; the boxes go again before the next round. The median round of each
		// attempt.
		constexpr uint32_t c_Warmup = 2;
		constexpr uint32_t c_Rounds = 8;
		Perf::CheckBudgetConfirmed("Perf.Physics.Create10kBodies", [&]
		{
			std::vector<double> samples;
			for (uint32_t round = 0; round < c_Warmup + c_Rounds; round++)
			{
				std::vector<Entity> boxes;
				boxes.reserve(10'000);
				for (uint32_t index = 0; index < 10'000; index++)
				{
					Entity box = scene.CreateEntity("Spawned");
					box.GetTransform().Translation = glm::vec3(static_cast<float>(index % 100) * 1.5f, 5.0f + static_cast<float>(round) * 2.0f, static_cast<float>(index / 100) * 1.5f);
					box.AddComponent<RigidBodyComponent>();
					box.AddComponent<BoxColliderComponent>();
					boxes.push_back(box);
				}

				const Clock::time_point start = Clock::now();
				const bool built = physics.HasBody(boxes.back());
				const double elapsed = MillisecondsSince(start);
				CHECK(built);
				if (round >= c_Warmup)
					samples.push_back(elapsed);

				scene.DestroyEntities(boxes);
				scene.OnUpdateRuntime(c_FrameTimestep);
				CHECK(physics.GetStats().DynamicBodyCount == 0);
			}
			const Perf::Measurement creation = Perf::Summarize(samples);
			MESSAGE("Creating 10,000 bodies: median ", creation.MedianMs, " ms, max ", creation.MaxMs, " ms");
			return creation.MedianMs;
		});
		scene.OnRuntimeStop();
	}

	TEST_CASE("Reading box3d's counters at 70,000 boxes")
	{
		// What b3World_GetCounters costs: box3d counts its islands and contacts together with its memory, walking every island
		// and shared hull. The world reads them after every step only if that is at most 0.5% of this scene's library step,
		// otherwise only while detailed statistics are on (PhysicsWorld::SetDetailedStats). Measured at the busy phase's
		// start (nearly every box its own island) and once the stacks stand; for information (DeveloperGuide "Physics
		// instruments" states the decision).
		Perf::ScopedApplicationJobSystem jobs;
		Scene scene;
		Perf::CreateBoxStacks(scene, 70'000);
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(physics.GetWorld() != nullptr);
		const auto measureReads = [&]()
		{
			std::vector<double> samples;
			for (uint32_t read = 0; read < 21; read++)
			{
				const Clock::time_point start = Clock::now();
				const PhysicsLibraryCounters counters = physics.GetWorld()->ReadLibraryCounters();
				samples.push_back(MillisecondsSince(start));
				CHECK(counters.Islands > 0);
			}
			return Perf::Summarize(samples).MedianMs;
		};
		scene.OnUpdateRuntime(c_FrameTimestep);
		const PhysicsLibraryCounters busy = physics.GetWorld()->ReadLibraryCounters();
		const double busyRead = measureReads();
		// The library step of the busy phase as Boxes70k measures it (frames 5 to 44), then the stacks standing.
		std::vector<double> librarySteps;
		for (uint32_t frame = 1; frame < 240; frame++)
		{
			scene.OnUpdateRuntime(c_FrameTimestep);
			if (frame >= 5 && frame < 45)
				librarySteps.push_back(physics.GetStats().LibraryStepTime);
		}
		const PhysicsLibraryCounters standing = physics.GetWorld()->ReadLibraryCounters();
		const double standingRead = measureReads();
		const double library = Perf::Summarize(librarySteps).MedianMs;
		MESSAGE("b3World_GetCounters at 70,000 boxes: ", busyRead, " ms with ", busy.Islands, " islands (the busy phase's start), ", standingRead, " ms with ",
			standing.Islands, " islands (", physics.GetStats().ActiveBodyCount, " bodies awake); the busy phase's library step median ", library,
			" ms, so a read is ", library > 0.0 ? 100.0 * std::max(busyRead, standingRead) / library : 0.0, "% of it (the rule: at most 0.5%)");
		scene.OnRuntimeStop();
	}

	TEST_CASE("box3d alone creating the bodies of Create10kBodies")
	{
		// Create10kBodies' spawn without Strata: 10,000 dynamic boxes created in a world that has stepped, the median of 8 rounds
		// (after 2 to warm up), each round's boxes destroyed again before the next. Against Create10kBodies it splits that
		// metric into box3d's share and Strata's (the roadmap's open items).
		Perf::ScopedApplicationJobSystem jobs;
		Perf::CheckBudgetConfirmed("Perf.Physics.Create10kBodies.LibraryOnly", [&]
		{
			LibraryWorld world(1'024, 1'024, 2'048);
			world.CreateBox(b3_staticBody, b3Vec3 { 75.0f, -0.5f, 75.0f });
			world.Step();
			std::vector<double> samples;
			std::vector<b3BodyId> bodies;
			bodies.reserve(10'000);
			for (uint32_t round = 0; round < 10; round++)
			{
				bodies.clear();
				const Clock::time_point start = Clock::now();
				for (uint32_t index = 0; index < 10'000; index++)
				{
					const b3Vec3 position { static_cast<float>(index % 100) * 1.5f, 5.0f + static_cast<float>(round) * 2.0f, static_cast<float>(index / 100) * 1.5f };
					bodies.push_back(world.CreateBox(b3_dynamicBody, position));
				}
				const double elapsed = MillisecondsSince(start);
				if (round >= 2)
					samples.push_back(elapsed);
				for (const b3BodyId body : bodies)
					b3DestroyBody(body);
				world.Step();
			}
			const Perf::Measurement creation = Perf::Summarize(samples);
			MESSAGE("box3d creating 10,000 dynamic boxes: median ", creation.MedianMs, " ms, max ", creation.MaxMs, " ms");
			return creation.MedianMs;
		});
	}

	TEST_CASE("box3d alone creating the bodies of Create100kStaticBodies")
	{
		// Create100kStaticBodies without Strata: 100,000 static boxes created in a world with room for 262,144 bodies that has
		// stepped (so each finds its pairs), the median of 5 rounds after one to warm up.
		Perf::ScopedApplicationJobSystem jobs;
		Perf::CheckBudgetConfirmed("Perf.Physics.Create100kStaticBodies.LibraryOnly", [&]
		{
			LibraryWorld world(262'144 - 1'024, 1'024, 2'048);
			world.CreateBox(b3_staticBody, b3Vec3 { 0.0f, -0.5f, 0.0f });
			world.Step();
			std::vector<double> samples;
			std::vector<b3BodyId> bodies;
			bodies.reserve(100'000);
			for (uint32_t round = 0; round < 6; round++)
			{
				bodies.clear();
				const Clock::time_point start = Clock::now();
				for (uint32_t index = 0; index < 100'000; index++)
					bodies.push_back(world.CreateBox(b3_staticBody, b3Vec3 { static_cast<float>(index % 316) * 2.0f, 0.5f, static_cast<float>(index / 316) * 2.0f }));
				const double elapsed = MillisecondsSince(start);
				if (round >= 1)
					samples.push_back(elapsed);
				for (const b3BodyId body : bodies)
					b3DestroyBody(body);
				world.Step();
			}
			const Perf::Measurement creation = Perf::Summarize(samples);
			MESSAGE("box3d creating 100,000 static boxes: median ", creation.MedianMs, " ms, max ", creation.MaxMs, " ms");
			return creation.MedianMs;
		});
	}

	TEST_CASE("box3d alone creating the bodies of Grow65kMaxFrame, and what its growth costs")
	{
		// The frame of Grow65kMaxFrame without Strata, in a world reserved for 65,536 bodies (box3d grows its storage while the
		// bodies come) and in one reserved for 131,072 (no growth): the first is the library's share of that frame, and the
		// difference of the two medians is what growing costs (GrowthMs). Each median is of 5 fresh worlds; a difference
		// within the spread of the two is noise, which the message says.
		Perf::ScopedApplicationJobSystem jobs;
		Perf::CheckBudgetsConfirmed({ "Perf.Physics.Grow65k.LibraryOnly", "Perf.Physics.Grow65k.GrowthMs" }, [&]
		{
			const Perf::Measurement growing = MeasureLibraryGrowth(65'536);
			const Perf::Measurement reserved = MeasureLibraryGrowth(131'072);
			const double growth = growing.MedianMs - reserved.MedianMs;
			const double spread = std::max(growing.MaxMs - growing.MinMs, reserved.MaxMs - reserved.MinMs);
			MESSAGE("box3d creating 65,536 bodies and stepping: ", growing.MedianMs, " ms (", growing.MinMs, "-", growing.MaxMs, ") in a world reserved for 65,536, ",
				reserved.MedianMs, " ms (", reserved.MinMs, "-", reserved.MaxMs, ") in one reserved for 131,072: growing costs ", growth, " ms",
				std::abs(growth) <= spread ? " (within noise: the spread is " + std::to_string(spread) + " ms)" : std::string());
			return Perf::MetricValues { { "Perf.Physics.Grow65k.LibraryOnly", growing.MedianMs }, { "Perf.Physics.Grow65k.GrowthMs", std::max(growth, 0.0) } };
		});
	}

	TEST_CASE("box3d alone taking a cell's 25,000 static bodies out of the simulation")
	{
		// The request's frame of Perf.World.CellUnloadMaxFrame without Strata: the cell's 25,000 static boxes disabled at once,
		// the median of 5 rounds after one to warm up (the bodies are enabled again, and the world stepped, between rounds).
		Perf::ScopedApplicationJobSystem jobs;
		Perf::CheckBudgetConfirmed("Perf.World.CellUnload.LibraryOnly", [&]
		{
			LibraryWorld world(32'768, 1'024, 2'048);
			world.CreateBox(b3_staticBody, b3Vec3 { 0.0f, 0.0f, 0.0f });
			std::vector<b3BodyId> bodies;
			bodies.reserve(25'000);
			for (uint32_t index = 0; index < 25'000; index++)
				bodies.push_back(world.CreateBox(b3_staticBody, b3Vec3 { static_cast<float>(index % 160) * 2.0f, 1.0f, static_cast<float>(index / 160) * 2.0f }));
			world.Step();
			std::vector<double> samples;
			for (uint32_t round = 0; round < 6; round++)
			{
				const Clock::time_point start = Clock::now();
				for (const b3BodyId body : bodies)
					b3Body_Disable(body);
				const double elapsed = MillisecondsSince(start);
				if (round >= 1)
					samples.push_back(elapsed);
				world.Step();
				for (const b3BodyId body : bodies)
					b3Body_Enable(body);
				world.Step();
			}
			const Perf::Measurement removal = Perf::Summarize(samples);
			MESSAGE("box3d disabling 25,000 static bodies: median ", removal.MedianMs, " ms, max ", removal.MaxMs, " ms");
			return removal.MedianMs;
		});
	}
}
