#include "Physics/PhysicsScenes.h"

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Physics/PhysicsTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Math/Math.h"
#include "Strata/Physics/PhysicsComponents.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Physics/PhysicsTaskSystem.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneServices.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

namespace Strata::Tests
{

	namespace
	{

		constexpr float c_Pi = 3.14159265f;

		struct BodyMaterial
		{
			float Mass = 1.0f;
			float Friction = 0.5f;
			float Restitution = 0.0f;
		};

		// The Physics Playground's ball prefabs (Ball, BallMetal, BallPlastic, BallWood).
		constexpr std::array<BodyMaterial, 4> c_BallMaterials = { { { 0.5f, 0.6f, 0.6f }, { 3.0f, 0.3f, 0.15f }, { 0.4f, 0.4f, 0.45f }, { 0.6f, 0.6f, 0.3f } } };
		// Its Crate prefab.
		constexpr BodyMaterial c_CrateMaterial = { 20.0f, 0.8f, 0.05f };
		constexpr float c_CrateAngularDamping = 0.5f;

		void ApplyMaterial(Entity entity, const BodyMaterial& material)
		{
			RigidBodyComponent& body = entity.GetComponent<RigidBodyComponent>();
			body.Mass = material.Mass;
			body.Friction = material.Friction;
			body.Restitution = material.Restitution;
		}

		Entity CreateCrate(Scene& scene, const std::string& name, const glm::vec3& position, float halfExtent = 0.5f)
		{
			Entity crate = CreateDynamicBox(scene, name, position, glm::vec3(halfExtent));
			ApplyMaterial(crate, c_CrateMaterial);
			crate.GetComponent<RigidBodyComponent>().AngularDamping = c_CrateAngularDamping;
			return crate;
		}

		Entity CreateCharacter(Scene& scene, const std::string& name, const glm::vec3& position)
		{
			Entity character = scene.CreateEntity(name);
			character.GetTransform().Translation = position;
			CharacterControllerComponent& controller = character.AddComponent<CharacterControllerComponent>();
			controller.Height = 1.8f;
			controller.Radius = 0.3f;
			return character;
		}

		// A row of 1 m static tiles along +X from x = startX, their tops at `top`.
		void CreateTileLane(Scene& scene, float startX, uint32_t tiles, float z, float top, float halfHeight = 0.25f)
		{
			for (uint32_t tile = 0; tile < tiles; tile++)
				CreateStaticBox(scene, "Tile", glm::vec3(startX + static_cast<float>(tile) + 0.5f, top - halfHeight, z), glm::vec3(0.5f, halfHeight, 0.5f));
		}

		float HorizontalDistance(const glm::vec3& a, const glm::vec3& b)
		{
			return glm::length(glm::vec2(a.x - b.x, a.z - b.z));
		}

		void KeepMax(std::map<std::string, double>& probes, const std::string& name, double value)
		{
			auto [it, inserted] = probes.try_emplace(name, value);
			if (!inserted)
				it->second = std::max(it->second, value);
		}

		// The scene's physics system. The settle scenes also run in a helper process without doctest, so nothing here asserts
		// through it: MeasureSettling checks that the system exists before anything is driven.
		PhysicsSystem& PhysicsOf(Scene& scene)
		{
			return *scene.GetSystem<PhysicsSystem>();
		}

		glm::quat RotationOf(const Scene& scene, Entity entity)
		{
			glm::vec3 translation;
			glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
			glm::vec3 scale;
			Math::DecomposeTransform(scene.GetWorldTransform(entity), translation, rotation, scale);
			return rotation;
		}

		// Formats a speed for a probe's name: 0.25, 0.5, 1, 2...
		std::string SpeedName(float speed)
		{
			std::array<char, 16> text {};
			std::snprintf(text.data(), text.size(), "%g", static_cast<double>(speed));
			return text.data();
		}

		// A flat triangle mesh of a 1 m grid, `length` along +X from 0 and 2 m across, its top at y = 0, counter-clockwise from
		// above (its front faces up).
		Ref<const PhysicsMeshData> MakeGridMesh(uint32_t length)
		{
			Ref<PhysicsMeshData> mesh = CreateRef<PhysicsMeshData>();
			constexpr uint32_t c_Rows = 3; // z = -1, 0, 1
			for (uint32_t x = 0; x <= length; x++)
			{
				for (uint32_t row = 0; row < c_Rows; row++)
					mesh->Positions.emplace_back(static_cast<float>(x), 0.0f, static_cast<float>(row) - 1.0f);
			}
			for (uint32_t x = 0; x < length; x++)
			{
				for (uint32_t row = 0; row + 1 < c_Rows; row++)
				{
					const uint32_t a = x * c_Rows + row;     // (x, z)
					const uint32_t b = a + 1;                // (x, z + 1)
					const uint32_t c = a + c_Rows;           // (x + 1, z)
					const uint32_t d = c + 1;                // (x + 1, z + 1)
					// Up-facing: (b - a) x (c - a) = +Z x +X = +Y.
					mesh->Indices.insert(mesh->Indices.end(), { a, b, c, c, b, d });
				}
			}
			return mesh;
		}

	}

	SettleScene CreateBallPit(Scene& scene, uint32_t count)
	{
		CreateGround(scene);
		const float half = count <= 600 ? 4.5f : 6.0f;
		constexpr float c_WallHalfHeight = 2.0f;
		constexpr float c_WallHalfThickness = 0.25f;
		for (int side = 0; side < 4; side++)
		{
			const bool alongX = side < 2;
			const float offset = (side % 2 == 0 ? -1.0f : 1.0f) * (half + c_WallHalfThickness);
			const glm::vec3 position = alongX ? glm::vec3(0.0f, c_WallHalfHeight, offset) : glm::vec3(offset, c_WallHalfHeight, 0.0f);
			const glm::vec3 extents = alongX ? glm::vec3(half + 2.0f * c_WallHalfThickness, c_WallHalfHeight, c_WallHalfThickness)
				: glm::vec3(c_WallHalfThickness, c_WallHalfHeight, half);
			CreateStaticBox(scene, "Wall", position, extents);
		}

		constexpr float c_Spacing = 0.6f;
		constexpr float c_Radius = 0.25f;
		const uint32_t perRow = static_cast<uint32_t>((2.0f * half - c_Spacing) / c_Spacing) + 1;
		const uint32_t perLayer = perRow * perRow;
		for (uint32_t index = 0; index < count; index++)
		{
			const uint32_t layer = index / perLayer;
			const uint32_t cell = index % perLayer;
			// Fixed offsets of up to 4 cm, so that the lattice does not stack ball on ball exactly.
			const float jitterX = static_cast<float>(static_cast<int>((index * 37u) % 9u) - 4) * 0.01f;
			const float jitterZ = static_cast<float>(static_cast<int>((index * 53u) % 9u) - 4) * 0.01f;
			const glm::vec3 position(-half + 0.3f + static_cast<float>(cell % perRow) * c_Spacing + jitterX, 0.3f + static_cast<float>(layer) * c_Spacing,
				-half + 0.3f + static_cast<float>(cell / perRow) * c_Spacing + jitterZ);
			Entity ball = CreateDynamicSphere(scene, "Ball", position, c_Radius);
			ApplyMaterial(ball, c_BallMaterials[index % c_BallMaterials.size()]);
		}

		SettleScene settle;
		settle.AfterStep = [half](Scene& scene, uint32_t, std::map<std::string, double>& probes)
		{
			uint32_t inPit = 0;
			for (const auto [handle, transform, sphere] : scene.GetRegistry().view<TransformComponent, SphereColliderComponent>().each())
			{
				const glm::vec3 position = transform.Translation;
				if (std::abs(position.x) < half && std::abs(position.z) < half && position.y < 4.0f) // Below the walls' top
					inPit++;
			}
			probes["pit.ballsInPit"] = inPit;
		};
		return settle;
	}

	SettleScene CreateTower(Scene& scene, uint32_t height)
	{
		CreateGround(scene);
		Entity top;
		for (uint32_t level = 0; level < height; level++)
			top = CreateCrate(scene, "Crate", glm::vec3(0.0f, 0.5f + static_cast<float>(level), 0.0f));
		const glm::vec3 start(0.0f, 0.5f + static_cast<float>(height - 1), 0.0f);
		SettleScene settle;
		settle.AfterStep = [top, start](Scene& scene, uint32_t, std::map<std::string, double>& probes)
		{
			probes["tower.topDriftMm"] = 1000.0 * HorizontalDistance(GetWorldPosition(scene, top), start);
		};
		return settle;
	}

	SettleScene CreatePyramid(Scene& scene, uint32_t base)
	{
		CreateGround(scene);
		constexpr float c_Pitch = 1.05f;
		Entity top;
		for (uint32_t row = 0; row < base; row++)
		{
			const uint32_t boxes = base - row;
			const float first = -0.5f * c_Pitch * static_cast<float>(boxes - 1);
			for (uint32_t box = 0; box < boxes; box++)
				top = CreateDynamicBox(scene, "Box", glm::vec3(first + c_Pitch * static_cast<float>(box), 0.5f + static_cast<float>(row), 0.0f));
		}
		const glm::vec3 start(0.0f, 0.5f + static_cast<float>(base - 1), 0.0f);
		SettleScene settle;
		settle.AfterStep = [top, start](Scene& scene, uint32_t, std::map<std::string, double>& probes)
		{
			probes["pyramid.topDriftMm"] = 1000.0 * HorizontalDistance(GetWorldPosition(scene, top), start);
		};
		return settle;
	}

	SettleScene CreateDominoRun(Scene& scene, uint32_t count)
	{
		CreateGround(scene);
		constexpr float c_Pitch = 0.55f;
		constexpr float c_Amplitude = 1.5f;
		constexpr float c_Wavelength = 12.0f;
		std::vector<Entity> dominoes;
		// Along a sine: x advances by the pitch's projection, each domino faces along the curve.
		float x = 0.0f;
		for (uint32_t index = 0; index < count; index++)
		{
			const float phase = 2.0f * c_Pi * x / c_Wavelength;
			const float slope = c_Amplitude * 2.0f * c_Pi / c_Wavelength * std::cos(phase);
			const float heading = std::atan(slope);
			Entity domino = scene.CreateEntity("Domino");
			TransformComponent& transform = domino.GetTransform();
			transform.Translation = glm::vec3(x, 0.5f, c_Amplitude * std::sin(phase));
			// Its local +X (its thickness) along the curve.
			transform.Rotation = glm::angleAxis(-heading, glm::vec3(0.0f, 1.0f, 0.0f));
			domino.AddComponent<RigidBodyComponent>();
			ApplyMaterial(domino, { 2.0f, 0.4f, 0.05f });
			domino.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.075f, 0.5f, 0.25f);
			dominoes.push_back(domino);
			x += c_Pitch * std::cos(heading);
		}

		SettleScene settle;
		settle.BeforeStep = [dominoes](Scene& scene, uint32_t step)
		{
			if (step != 0 || dominoes.empty())
				return;
			// The Playground's push: 1.5 N s forward at the first domino's top.
			const Entity first = dominoes.front();
			const glm::quat rotation = RotationOf(scene, first);
			const glm::vec3 top = GetWorldPosition(scene, first) + rotation * glm::vec3(0.0f, 0.45f, 0.0f);
			PhysicsOf(scene).AddImpulseAtPosition(first, rotation * glm::vec3(1.5f, 0.0f, 0.0f), top);
		};
		settle.AfterStep = [dominoes](Scene& scene, uint32_t step, std::map<std::string, double>& probes)
		{
			uint32_t down = 0;
			for (const Entity domino : dominoes)
			{
				if ((RotationOf(scene, domino) * glm::vec3(0.0f, 1.0f, 0.0f)).y < 0.7071f)
					down++;
			}
			probes["dominoes.down"] = down;
			auto [it, inserted] = probes.try_emplace("dominoes.allDownSeconds", -1.0);
			if (down == dominoes.size() && it->second < 0.0)
				it->second = static_cast<double>(step + 1) * scene.GetSettings().FixedTimestep;
		};
		return settle;
	}

	SettleScene CreateSeamProbe(Scene& scene)
	{
		CreateGround(scene);
		constexpr std::array<float, 6> c_Speeds = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
		constexpr std::array<float, 3> c_LedgeSpeeds = { 0.25f, 0.5f, 1.0f };
		constexpr uint32_t c_LaneLength = 40;
		constexpr float c_LaneSpacing = 2.5f;
		constexpr float c_Half = 0.25f; // The boxes' half extent and the balls' radius: both rest with their center at 0.25
		const AssetHandle meshHandle(0x5EA3);

		struct Probe
		{
			Entity Body;
			std::string Name;
			float Speed = 0.0f;
			bool Ball = false;
			bool Ledge = false;
			float RestY = 0.0f;
			float EdgeX = 0.0f;
			// The lowest and highest center along the lane so far (seams), from c_SettledStep on.
			float LowestY = std::numeric_limits<float>::max();
			float HighestY = std::numeric_limits<float>::lowest();
		};
		// The contacts settle first: bodies start exactly at the floor's nominal top, and a triangle mesh holds them up to its
		// skin's few millimeters above it.
		constexpr uint32_t c_SettledStep = 10;
		auto probes = CreateRef<std::vector<Probe>>();
		// The ground lies 1 m below the lanes, so that nothing but the lanes holds the bodies up.
		uint32_t lane = 0;
		const auto makeBody = [&](bool ball, const glm::vec3& position)
		{
			Entity body = ball ? CreateDynamicSphere(scene, "Ball", position, c_Half) : CreateDynamicBox(scene, "Box", position, glm::vec3(c_Half));
			RigidBodyComponent& rigidBody = body.GetComponent<RigidBodyComponent>();
			// Constant speeds: no damping; boxes slide without friction, balls roll on theirs.
			rigidBody.LinearDamping = 0.0f;
			rigidBody.AngularDamping = 0.0f;
			rigidBody.Friction = ball ? 0.6f : 0.0f;
			return body;
		};
		for (const bool mesh : { false, true })
		{
			for (const bool ball : { false, true })
			{
				for (const float speed : c_Speeds)
				{
					const float z = static_cast<float>(lane++) * c_LaneSpacing;
					if (mesh)
					{
						Entity floor = scene.CreateEntity("Mesh Lane");
						floor.GetTransform().Translation = glm::vec3(0.0f, 1.0f, z);
						MeshColliderComponent& collider = floor.AddComponent<MeshColliderComponent>();
						collider.Mesh = meshHandle;
						collider.Convex = false;
					}
					else
					{
						CreateTileLane(scene, 0.0f, c_LaneLength, z, 1.0f);
					}
					const std::string name = std::string("seam.") + (ball ? "ball." : "box.") + (mesh ? "mesh." : "tiles.") + SpeedName(speed);
					probes->push_back(Probe { makeBody(ball, glm::vec3(1.0f, 1.0f + c_Half, z)), name, speed, ball, false, 1.0f + c_Half, 0.0f });
				}
			}
		}
		// Ledges: a lane 6 m long whose top is 1 m above the next tiles' top, the bodies 0.5 m before its edge.
		for (const bool ball : { false, true })
		{
			for (const float speed : c_LedgeSpeeds)
			{
				const float z = static_cast<float>(lane++) * c_LaneSpacing;
				CreateTileLane(scene, 0.0f, 6, z, 2.0f, 0.5f);
				CreateTileLane(scene, 6.0f, 10, z, 1.0f);
				const std::string name = std::string("ledge.") + (ball ? "ball." : "box.") + SpeedName(speed);
				probes->push_back(Probe { makeBody(ball, glm::vec3(5.5f, 2.0f + c_Half, z)), name, speed, ball, true, 2.0f + c_Half, 6.0f });
			}
		}

		SettleScene settle;
		const Ref<const PhysicsMeshData> grid = MakeGridMesh(c_LaneLength);
		settle.MeshProvider = CreateRef<FunctionMeshProvider>([grid, meshHandle](AssetHandle mesh) { return mesh == meshHandle ? grid : nullptr; });
		settle.BeforeStep = [probes](Scene& scene, uint32_t step)
		{
			if (step != 0)
				return;
			PhysicsSystem& physics = PhysicsOf(scene);
			for (const Probe& probe : *probes)
			{
				physics.SetLinearVelocity(probe.Body, glm::vec3(probe.Speed, 0.0f, 0.0f));
				// Rolling without slipping along +X: the contact point at rest.
				if (probe.Ball)
					physics.SetAngularVelocity(probe.Body, glm::vec3(0.0f, 0.0f, -probe.Speed / c_Half));
			}
		};
		settle.AfterStep = [probes](Scene& scene, uint32_t step, std::map<std::string, double>& values)
		{
			PhysicsSystem& physics = PhysicsOf(scene);
			for (Probe& probe : *probes)
			{
				const glm::vec3 position = GetWorldPosition(scene, probe.Body);
				if (probe.Ledge)
				{
					const std::string key = probe.Name + ".travel";
					if (values.find(key) == values.end() && position.y < probe.RestY - 0.05f)
						values[key] = position.x - probe.EdgeX;
					continue;
				}
				KeepMax(values, probe.Name + ".excursionMm", 0.0);
				KeepMax(values, probe.Name + ".upwardSpeed", 0.0);
				// Along the lane, once the contact settled.
				if (step < c_SettledStep || position.x < 0.5f || position.x > static_cast<float>(c_LaneLength) - 0.5f)
					continue;
				probe.LowestY = std::min(probe.LowestY, position.y);
				probe.HighestY = std::max(probe.HighestY, position.y);
				KeepMax(values, probe.Name + ".excursionMm", 1000.0 * (probe.HighestY - probe.LowestY));
				KeepMax(values, probe.Name + ".upwardSpeed", std::max(0.0f, physics.GetLinearVelocity(probe.Body).y));
			}
		};
		return settle;
	}

	SettleScene CreateCratePush(Scene& scene)
	{
		CreateGround(scene);
		// The tiles of both characters' paths, their tops at y = 0.
		for (const float z : { -1.0f, 0.0f, 1.0f, 5.0f, 6.0f, 7.0f })
			CreateTileLane(scene, -2.0f, 14, z, 0.0f);
		const Entity pusher = CreateCharacter(scene, "Pusher", glm::vec3(0.0f, 0.0f, 0.0f));
		// A crate the character's push strength moves (Physics.Character's): 5 kg. Heavier ones (the Third-Person Adventure's 25
		// kg crate) stop the character after a push, and its script pushes them instead.
		Entity crate = CreateDynamicBox(scene, "Crate", glm::vec3(1.5f, 0.5f, 0.0f));
		crate.GetComponent<RigidBodyComponent>().Mass = 5.0f;
		const Entity carrier = CreateCharacter(scene, "Carrier", glm::vec3(0.0f, 0.0f, 6.0f));
		Entity headCrate = CreateCrate(scene, "Head Crate", glm::vec3(0.0f, 2.05f, 6.0f), 0.2f);
		headCrate.GetComponent<RigidBodyComponent>().Mass = 1.0f;

		constexpr uint32_t c_PushSteps = 120;  // 2 s at 60 Hz
		constexpr uint32_t c_CarryStart = 60;  // The head crate lands first
		constexpr uint32_t c_CarrySteps = 60;  // 1 s at 0.5 m/s
		auto offsetAtStart = CreateRef<glm::vec3>(0.0f);
		SettleScene settle;
		settle.BeforeStep = [=](Scene& scene, uint32_t step)
		{
			ICharacterControl& characters = *scene.GetService<ICharacterControl>();
			if (step == 0)
				characters.MoveCharacter(pusher, glm::vec3(2.0f, 0.0f, 0.0f));
			else if (step == c_PushSteps)
				characters.MoveCharacter(pusher, glm::vec3(0.0f));
			if (step == c_CarryStart)
			{
				*offsetAtStart = GetWorldPosition(scene, headCrate) - GetWorldPosition(scene, carrier);
				characters.MoveCharacter(carrier, glm::vec3(0.5f, 0.0f, 0.0f));
			}
			else if (step == c_CarryStart + c_CarrySteps)
			{
				characters.MoveCharacter(carrier, glm::vec3(0.0f));
			}
		};
		settle.AfterStep = [=](Scene& scene, uint32_t step, std::map<std::string, double>& probes)
		{
			if (step + 1 == c_PushSteps)
			{
				probes["crate.distance"] = GetWorldPosition(scene, crate).x - 1.5f;
				probes["pusher.distance"] = GetWorldPosition(scene, pusher).x;
			}
			if (step + 1 == c_CarryStart)
				probes["headCrate.onTopStanding"] = GetWorldPosition(scene, headCrate).y - GetWorldPosition(scene, carrier).y > 1.5f ? 1.0 : 0.0;
			if (step + 1 == c_CarryStart + c_CarrySteps)
			{
				const glm::vec3 offset = GetWorldPosition(scene, headCrate) - GetWorldPosition(scene, carrier);
				probes["headCrate.slip"] = HorizontalDistance(offset, *offsetAtStart);
				probes["headCrate.onTop"] = offset.y > 1.5f ? 1.0 : 0.0;
			}
		};
		return settle;
	}

	SettleScene CreateKinematicPlatform(Scene& scene)
	{
		CreateGround(scene);
		Entity platform = scene.CreateEntity("Platform");
		platform.GetTransform().Translation = glm::vec3(0.0f, 1.0f, 0.0f);
		platform.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		platform.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(2.0f, 0.25f, 2.0f);
		std::vector<Entity> riders;
		for (const glm::vec2 corner : { glm::vec2(-1.0f, -1.0f), glm::vec2(1.0f, -1.0f), glm::vec2(-1.0f, 1.0f), glm::vec2(1.0f, 1.0f) })
			riders.push_back(CreateDynamicBox(scene, "Rider", glm::vec3(corner.x, 1.5f, corner.y), glm::vec3(0.25f)));

		constexpr uint32_t c_MoveStart = 60;
		// The riders slide while the platform's sudden start brings them up to speed (about 0.7 m); what counts is how they
		// creep once they ride along, from a second into the motion for four seconds.
		constexpr uint32_t c_RideStart = c_MoveStart + 60;
		constexpr uint32_t c_RideEnd = c_RideStart + 240;
		auto localAtStart = CreateRef<std::vector<glm::vec3>>();
		const auto toPlatform = [platform](Scene& scene, Entity rider)
		{
			return glm::vec3(glm::inverse(scene.GetWorldTransform(platform)) * glm::vec4(GetWorldPosition(scene, rider), 1.0f));
		};
		SettleScene settle;
		settle.BeforeStep = [=](Scene& scene, uint32_t step)
		{
			if (step == c_RideStart)
			{
				for (const Entity rider : riders)
					localAtStart->push_back(toPlatform(scene, rider));
			}
			if (step < c_MoveStart)
				return;
			// Its target for the end of this step: 2 m/s along +X, turning at 30 degrees/s.
			const float time = static_cast<float>(step + 1 - c_MoveStart) * scene.GetSettings().FixedTimestep;
			Entity moving = platform;
			TransformComponent& transform = moving.GetTransform();
			transform.Translation = glm::vec3(2.0f * time, 1.0f, 0.0f);
			transform.Rotation = glm::angleAxis(glm::radians(30.0f) * time, glm::vec3(0.0f, 1.0f, 0.0f));
			moving.MarkModified<TransformComponent>();
		};
		settle.AfterStep = [=](Scene& scene, uint32_t step, std::map<std::string, double>& probes)
		{
			KeepMax(probes, "platform.slipMm", 0.0);
			if (step >= c_RideEnd || localAtStart->size() != riders.size())
				return;
			for (size_t index = 0; index < riders.size(); index++)
				KeepMax(probes, "platform.slipMm", 1000.0 * HorizontalDistance(toPlatform(scene, riders[index]), (*localAtStart)[index]));
		};
		return settle;
	}

	SettleScene CreateCharacterCrowd(Scene& scene, uint32_t count)
	{
		constexpr uint32_t c_Columns = 40;
		Entity floor = scene.CreateEntity("Floor");
		floor.GetTransform().Translation = glm::vec3(60.0f, -0.5f, 30.0f);
		floor.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(100.0f, 0.5f, 100.0f);
		for (uint32_t step = 0; step < 30; step++)
			CreateStaticBox(scene, "Step " + std::to_string(step), glm::vec3(4.0f * static_cast<float>(step) + 3.0f, 0.15f, 30.0f), glm::vec3(1.0f, 0.15f, 100.0f));
		std::vector<Entity> characters;
		characters.reserve(count);
		for (uint32_t index = 0; index < count; index++)
		{
			Entity character = scene.CreateEntity("Character " + std::to_string(index));
			character.GetTransform().Translation = glm::vec3(1.0f * static_cast<float>(index / c_Columns), 0.0f, 1.5f * static_cast<float>(index % c_Columns));
			character.AddComponent<CharacterControllerComponent>();
			characters.push_back(character);
		}
		SettleScene settle;
		settle.BeforeStep = [characters](Scene& scene, uint32_t step)
		{
			if (step != 0)
				return;
			ICharacterControl& control = *scene.GetService<ICharacterControl>();
			for (const Entity character : characters)
				control.MoveCharacter(character, glm::vec3(2.0f, 0.0f, 0.0f));
		};
		return settle;
	}

	SettleScene CreateFallingBoxes(Scene& scene, uint32_t count, bool trackFloor)
	{
		const Perf::BoxStacks stacks = Perf::CreateBoxStacks(scene, count);
		SettleScene settle;
		if (!trackFloor)
			return settle;
		const UUID floor = stacks.Floor.GetUUID();
		auto floorContacts = CreateRef<uint64_t>(0);
		settle.BeforeStep = [floor, floorContacts](Scene& scene, uint32_t step)
		{
			if (step != 0)
				return;
			ICollisionEventSource& contacts = *scene.GetService<ICollisionEventSource>();
			contacts.AddCollisionListener([floor, floorContacts](const CollisionEvent& event)
			{
				if (event.Type == CollisionEventType::Begin && event.Involves(floor))
					(*floorContacts)++;
			}, CollisionListenerScope::WatchedEntities);
			contacts.WatchCollisions(floor);
		};
		settle.AfterStep = [floorContacts](Scene&, uint32_t, std::map<std::string, double>& probes)
		{
			probes["boxes.floorContactBegins"] = static_cast<double>(*floorContacts);
		};
		return settle;
	}

	std::vector<std::string> GetSettleSceneNames()
	{
		return { "BallPit2k", "BallPit500", "Tower20", "Pyramid15", "Dominoes64", "SeamProbe", "CratePush", "KinematicPlatform", "Characters1000",
			"Boxes10k", "Boxes30kTracked", "Boxes70k" };
	}

	std::optional<SettleScene> CreateSettleScene(Scene& scene, std::string_view name)
	{
		if (name == "BallPit2k")
			return CreateBallPit(scene, 2'000);
		if (name == "BallPit500")
			return CreateBallPit(scene, 500);
		if (name == "Tower20")
			return CreateTower(scene, 20);
		if (name == "Pyramid15")
			return CreatePyramid(scene, 15);
		if (name == "Dominoes64")
			return CreateDominoRun(scene, 64);
		if (name == "SeamProbe")
			return CreateSeamProbe(scene);
		if (name == "CratePush")
			return CreateCratePush(scene);
		if (name == "KinematicPlatform")
			return CreateKinematicPlatform(scene);
		if (name == "Characters1000")
			return CreateCharacterCrowd(scene, 1'000);
		if (name == "Boxes10k")
			return CreateFallingBoxes(scene, 10'000, false);
		if (name == "Boxes30kTracked")
			return CreateFallingBoxes(scene, 30'000, true);
		if (name == "Boxes70k")
			return CreateFallingBoxes(scene, 70'000, false);
		return std::nullopt;
	}

	nlohmann::json SettlingSummary::ToJson() const
	{
		nlohmann::json probes = nlohmann::json::object();
		for (const auto& [name, value] : Probes)
			probes[name] = value;
		return {
			{ "seconds", Seconds },
			{ "steps", Steps },
			{ "movingBodies", MovingBodies },
			{ "secondsTo99Asleep", SecondsTo99Asleep },
			{ "secondsToAllAsleep", SecondsToAllAsleep },
			{ "stepMs", StepMs },
			{ "libraryMs", LibraryMs },
			{ "settleCpuMs", SettleCpuMs },
			{ "busyStrataMs", BusyStrataMs },
			{ "busyLibraryMs", BusyLibraryMs },
			{ "busyCollideMs", BusyCollideMs },
			{ "busySyncMs", BusySyncMs },
			{ "busyWriteBackMs", BusyWriteBackMs },
			{ "busyRecycledShare", BusyRecycledShare },
			{ "meanRecycledShare", MeanRecycledShare },
			{ "maxSpeedAt5s", MaxSpeedAt5s },
			{ "restingJitterMm", RestingJitterMm },
			{ "probes", std::move(probes) } };
	}

	SettlingSummary MeasureSettling(Scene& scene, const SettleScene& settle, float seconds)
	{
		std::optional<ScopedMeshProvider> meshProvider;
		if (settle.MeshProvider)
			meshProvider.emplace(settle.MeshProvider);

		SettlingSummary summary;
		scene.OnRuntimeStart();
		PhysicsSystem* system = scene.GetSystem<PhysicsSystem>();
		if (!system)
		{
			scene.OnRuntimeStop();
			return summary;
		}
		PhysicsSystem& physics = *system;
		physics.SetDetailedStats(true);
		const float timestep = scene.GetSettings().FixedTimestep;
		const uint32_t steps = static_cast<uint32_t>(std::lround(seconds / timestep));
		const uint32_t stepsIn5s = static_cast<uint32_t>(std::lround(5.0f / timestep));
		const uint32_t stepsIn10s = static_cast<uint32_t>(std::lround(10.0f / timestep));
		constexpr uint32_t c_BusyFirst = 5;
		constexpr uint32_t c_BusyEnd = 45;
		std::vector<double> busyStrata;
		std::vector<double> busyLibrary;
		std::vector<double> busyCollide;
		std::vector<double> busySync;
		std::vector<double> busyWriteBack;
		uint64_t busyRecycled = 0;
		uint64_t busyAwake = 0;
		uint64_t recycled = 0;
		uint64_t awake = 0;
		double jitterSum = 0.0;
		uint32_t jitterSteps = 0;
		for (uint32_t step = 0; step < steps; step++)
		{
			if (settle.BeforeStep)
				settle.BeforeStep(scene, step);
			scene.OnUpdateRuntime(timestep);
			if (settle.AfterStep)
				settle.AfterStep(scene, step, summary.Probes);

			const PhysicsStats stats = physics.GetStats();
			const double elapsed = static_cast<double>(step + 1) * timestep;
			summary.StepMs += stats.LastStepTime;
			summary.LibraryMs += stats.LibraryStepTime;
			if (step < stepsIn10s)
				summary.SettleCpuMs += stats.LastStepTime;
			recycled += stats.RecycledContactCount;
			awake += stats.AwakeContactCount;
			if (step >= c_BusyFirst && step < c_BusyEnd)
			{
				busyStrata.push_back(static_cast<double>(stats.LastStepTime) - stats.LibraryStepTime);
				busyLibrary.push_back(stats.LibraryStepTime);
				busyCollide.push_back(stats.LibraryProfile.Collide);
				busySync.push_back(stats.SyncTime);
				busyWriteBack.push_back(stats.WriteBackTime);
				busyRecycled += stats.RecycledContactCount;
				busyAwake += stats.AwakeContactCount;
			}
			if (step + 1 == stepsIn5s)
				summary.MaxSpeedAt5s = stats.MaxBodySpeed;
			if (step >= stepsIn5s && step < stepsIn10s && stats.WrittenBodyCount > 0)
			{
				jitterSum += stats.RestingJitter;
				jitterSteps++;
			}
			const uint32_t moving = stats.MovingBodyCount;
			if (moving > 0)
			{
				if (summary.SecondsTo99Asleep < 0.0 && static_cast<double>(stats.SleepingBodyCount) >= 0.99 * static_cast<double>(moving))
					summary.SecondsTo99Asleep = elapsed;
				if (summary.SecondsToAllAsleep < 0.0 && stats.SleepingBodyCount == moving)
					summary.SecondsToAllAsleep = elapsed;
			}
			summary.Steps++;
		}
		summary.Seconds = static_cast<double>(summary.Steps) * timestep;
		summary.MovingBodies = physics.GetStats().MovingBodyCount;
		summary.MeanRecycledShare = awake > 0 ? static_cast<double>(recycled) / static_cast<double>(awake) : 0.0;
		summary.RestingJitterMm = jitterSteps > 0 ? jitterSum / jitterSteps : 0.0;
		const auto median = [](const std::vector<double>& values) { return values.empty() ? 0.0 : Perf::Summarize(values).MedianMs; };
		summary.BusyStrataMs = median(busyStrata);
		summary.BusyLibraryMs = median(busyLibrary);
		summary.BusyCollideMs = median(busyCollide);
		summary.BusySyncMs = median(busySync);
		summary.BusyWriteBackMs = median(busyWriteBack);
		summary.BusyRecycledShare = busyAwake > 0 ? static_cast<double>(busyRecycled) / static_cast<double>(busyAwake) : 0.0;
		scene.OnRuntimeStop();
		return summary;
	}

	namespace
	{

		bool ParseNumber(std::string_view text, double& outValue)
		{
			if (text.empty())
				return false;
			// Apple Clang 15's libc++ has no std::from_chars for floating point; a stream with the classic locale reads the
			// number independent of the C locale. The whole text must be the number.
			std::istringstream stream { std::string(text) };
			stream.imbue(std::locale::classic());
			double value = 0.0;
			stream >> value;
			if (stream.fail() || stream.peek() != std::istringstream::traits_type::eof() || !std::isfinite(value))
				return false;
			outValue = value;
			return true;
		}

	}

	int RunPhysicsSettleProcess(int argc, char** argv)
	{
		if (argc < 5)
		{
			std::fprintf(stderr, "physics-settle needs <scene> <seconds> <result.json> [options]\n");
			return 1;
		}
		const std::string_view sceneName = argv[2];
		double seconds = 0.0;
		if (!ParseNumber(argv[3], seconds) || seconds <= 0.0 || seconds > 600.0)
		{
			std::fprintf(stderr, "physics-settle: '%s' is not a number of seconds from 0 to 600\n", argv[3]);
			return 1;
		}
		const std::filesystem::path resultPath = FileSystem::FromUTF8(argv[4]);

		PhysicsSettings settings;
		uint32_t workers = 0;
		for (int index = 5; index < argc; index++)
		{
			const std::string_view option = argv[index];
			double value = 0.0;
			if (index + 1 >= argc || !ParseNumber(argv[index + 1], value))
			{
				std::fprintf(stderr, "physics-settle: %s needs a number\n", argv[index]);
				return 1;
			}
			index++;
			if (option == "--workers" && value >= 1.0 && value <= 64.0)
				workers = static_cast<uint32_t>(value);
			else if (option == "--collision-steps" && value >= 1.0 && value <= 64.0)
				settings.CollisionSteps = static_cast<uint32_t>(value);
			else if (option == "--recycle-distance" && value >= 0.0 && value <= 1.0)
				settings.ContactRecycleDistance = static_cast<float>(value);
			else if (option == "--sleep-threshold" && value >= 0.0 && value <= 1.0)
				settings.SleepThreshold = static_cast<float>(value);
			else if (option == "--contact-hertz" && value >= 1.0 && value <= 240.0)
				settings.ContactHertz = static_cast<float>(value);
			else if (option == "--contact-damping" && value >= 0.0 && value <= 100.0)
				settings.ContactDampingRatio = static_cast<float>(value);
			else if (option == "--contact-push-speed" && value >= 0.01 && value <= 100.0)
				settings.ContactPushSpeed = static_cast<float>(value);
			else
			{
				std::fprintf(stderr, "physics-settle: unknown option or value out of range: %s %s\n", argv[index - 1], argv[index]);
				return 1;
			}
		}

		// As applications run: a job system of the machine's workers (box3d's workers follow it, up to the limit asked for).
		JobSystem::Init(JobSystemSpecification());
		PhysicsTaskSystem::SetWorkerLimit(workers);
		const PhysicsSettings previous = PhysicsSystem::GetDefaultSettings();
		PhysicsSystem::SetDefaultSettings(settings);
		int result = 0;
		{
			Scene scene { std::string(sceneName) };
			const std::optional<SettleScene> settle = CreateSettleScene(scene, sceneName);
			if (!settle)
			{
				std::fprintf(stderr, "physics-settle: no settle scene '%.*s'\n", static_cast<int>(sceneName.size()), sceneName.data());
				result = 1;
			}
			else
			{
				const SettlingSummary summary = MeasureSettling(scene, *settle, static_cast<float>(seconds));
				nlohmann::json document = summary.ToJson();
				document["scene"] = std::string(sceneName);
				document["workers"] = PhysicsTaskSystem::GetWorkerCount();
				document["collisionSteps"] = settings.CollisionSteps;
				document["recycleDistance"] = settings.ContactRecycleDistance;
				document["sleepThreshold"] = settings.SleepThreshold;
				document["contactHertz"] = settings.ContactHertz;
				document["contactDamping"] = settings.ContactDampingRatio;
				document["contactPushSpeed"] = settings.ContactPushSpeed;
				std::ofstream file(resultPath, std::ios::binary | std::ios::trunc);
				file << document.dump(1, '\t') << '\n';
				if (!file.good())
				{
					std::fprintf(stderr, "physics-settle: cannot write '%s'\n", argv[4]);
					result = 1;
				}
			}
		}
		PhysicsSystem::SetDefaultSettings(previous);
		PhysicsTaskSystem::SetWorkerLimit(0);
		JobSystem::Shutdown();
		return result;
	}

}
