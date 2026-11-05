#include "Renderer/GIScenes.h"

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/GPUProfiler.h"
#include "Strata/Renderer/GlobalIlluminationLayout.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/Renderer.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <random>
#include <tuple>
#include <utility>

namespace Strata::Tests
{

	namespace
	{

		// One material per color: diffuse, roughness 0.8, metallic 0 (the model's surfaces).
		class YardMaterials
		{
		public:
			explicit YardMaterials(SceneTestAssets& assets)
				: m_Assets(assets)
			{
			}

			AssetHandle Get(const glm::vec3& color)
			{
				const auto key = std::make_tuple(color.r, color.g, color.b);
				auto found = m_Materials.find(key);
				if (found != m_Materials.end())
					return found->second;
				MaterialProperties properties;
				properties.BaseColor = glm::vec4(color, 1.0f);
				properties.Roughness = 0.8f;
				properties.Metallic = 0.0f;
				const AssetHandle handle = m_Assets.AddMaterial(properties);
				m_Materials.emplace(key, handle);
				return handle;
			}

		private:
			SceneTestAssets& m_Assets;
			std::map<std::tuple<float, float, float>, AssetHandle> m_Materials;
		};

		void AddBox(Scene& scene, AssetHandle material, const glm::vec3& minimum, const glm::vec3& maximum)
		{
			Entity box = AddMesh(scene, BuiltinAssets::CubeMesh, material, 0.5f * (minimum + maximum), "Box");
			box.GetComponent<TransformComponent>().Scale = maximum - minimum;
		}

		void AddSphere(Scene& scene, AssetHandle material, const glm::vec3& center, float radius)
		{
			// The built-in sphere's radius is 0.5.
			Entity sphere = AddMesh(scene, BuiltinAssets::SphereMesh, material, center, "Sphere");
			sphere.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f * radius);
		}

		void AddCylinder(Scene& scene, AssetHandle material, const glm::vec3& base, float radius, float height)
		{
			// The built-in cylinder is centered, 1 high, radius 0.5.
			Entity cylinder = AddMesh(scene, BuiltinAssets::CylinderMesh, material, base + glm::vec3(0.0f, 0.5f * height, 0.0f), "Cylinder");
			cylinder.GetComponent<TransformComponent>().Scale = glm::vec3(2.0f * radius, height, 2.0f * radius);
		}

		// An unlit emitter of a colored luminance (nits): the color over its largest component, times that component.
		AssetHandle AddEmitter(SceneTestAssets& assets, const glm::vec3& luminance)
		{
			const float peak = std::max({ luminance.r, luminance.g, luminance.b });
			MaterialProperties properties = UnlitColor({ 0.0f, 0.0f, 0.0f, 1.0f });
			properties.EmissiveColor = peak > 0.0f ? luminance / peak : glm::vec3(0.0f);
			properties.EmissiveIntensity = peak;
			return assets.AddMaterial(properties);
		}

		// 64 x 128 texels of 9 vertical blades (alpha 1 inside, 0 outside), 3 texels wide at the base and tapering to a point at
		// heights between 65% and 100% of the card; white, so the material's base color tints them.
		AssetHandle AddGrassTexture(SceneTestAssets& assets)
		{
			constexpr uint32_t c_Width = 64;
			constexpr uint32_t c_Height = 128;
			constexpr uint32_t c_Blades = 9;
			std::vector<uint8_t> pixels(static_cast<size_t>(c_Width) * c_Height * 4, 0);
			for (uint32_t blade = 0; blade < c_Blades; blade++)
			{
				const float center = (static_cast<float>(blade) + 0.5f) * static_cast<float>(c_Width) / static_cast<float>(c_Blades);
				const float top = static_cast<float>(c_Height) * (0.65f + 0.35f * static_cast<float>((blade * 5) % c_Blades) / static_cast<float>(c_Blades - 1));
				for (uint32_t row = 0; row < c_Height; row++)
				{
					// Rows count from the top of the texture; the card's base is its bottom row.
					const float height = static_cast<float>(c_Height - 1 - row) + 0.5f;
					if (height >= top)
						continue;
					const float halfWidth = 1.5f * (1.0f - height / top);
					for (uint32_t x = 0; x < c_Width; x++)
					{
						if (std::abs(static_cast<float>(x) + 0.5f - center) >= std::max(halfWidth, 0.5f))
							continue;
						uint8_t* texel = pixels.data() + (static_cast<size_t>(row) * c_Width + x) * 4;
						texel[0] = 255;
						texel[1] = 255;
						texel[2] = 255;
						texel[3] = 255;
					}
				}
			}
			return assets.AddTexture(c_Width, c_Height, pixels, TextureFormat::RGBA8SRGB, TextureFilter::Linear, TextureWrap::Clamp);
		}

		// A box of the sun room: its corners and diffuse albedo.
		struct RoomBox
		{
			glm::dvec3 Minimum;
			glm::dvec3 Maximum;
			glm::vec3 Albedo;
			bool Ground = false; // The ground plane (its top face at y = 0), not a cube
		};

		// LookDevScene.json's Room Floor, Ceiling and Walls (centers and sizes there) on its 60 x 60 m ground, with the options'
		// seal, offset and paired room.
		std::vector<RoomBox> GetSunRoomBoxes(const SunRoomOptions& options)
		{
			const glm::vec3 plaster(0.58f);
			const glm::dvec3 offset(options.Offset);
			const auto part = [&](const glm::dvec3& center, const glm::dvec3& size, const glm::vec3& albedo)
			{
				return RoomBox { center + offset - 0.5 * size, center + offset + 0.5 * size, albedo };
			};
			if (options.Hall)
			{
				// A 16 x 2.9 x 16 m hall (inside x -18 to -2, z -15 to 1) with the room's 0.2 m walls, ceiling and window.
				std::vector<RoomBox> hall = {
					RoomBox { { -30.0, -1.0, -30.0 }, { 30.0, 0.0, 30.0 }, glm::vec3(0.35f), true },
					part({ -10.0, 0.05, -7.0 }, { 16.4, 0.1, 16.4 }, options.FloorAlbedo), // Floor
					part({ -10.0, 3.1, -7.0 }, { 16.4, 0.2, 16.4 }, plaster),              // Ceiling
					part({ -18.1, 1.5, -7.0 }, { 0.2, 3.0, 16.4 }, plaster),               // West wall
					part({ -1.9, 1.5, -7.0 }, { 0.2, 3.0, 16.4 }, plaster),                // East wall
					part({ -10.0, 1.5, -15.1 }, { 16.4, 3.0, 0.2 }, plaster),              // North wall
					part({ -14.6, 1.5, 1.1 }, { 7.2, 3.0, 0.2 }, plaster),                 // South wall, left of the window
					part({ -5.4, 1.5, 1.1 }, { 7.2, 3.0, 0.2 }, plaster),                  // Right of it
					part({ -10.0, 0.45, 1.1 }, { 2.0, 0.9, 0.2 }, plaster),                // Below it
					part({ -10.0, 2.6, 1.1 }, { 2.0, 0.8, 0.2 }, plaster)                  // Above it
				};
				if (options.SealWindow)
					hall.push_back(part({ -10.0, 1.55, 1.1 }, { 2.0, 1.3, 0.2 }, plaster));
				return hall;
			}
			std::vector<RoomBox> boxes = {
				RoomBox { { -30.0, -1.0, -30.0 }, { 30.0, 0.0, 30.0 }, glm::vec3(0.35f), true },
				part({ -10.0, 0.05, -2.0 }, { 6.4, 0.1, 6.4 }, options.FloorAlbedo), // Floor
				part({ -10.0, 3.1, -2.0 }, { 6.4, 0.2, 6.4 }, plaster),              // Ceiling
				part({ -13.1, 1.5, -2.0 }, { 0.2, 3.0, 6.4 }, plaster),              // West wall
				part({ -6.9, 1.5, -2.0 }, { 0.2, 3.0, 6.4 }, plaster),               // East wall
				part({ -10.0, 1.5, -5.1 }, { 6.4, 3.0, 0.2 }, plaster),              // North wall
				part({ -12.1, 1.5, 1.1 }, { 2.2, 3.0, 0.2 }, plaster),               // South wall, left of the window
				part({ -7.9, 1.5, 1.1 }, { 2.2, 3.0, 0.2 }, plaster),                // Right of it
				part({ -10.0, 0.45, 1.1 }, { 2.0, 0.9, 0.2 }, plaster),              // Below it
				part({ -10.0, 2.6, 1.1 }, { 2.0, 0.8, 0.2 }, plaster)                // Above it
			};
			if (options.SealWindow)
				boxes.push_back(part({ -10.0, 1.55, 1.1 }, { 2.0, 1.3, 0.2 }, plaster));
			if (options.PairedRoom)
			{
				// Room B: x from -6.8 (A's east wall) to -0.8, sealed.
				boxes.push_back(part({ -3.75, 0.05, -2.0 }, { 6.1, 0.1, 6.4 }, plaster)); // Floor
				boxes.push_back(part({ -3.75, 3.1, -2.0 }, { 6.1, 0.2, 6.4 }, plaster));  // Ceiling
				boxes.push_back(part({ -0.7, 1.5, -2.0 }, { 0.2, 3.0, 6.4 }, plaster));   // East wall
				boxes.push_back(part({ -3.75, 1.5, -5.1 }, { 6.1, 3.0, 0.2 }, plaster));  // North wall
				boxes.push_back(part({ -3.75, 1.5, 1.1 }, { 6.1, 3.0, 0.2 }, plaster));   // South wall
			}
			return boxes;
		}

		// The nearest box a ray enters at a distance above 1e-9, its distance and the normal of the face it enters.
		const RoomBox* TraceRoom(const std::vector<RoomBox>& boxes, const glm::dvec3& origin, const glm::dvec3& direction, double& distance, glm::dvec3& normal)
		{
			const RoomBox* nearest = nullptr;
			distance = std::numeric_limits<double>::max();
			for (const RoomBox& box : boxes)
			{
				double enter = -std::numeric_limits<double>::max();
				double exit = std::numeric_limits<double>::max();
				int enterAxis = -1;
				bool missed = false;
				for (int axis = 0; axis < 3 && !missed; axis++)
				{
					if (std::abs(direction[axis]) < 1e-12)
					{
						missed = origin[axis] < box.Minimum[axis] || origin[axis] > box.Maximum[axis];
						continue;
					}
					double first = (box.Minimum[axis] - origin[axis]) / direction[axis];
					double second = (box.Maximum[axis] - origin[axis]) / direction[axis];
					if (first > second)
						std::swap(first, second);
					if (first > enter)
					{
						enter = first;
						enterAxis = axis;
					}
					exit = std::min(exit, second);
					missed = enter > exit;
				}
				// Rays start outside every box (points on faces are moved off them first): a ray from inside one would leave it.
				if (missed || enterAxis < 0 || enter <= 1e-9 || enter >= distance)
					continue;
				distance = enter;
				normal = glm::dvec3(0.0);
				normal[enterAxis] = direction[enterAxis] > 0.0 ? -1.0 : 1.0;
				nearest = &box;
			}
			return nearest;
		}

		glm::dvec3 SunRadiance(const std::vector<RoomBox>& boxes, const SunRoomOptions& options, const RoomBox& box, const glm::dvec3& point, const glm::dvec3& normal)
		{
			if (!(options.SunIlluminance > 0.0f))
				return glm::dvec3(0.0);
			const glm::dvec3 towardSun = -glm::normalize(glm::dvec3(options.SunDirection));
			const double cosine = glm::dot(normal, towardSun);
			if (cosine <= 0.0)
				return glm::dvec3(0.0);
			double distance = 0.0;
			glm::dvec3 ignored;
			if (TraceRoom(boxes, point + normal * 1e-6, towardSun, distance, ignored))
				return glm::dvec3(0.0);
			return glm::dvec3(box.Albedo) * (static_cast<double>(options.SunIlluminance) * cosine / glm::pi<double>());
		}

		SceneCamera StrafeCamera(const glm::vec3& position, const glm::vec3& target, float verticalFieldOfView, float aspectRatio)
		{
			SceneCamera camera = LookAt(position, target, aspectRatio);
			camera.Projection = Math::PerspectiveReverseZ(glm::radians(verticalFieldOfView), aspectRatio, 0.1f, 1000.0f);
			camera.Far = 1000.0f;
			camera.VerticalFOV = glm::radians(verticalFieldOfView);
			return camera;
		}

	}

	void BuildYard(Scene& scene, SceneTestAssets& assets, const YardOptions& options)
	{
		YardMaterials materials(assets);
		AddNeutralPostProcess(scene);

		// The ground.
		Entity ground = AddMesh(scene, BuiltinAssets::PlaneMesh, materials.Get({ 0.42f, 0.35f, 0.27f }), glm::vec3(0.0f), "Ground");
		ground.GetComponent<TransformComponent>().Scale = glm::vec3(60.0f, 1.0f, 60.0f);

		const glm::vec3 plaster(0.70f, 0.64f, 0.54f);
		const glm::vec3 wood(0.30f, 0.20f, 0.12f);
		const glm::vec3 red(0.62f, 0.09f, 0.07f);
		const glm::vec3 blue(0.18f, 0.32f, 0.50f);
		const glm::vec3 rock(0.33f, 0.30f, 0.27f);
		const float h = 2.8f;
		const float t = 0.2f;
		const AssetHandle wall = materials.Get(plaster);
		// The hut: north and east walls.
		AddBox(scene, wall, { -t, 0.0f, 5.0f }, { 6.0f + t, h, 5.0f + t });
		AddBox(scene, wall, { 6.0f, 0.0f, -t }, { 6.0f + t, h, 5.0f });
		// The south wall with a door (x 2.5-3.5, up to 2.1 m).
		AddBox(scene, wall, { -t, 0.0f, -t }, { 2.5f, h, 0.0f });
		AddBox(scene, wall, { 3.5f, 0.0f, -t }, { 6.0f, h, 0.0f });
		AddBox(scene, wall, { 2.5f, 2.1f, -t }, { 3.5f, h, 0.0f });
		// The west wall with a window (z 1.5-3.0, y 1-2).
		AddBox(scene, wall, { -t, 0.0f, 0.0f }, { 0.0f, h, 1.5f });
		AddBox(scene, wall, { -t, 0.0f, 3.0f }, { 0.0f, h, 5.0f });
		AddBox(scene, wall, { -t, 0.0f, 1.5f }, { 0.0f, 1.0f, 3.0f });
		AddBox(scene, wall, { -t, 2.0f, 1.5f }, { 0.0f, h, 3.0f });
		// The roof with a 0.6 m overhang.
		AddBox(scene, materials.Get({ 0.25f, 0.22f, 0.20f }), { -0.6f, h, -0.6f }, { 6.6f, h + 0.2f, 5.6f });
		// Barrels and crates by the south wall.
		AddCylinder(scene, materials.Get(red), { 1.0f, 0.0f, -0.8f }, 0.35f, 1.0f);
		AddCylinder(scene, materials.Get(red), { 1.8f, 0.0f, -0.9f }, 0.35f, 1.0f);
		AddBox(scene, materials.Get(wood), { 4.1f, 0.0f, -1.3f }, { 5.1f, 1.0f, -0.3f });
		AddBox(scene, materials.Get(wood), { 4.3f, 1.0f, -1.1f }, { 5.0f, 1.7f, -0.4f });
		// The covered car.
		AddBox(scene, materials.Get(blue), { -5.0f, 0.15f, -4.0f }, { -1.0f, 1.2f, -2.2f });
		AddBox(scene, materials.Get(blue), { -4.2f, 1.2f, -3.9f }, { -1.8f, 1.7f, -2.3f });
		// Boulders.
		AddSphere(scene, materials.Get(rock), { 9.0f, 0.6f, -2.0f }, 2.2f);
		AddSphere(scene, materials.Get(rock), { 11.0f, 0.4f, 3.0f }, 3.0f);
		AddSphere(scene, materials.Get(rock), { -8.0f, 0.2f, 2.0f }, 1.6f);
		// The fence: thin posts every 0.5 m and two rails.
		if (options.Fence)
		{
			for (int index = 0; index < 34; index++)
			{
				const float x = -8.0f + static_cast<float>(index) * 0.5f;
				AddBox(scene, materials.Get(wood), { x, 0.0f, -7.0f }, { x + 0.06f, 1.2f, -6.94f });
			}
			AddBox(scene, materials.Get(wood), { -8.0f, 1.0f, -7.0f }, { 9.0f, 1.06f, -6.96f });
			AddBox(scene, materials.Get(wood), { -8.0f, 0.5f, -7.0f }, { 9.0f, 0.56f, -6.96f });
		}
		// The lantern on a post beside the door, and one inside.
		AddBox(scene, materials.Get(wood), { 3.8f, 0.0f, -1.1f }, { 3.86f, 1.6f, -1.04f });
		AddSphere(scene, AddEmitter(assets, glm::vec3(40.0f, 24.0f, 9.0f) * options.LanternPower), { 3.83f, 1.68f, -1.07f }, 0.09f);
		AddSphere(scene, AddEmitter(assets, glm::vec3(30.0f, 18.0f, 7.0f) * options.LanternPower), { 5.3f, 0.5f, 4.3f }, 0.12f);

		// Grass: cards 0.30 m wide and 0.25-0.60 m tall standing on the ground, drawn in the model's order (x, z, height, color),
		// then a random turn about the vertical.
		if (options.GrassCards > 0)
		{
			const AssetHandle texture = AddGrassTexture(assets);
			std::mt19937 random(42);
			std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
			for (uint32_t index = 0; index < options.GrassCards; index++)
			{
				const float x = -6.0f + 12.0f * uniform(random);
				const float z = -9.0f + 8.0f * uniform(random);
				const float height = 0.25f + 0.35f * uniform(random);
				const glm::vec3 color = glm::vec3(0.45f, 0.40f, 0.22f) * (0.7f + 0.5f * uniform(random));
				const float yaw = glm::two_pi<float>() * uniform(random);
				MaterialProperties properties;
				properties.BaseColor = glm::vec4(color, 1.0f);
				properties.Roughness = 0.8f;
				properties.AlphaMode = MaterialAlphaMode::Mask;
				properties.AlphaCutoff = 0.5f;
				properties.DoubleSided = true;
				properties.BaseColorMap = texture;
				Entity card = AddMesh(scene, BuiltinAssets::QuadMesh, assets.AddMaterial(properties), { x, 0.5f * height, z }, "Grass");
				TransformComponent& transform = card.GetComponent<TransformComponent>();
				transform.Rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
				transform.Scale = glm::vec3(0.30f, height, 1.0f);
			}
		}

		// The sun lies toward (-1, 0.75, 0.28): its light travels the other way.
		Entity sun = AddSun(scene, glm::normalize(glm::vec3(1.0f, -0.75f, -0.28f)), 16.0f);
		sun.GetComponent<DirectionalLightComponent>().Color = glm::vec3(1.0f, 0.93f, 0.82f);
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.Source = SkyLightSource::Procedural;
		sky.ZenithColor = glm::vec3(0.30f, 0.50f, 1.0f);
		sky.HorizonColor = glm::vec3(0.95f, 1.0f, 1.08f);
		sky.GroundColor = glm::vec3(0.20f, 0.18f, 0.15f);
		sky.SunIntensity = 0.0f;
		sky.Intensity = 1.0f;
	}

	void BuildRoom(Scene& scene, SceneTestAssets& assets)
	{
		AddNeutralPostProcess(scene);
		MaterialProperties groundProperties;
		groundProperties.BaseColor = glm::vec4(0.35f, 0.35f, 0.35f, 1.0f);
		groundProperties.Roughness = 0.85f;
		Entity ground = AddMesh(scene, BuiltinAssets::PlaneMesh, assets.AddMaterial(groundProperties), glm::vec3(0.0f), "Ground");
		ground.GetComponent<TransformComponent>().Scale = glm::vec3(60.0f, 1.0f, 60.0f);
		MaterialProperties plasterProperties;
		plasterProperties.BaseColor = glm::vec4(0.58f, 0.58f, 0.58f, 1.0f);
		plasterProperties.Roughness = 0.9f;
		const AssetHandle plaster = assets.AddMaterial(plasterProperties);
		// Center and size of each part, as LookDevScene.json places them.
		const auto addPart = [&](AssetHandle mesh, const glm::vec3& center, const glm::vec3& size)
		{
			Entity part = AddMesh(scene, mesh, plaster, center, "Room");
			part.GetComponent<TransformComponent>().Scale = size;
		};
		addPart(BuiltinAssets::CubeMesh, { -10.0f, 0.05f, -2.0f }, { 6.4f, 0.1f, 6.4f });  // Floor
		addPart(BuiltinAssets::CubeMesh, { -10.0f, 3.1f, -2.0f }, { 6.4f, 0.2f, 6.4f });   // Ceiling
		addPart(BuiltinAssets::CubeMesh, { -13.1f, 1.5f, -2.0f }, { 0.2f, 3.0f, 6.4f });   // West wall
		addPart(BuiltinAssets::CubeMesh, { -6.9f, 1.5f, -2.0f }, { 0.2f, 3.0f, 6.4f });    // East wall
		addPart(BuiltinAssets::CubeMesh, { -10.0f, 1.5f, -5.1f }, { 6.4f, 3.0f, 0.2f });   // North wall
		addPart(BuiltinAssets::CubeMesh, { -12.1f, 1.5f, 1.1f }, { 2.2f, 3.0f, 0.2f });    // South wall, left of the window
		addPart(BuiltinAssets::CubeMesh, { -7.9f, 1.5f, 1.1f }, { 2.2f, 3.0f, 0.2f });     // Right of it
		addPart(BuiltinAssets::CubeMesh, { -10.0f, 0.45f, 1.1f }, { 2.0f, 0.9f, 0.2f });   // Below it
		addPart(BuiltinAssets::CubeMesh, { -10.0f, 2.6f, 1.1f }, { 2.0f, 0.8f, 0.2f });    // Above it
		addPart(BuiltinAssets::CubeMesh, { -10.5f, 0.35f, -3.0f }, { 0.5f, 0.5f, 0.5f });  // The box
		addPart(BuiltinAssets::SphereMesh, { -8.5f, 0.4f, -2.5f }, { 0.6f, 0.6f, 0.6f }); // The ball
		Entity lamp = AddPointLight(scene, { -11.5f, 2.2f, -4.0f }, glm::vec3(1.0f), 150.0f, 6.0f);
		lamp.GetComponent<PointLightComponent>().CastShadows = true;
		Entity sun = scene.CreateEntity("Sun");
		sun.GetComponent<TransformComponent>().Rotation = Math::EulerDegreesToQuat(glm::vec3(-40.0f, 30.0f, 0.0f));
		sun.AddComponent<DirectionalLightComponent>().Intensity = 100000.0f;
		SkyLightComponent& sky = scene.CreateEntity("Sky").AddComponent<SkyLightComponent>();
		sky.Source = SkyLightSource::Atmosphere;
		sky.Intensity = 1.0f;
	}

	glm::vec3 SunRoomDirection(float pitch, float yaw)
	{
		return glm::normalize(Math::EulerDegreesToQuat(glm::vec3(pitch, yaw, 0.0f)) * glm::vec3(0.0f, 0.0f, -1.0f));
	}

	void BuildSunRoom(Scene& scene, SceneTestAssets& assets, const SunRoomOptions& options)
	{
		AddNeutralPostProcess(scene);
		std::map<std::tuple<float, float, float>, AssetHandle> materials;
		for (const RoomBox& box : GetSunRoomBoxes(options))
		{
			const auto key = std::make_tuple(box.Albedo.r, box.Albedo.g, box.Albedo.b);
			if (materials.find(key) == materials.end())
			{
				MaterialProperties properties;
				properties.BaseColor = glm::vec4(box.Albedo, 1.0f);
				properties.Roughness = box.Ground ? 0.85f : 0.9f; // LookDevScene.json's Ground and Plaster
				materials.emplace(key, assets.AddMaterial(properties));
			}
			const glm::vec3 center(0.5 * (box.Minimum + box.Maximum));
			if (box.Ground)
			{
				AddMesh(scene, BuiltinAssets::PlaneMesh, materials[key], glm::vec3(0.0f), "Ground").GetComponent<TransformComponent>().Scale =
					glm::vec3(static_cast<float>(box.Maximum.x - box.Minimum.x), 1.0f, static_cast<float>(box.Maximum.z - box.Minimum.z));
				continue;
			}
			AddMesh(scene, BuiltinAssets::CubeMesh, materials[key], center, "Room").GetComponent<TransformComponent>().Scale = glm::vec3(box.Maximum - box.Minimum);
		}
		if (options.SunIlluminance > 0.0f)
			AddSun(scene, options.SunDirection, options.SunIlluminance);
		if (options.AmbientColor != glm::vec3(0.0f))
			scene.CreateEntity("Sky").AddComponent<SkyLightComponent>().AmbientColor = options.AmbientColor;
	}

	glm::vec3 SunRoomReference(const SunRoomOptions& options, const glm::vec3& point, const glm::vec3& normal, uint32_t strata)
	{
		const std::vector<RoomBox> boxes = GetSunRoomBoxes(options);
		const glm::dvec3 n = glm::normalize(glm::dvec3(normal));
		const glm::dvec3 origin = glm::dvec3(point) + n * 1e-6;
		const glm::dvec3 helper = std::abs(n.y) < 0.9 ? glm::dvec3(0.0, 1.0, 0.0) : glm::dvec3(1.0, 0.0, 0.0);
		const glm::dvec3 tangent = glm::normalize(glm::cross(helper, n));
		const glm::dvec3 bitangent = glm::cross(n, tangent);
		glm::dvec3 sum(0.0);
		for (uint32_t i = 0; i < strata; i++)
		{
			for (uint32_t j = 0; j < strata; j++)
			{
				// Cosine-weighted directions (Malley's method over stratified cells): the mean radiance is the irradiance over pi.
				const double u = (static_cast<double>(i) + 0.5) / strata;
				const double v = (static_cast<double>(j) + 0.5) / strata;
				const double radius = std::sqrt(u);
				const double angle = glm::two_pi<double>() * v;
				const glm::dvec3 direction = tangent * (radius * std::cos(angle)) + bitangent * (radius * std::sin(angle)) + n * std::sqrt(1.0 - u);
				double distance = 0.0;
				glm::dvec3 hitNormal;
				const RoomBox* hit = TraceRoom(boxes, origin, direction, distance, hitNormal);
				if (hit)
					sum += SunRadiance(boxes, options, *hit, origin + direction * distance, hitNormal);
				else
					sum += glm::dvec3(options.AmbientColor); // The constant sky
			}
		}
		return glm::vec3(sum / static_cast<double>(strata * strata));
	}

	glm::vec3 SunRoomRadiance(const SunRoomOptions& options, const glm::vec3& point, const glm::vec3& normal)
	{
		const std::vector<RoomBox> boxes = GetSunRoomBoxes(options);
		const glm::dvec3 n = glm::normalize(glm::dvec3(normal));
		const glm::dvec3 origin = glm::dvec3(point) + n * 1e-6;
		double distance = 0.0;
		glm::dvec3 hitNormal;
		// The box the point lies on: the one a short ray back into the surface enters.
		const RoomBox* box = TraceRoom(boxes, origin + n * 1e-3, -n, distance, hitNormal);
		return box ? glm::vec3(SunRadiance(boxes, options, *box, origin, n)) : glm::vec3(0.0f);
	}

	SceneCamera YardStrafe(uint32_t frame, float speed, float aspectRatio)
	{
		const glm::vec3 offset(0.02f * speed * static_cast<float>(frame), 0.0f, 0.0f);
		return StrafeCamera(glm::vec3(-3.5f, 1.7f, -9.5f) + offset, glm::vec3(2.5f, 1.0f, -0.5f) + offset, 55.0f, aspectRatio);
	}

	namespace
	{

		bool AxisSeparates(const glm::vec3& axis, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, const glm::vec3& halfSize)
		{
			const float p0 = glm::dot(v0, axis);
			const float p1 = glm::dot(v1, axis);
			const float p2 = glm::dot(v2, axis);
			const float radius = halfSize.x * std::abs(axis.x) + halfSize.y * std::abs(axis.y) + halfSize.z * std::abs(axis.z);
			return std::max({ p0, p1, p2 }) < -radius || std::min({ p0, p1, p2 }) > radius;
		}

	}

	bool TriangleBoxOverlap(const glm::vec3& center, const glm::vec3& halfSize, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
	{
		const glm::vec3 v0 = a - center;
		const glm::vec3 v1 = b - center;
		const glm::vec3 v2 = c - center;
		const glm::vec3 edges[3] = { v1 - v0, v2 - v1, v0 - v2 };
		const glm::vec3 axes[3] = { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f) };
		for (const glm::vec3& axis : axes)
		{
			for (const glm::vec3& edge : edges)
			{
				if (AxisSeparates(glm::cross(axis, edge), v0, v1, v2, halfSize))
					return false;
			}
		}
		for (int axis = 0; axis < 3; axis++)
		{
			if (std::max({ v0[axis], v1[axis], v2[axis] }) < -halfSize[axis] || std::min({ v0[axis], v1[axis], v2[axis] }) > halfSize[axis])
				return false;
		}
		const glm::vec3 normal = glm::cross(edges[0], edges[1]);
		return std::abs(glm::dot(normal, v0)) <= halfSize.x * std::abs(normal.x) + halfSize.y * std::abs(normal.y) + halfSize.z * std::abs(normal.z);
	}

	std::vector<glm::i64vec3> ReferenceVoxels(const Mesh& mesh, const glm::mat4& world, const glm::i64vec3& anchorVoxel, float voxelSize,
		std::vector<glm::i64vec3>* outFacing, std::vector<glm::i64vec3>* outSolid)
	{
		const RenderData::AffineTransform rows = RenderData::AffineTransform::FromMatrix(world);
		const glm::vec3 anchor = glm::vec3(glm::dvec3(anchorVoxel) * static_cast<double>(voxelSize));
		const std::vector<glm::vec3>& positions = mesh.GetPositions();
		const std::vector<uint32_t>& indices = mesh.GetIndices();
		std::vector<glm::i64vec3> voxels;
		for (const Submesh& submesh : mesh.GetSubmeshes())
		{
			const MeshLOD& detail = submesh.LODs.front();
			for (uint32_t index = detail.IndexOffset; index + 2 < detail.IndexOffset + detail.IndexCount; index += 3)
			{
				glm::vec3 corners[3];
				for (uint32_t corner = 0; corner < 3; corner++)
					corners[corner] = GlobalIllumination::GetVoxelCoordinate(rows, positions[submesh.BaseVertex + indices[index + corner]], anchor, voxelSize);
				const glm::vec3 low = glm::min(glm::min(corners[0], corners[1]), corners[2]);
				const glm::vec3 high = glm::max(glm::max(corners[0], corners[1]), corners[2]);
				const glm::ivec3 first = glm::ivec3(glm::ceil(low)) - 1;
				const glm::ivec3 last = glm::ivec3(glm::floor(high));
				// The side the front faces (counter-clockwise; the test meshes are not mirrored).
				const glm::vec3 facing = glm::cross(corners[1] - corners[0], corners[2] - corners[0]);
				for (int z = first.z; z <= last.z; z++)
				{
					for (int y = first.y; y <= last.y; y++)
					{
						for (int x = first.x; x <= last.x; x++)
						{
							const glm::vec3 center = glm::vec3(x, y, z) + 0.5f;
							if (!TriangleBoxOverlap(center, glm::vec3(0.5f), corners[0], corners[1], corners[2]))
								continue;
							voxels.push_back(anchorVoxel + glm::i64vec3(x, y, z));
							// Include/Voxelize.slang's FacesVoxel: not wholly behind the triangle (IsVoxelBehind), and overlapping it with the box
							// inset along the triangle's plane (no edge or corner touch).
							const float reach = glm::dot(facing, center - corners[0]) + 0.5f * (std::abs(facing.x) + std::abs(facing.y) + std::abs(facing.z));
							const float planeInset = 0.5f - RenderData::c_GISolidInsetVoxels;
							const glm::vec3 facingHalfSize(facing.x != 0.0f ? 0.5f : planeInset, facing.y != 0.0f ? 0.5f : planeInset, facing.z != 0.0f ? 0.5f : planeInset);
							if (outFacing && reach > 0.0f && TriangleBoxOverlap(center, facingHalfSize, corners[0], corners[1], corners[2]))
								outFacing->push_back(anchorVoxel + glm::i64vec3(x, y, z));
							// Include/Voxelize.slang's IsVoxelInFront and IsVoxelSolidFor: a triangle not wholly behind the voxel's center
							// plane reaches into the voxel's inset box; one behind it covers part of the voxel's face (inset along it).
							const float front = glm::dot(facing, center - corners[0]) - 0.5f * (std::abs(facing.x) + std::abs(facing.y) + std::abs(facing.z));
							if (outSolid && front < 0.0f)
							{
								const glm::vec3 inset(0.5f - RenderData::c_GISolidInsetVoxels);
								const glm::vec3 halfSize = reach > 0.0f ? inset
									: glm::vec3(facing.x != 0.0f ? 0.5f : inset.x, facing.y != 0.0f ? 0.5f : inset.y, facing.z != 0.0f ? 0.5f : inset.z);
								if (TriangleBoxOverlap(center, halfSize, corners[0], corners[1], corners[2]))
									outSolid->push_back(anchorVoxel + glm::i64vec3(x, y, z));
							}
						}
					}
				}
			}
		}
		const auto less = [](const glm::i64vec3& a, const glm::i64vec3& b) { return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z); };
		std::sort(voxels.begin(), voxels.end(), less);
		voxels.erase(std::unique(voxels.begin(), voxels.end()), voxels.end());
		for (std::vector<glm::i64vec3>* list : { outFacing, outSolid })
		{
			if (!list)
				continue;
			std::sort(list->begin(), list->end(), less);
			list->erase(std::unique(list->begin(), list->end()), list->end());
		}

		return voxels;
	}

	void BuildCornellBox(Scene& scene, SceneTestAssets& assets)
	{
		AddNeutralPostProcess(scene);
		const auto diffuse = [&](const glm::vec3& albedo)
		{
			MaterialProperties properties;
			properties.BaseColor = glm::vec4(albedo, 1.0f);
			properties.Roughness = 0.9f;
			properties.Metallic = 0.0f;
			return assets.AddMaterial(properties);
		};
		const AssetHandle white = diffuse(c_CornellWhite);
		constexpr float c_Half = 2.75f;
		constexpr float c_Height = 5.5f;
		constexpr float c_Wall = 0.2f;
		AddBox(scene, white, { -c_Half - c_Wall, -c_Wall, -c_Half - c_Wall }, { c_Half + c_Wall, 0.0f, c_Half });          // Floor
		AddBox(scene, white, { -c_Half - c_Wall, c_Height, -c_Half - c_Wall }, { c_Half + c_Wall, c_Height + c_Wall, c_Half }); // Ceiling
		AddBox(scene, white, { -c_Half, 0.0f, -c_Half - c_Wall }, { c_Half, c_Height, -c_Half });                          // Back wall
		AddBox(scene, diffuse(c_CornellRed), { -c_Half - c_Wall, 0.0f, -c_Half - c_Wall }, { -c_Half, c_Height, c_Half });  // Left wall
		AddBox(scene, diffuse(c_CornellGreen), { c_Half, 0.0f, -c_Half - c_Wall }, { c_Half + c_Wall, c_Height, c_Half });  // Right wall
		Entity tall = AddMesh(scene, BuiltinAssets::CubeMesh, white, glm::vec3(-1.05f, 1.6f, -1.0f), "Tall Box");
		TransformComponent& tallTransform = tall.GetComponent<TransformComponent>();
		tallTransform.Scale = glm::vec3(1.6f, 3.2f, 1.6f);
		tallTransform.Rotation = glm::angleAxis(glm::radians(18.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		AddSphere(scene, white, glm::vec3(1.15f, 0.9f, -0.3f), 0.9f);
		AddBox(scene, AddEmitter(assets, glm::vec3(c_CornellPanelLuminance)), { -0.65f, c_Height - 0.3f, -0.525f }, { 0.65f, c_Height, 0.525f });
	}

	SceneCamera CornellCamera(float aspectRatio)
	{
		return StrafeCamera(glm::vec3(0.0f, 2.75f, 4.6f), glm::vec3(0.0f, 2.4f, 0.0f), 70.0f, aspectRatio);
	}

	void BuildGlowingBox(Scene& scene, SceneTestAssets& assets, const glm::vec3& minimum, const glm::vec3& maximum, float albedo, float luminance)
	{
		AddNeutralPostProcess(scene);
		MaterialProperties properties;
		properties.BaseColor = glm::vec4(glm::vec3(albedo), 1.0f);
		properties.Roughness = 1.0f;
		properties.Metallic = 0.0f;
		properties.EmissiveColor = glm::vec3(1.0f);
		properties.EmissiveIntensity = luminance;
		const AssetHandle material = assets.AddMaterial(properties);
		constexpr float c_Wall = 0.2f;
		const glm::vec3 low = minimum - c_Wall;
		const glm::vec3 high = maximum + c_Wall;
		AddBox(scene, material, low, { high.x, minimum.y, high.z });                           // Floor
		AddBox(scene, material, { low.x, maximum.y, low.z }, high);                            // Ceiling
		AddBox(scene, material, { low.x, minimum.y, low.z }, { minimum.x, maximum.y, high.z }); // West
		AddBox(scene, material, { maximum.x, minimum.y, low.z }, { high.x, maximum.y, high.z }); // East
		AddBox(scene, material, { minimum.x, minimum.y, low.z }, { maximum.x, maximum.y, minimum.z }); // North
		AddBox(scene, material, { minimum.x, minimum.y, maximum.z }, { maximum.x, maximum.y, high.z }); // South
	}

	SceneCamera RoomStrafe(uint32_t frame, float aspectRatio)
	{
		const glm::vec3 offset(0.01f * static_cast<float>(frame), 0.0f, 0.0f);
		return StrafeCamera(glm::vec3(-9.0f, 1.5f, -4.5f) + offset, glm::vec3(-10.0f, 1.2f, 1.1f) + offset, 70.0f, aspectRatio);
	}

	bool RenderGIFrame(SceneRenderer& renderer, Scene& scene, const SceneCamera& camera, const SceneRenderOptions& options)
	{
		GPUProfiler& profiler = Renderer::GetGPUProfiler();
		profiler.BeginFrame();
		if (!renderer.Render(scene, camera, nullptr, options))
			return false;
		if (!profiler.IsEnabled())
			return true;
		renderer.ResolveGPUStats(true);
		const SceneRendererGPUStats& gpu = renderer.GetGPUStats();
		CHECK(gpu.HasTimes);
		float stageMs = 0.0f;
		for (const GPUStageTime& stage : gpu.StageMs)
		{
			if (stage.Stage == RendererGPUStages::Get().RadianceCascades)
				stageMs += stage.Milliseconds;
		}
		INFO("frame " << renderer.GetRenderCount() << " of its renderer: GPU.RadianceCascades " << stageMs << " ms, the frame " << gpu.BusyMs << " ms");
		CHECK(stageMs <= c_MaxGIFrameMilliseconds);
		return true;
	}

}
