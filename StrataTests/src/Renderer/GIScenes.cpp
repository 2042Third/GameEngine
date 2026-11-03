#include "Renderer/GIScenes.h"

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/GlobalIlluminationLayout.h"
#include "Strata/Renderer/Mesh.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <tuple>

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
		std::vector<glm::i64vec3>* outFacing)
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
							// Include/Voxelize.slang's IsVoxelBehind.
							const float reach = glm::dot(facing, center - corners[0]) + 0.5f * (std::abs(facing.x) + std::abs(facing.y) + std::abs(facing.z));
							if (outFacing && reach > 0.0f)
								outFacing->push_back(anchorVoxel + glm::i64vec3(x, y, z));
						}
					}
				}
			}
		}
		const auto less = [](const glm::i64vec3& a, const glm::i64vec3& b) { return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z); };
		std::sort(voxels.begin(), voxels.end(), less);
		voxels.erase(std::unique(voxels.begin(), voxels.end()), voxels.end());
		if (outFacing)
		{
			std::sort(outFacing->begin(), outFacing->end(), less);
			outFacing->erase(std::unique(outFacing->begin(), outFacing->end()), outFacing->end());
		}
		return voxels;
	}

	SceneCamera RoomStrafe(uint32_t frame, float aspectRatio)
	{
		const glm::vec3 offset(0.01f * static_cast<float>(frame), 0.0f, 0.0f);
		return StrafeCamera(glm::vec3(-9.0f, 1.5f, -4.5f) + offset, glm::vec3(-10.0f, 1.2f, 1.1f) + offset, 70.0f, aspectRatio);
	}

}
