#include "stpch.h"
#include "Strata/Renderer/MeshFactory.h"

#include <glm/gtc/constants.hpp>

namespace Strata
{

	namespace
	{

		struct MeshBuilder
		{
			std::vector<glm::vec3> Positions;
			std::vector<MeshVertexAttributes> Attributes;
			std::vector<uint32_t> Indices;

			uint32_t AddVertex(const glm::vec3& position, const glm::vec3& normal, const glm::vec2& texCoord)
			{
				Positions.push_back(position);
				MeshVertexAttributes attributes;
				attributes.Normal = normal;
				attributes.TexCoord = texCoord;
				Attributes.push_back(attributes);
				return static_cast<uint32_t>(Positions.size() - 1);
			}

			void AddTriangle(uint32_t a, uint32_t b, uint32_t c)
			{
				Indices.push_back(a);
				Indices.push_back(b);
				Indices.push_back(c);
			}

			// Quad with corners in counter-clockwise order as seen from the front.
			void AddQuad(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
			{
				AddTriangle(a, b, c);
				AddTriangle(a, c, d);
			}

			Ref<Mesh> Build(const char* name, AssetHandle material)
			{
				MeshUtils::GenerateTangents(Positions, Indices, Attributes);
				MeshUtils::OptimizeIndices(Indices, Positions.size());

				Submesh submesh;
				submesh.Name = name;
				submesh.VertexCount = static_cast<uint32_t>(Positions.size());
				submesh.LODs.push_back(MeshLOD { 0, static_cast<uint32_t>(Indices.size()) });
				submesh.Material = material;

				std::string error;
				Ref<Mesh> mesh = Mesh::Create(std::move(Positions), std::move(Attributes), std::move(Indices), { submesh }, &error);
				ST_CORE_ASSERT(mesh, "Primitive mesh '{}' is invalid: {}", name, error);
				return mesh;
			}
		};

		// Ring of vertices around the Y axis at height y; angle 0 points to +Z so UV u = 0 starts at the front.
		glm::vec3 RingDirection(float angle)
		{
			return glm::vec3(glm::sin(angle), 0.0f, glm::cos(angle));
		}

	}

	Ref<Mesh> MeshFactory::CreateCube(float size, AssetHandle material)
	{
		MeshBuilder builder;
		const float h = size * 0.5f;
		struct Face
		{
			glm::vec3 Normal;
			glm::vec3 Right; // +U direction
			glm::vec3 Up;    // +V direction (texture V grows downward, so V = 1 at the bottom)
		};
		const Face faces[] = {
			{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
			{ { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
			{ { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },
			{ { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
			{ { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
			{ { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } }
		};

		for (const Face& face : faces)
		{
			const glm::vec3 center = face.Normal * h;
			const uint32_t a = builder.AddVertex(center - face.Right * h - face.Up * h, face.Normal, { 0.0f, 1.0f });
			const uint32_t b = builder.AddVertex(center + face.Right * h - face.Up * h, face.Normal, { 1.0f, 1.0f });
			const uint32_t c = builder.AddVertex(center + face.Right * h + face.Up * h, face.Normal, { 1.0f, 0.0f });
			const uint32_t d = builder.AddVertex(center - face.Right * h + face.Up * h, face.Normal, { 0.0f, 0.0f });
			builder.AddQuad(a, b, c, d);
		}
		return builder.Build("Cube", material);
	}

	Ref<Mesh> MeshFactory::CreateSphere(float radius, uint32_t segments, uint32_t rings, AssetHandle material)
	{
		segments = std::max(segments, 3u);
		rings = std::max(rings, 2u);
		MeshBuilder builder;
		for (uint32_t ring = 0; ring <= rings; ring++)
		{
			const float v = static_cast<float>(ring) / static_cast<float>(rings);
			const float polar = v * glm::pi<float>(); // 0 at +Y
			for (uint32_t segment = 0; segment <= segments; segment++)
			{
				const float u = static_cast<float>(segment) / static_cast<float>(segments);
				const float azimuth = u * glm::two_pi<float>();
				const glm::vec3 normal = RingDirection(azimuth) * glm::sin(polar) + glm::vec3(0.0f, glm::cos(polar), 0.0f);
				builder.AddVertex(normal * radius, normal, { u, v });
			}
		}

		const uint32_t stride = segments + 1;
		for (uint32_t ring = 0; ring < rings; ring++)
		{
			for (uint32_t segment = 0; segment < segments; segment++)
			{
				const uint32_t topLeft = ring * stride + segment;
				const uint32_t bottomLeft = topLeft + stride;
				// Seen from outside: top-left -> bottom-left -> bottom-right -> top-right is counter-clockwise.
				if (ring != 0)
					builder.AddTriangle(topLeft, bottomLeft, topLeft + 1);
				if (ring != rings - 1)
					builder.AddTriangle(topLeft + 1, bottomLeft, bottomLeft + 1);
			}
		}
		return builder.Build("Sphere", material);
	}

	Ref<Mesh> MeshFactory::CreatePlane(float size, uint32_t subdivisions, AssetHandle material)
	{
		subdivisions = std::max(subdivisions, 1u);
		MeshBuilder builder;
		const float h = size * 0.5f;
		for (uint32_t row = 0; row <= subdivisions; row++)
		{
			const float v = static_cast<float>(row) / static_cast<float>(subdivisions);
			for (uint32_t column = 0; column <= subdivisions; column++)
			{
				const float u = static_cast<float>(column) / static_cast<float>(subdivisions);
				// Row 0 is the far edge (-Z) so that v grows toward +Z (toward the viewer looking down -Z).
				builder.AddVertex({ -h + u * size, 0.0f, -h + v * size }, { 0.0f, 1.0f, 0.0f }, { u, v });
			}
		}

		const uint32_t stride = subdivisions + 1;
		for (uint32_t row = 0; row < subdivisions; row++)
		{
			for (uint32_t column = 0; column < subdivisions; column++)
			{
				const uint32_t farLeft = row * stride + column;
				const uint32_t nearLeft = farLeft + stride;
				// Counter-clockwise seen from above (+Y).
				builder.AddQuad(nearLeft, nearLeft + 1, farLeft + 1, farLeft);
			}
		}
		return builder.Build("Plane", material);
	}

	Ref<Mesh> MeshFactory::CreateQuad(float size, AssetHandle material)
	{
		MeshBuilder builder;
		const float h = size * 0.5f;
		const glm::vec3 normal(0.0f, 0.0f, 1.0f);
		const uint32_t a = builder.AddVertex({ -h, -h, 0.0f }, normal, { 0.0f, 1.0f });
		const uint32_t b = builder.AddVertex({ h, -h, 0.0f }, normal, { 1.0f, 1.0f });
		const uint32_t c = builder.AddVertex({ h, h, 0.0f }, normal, { 1.0f, 0.0f });
		const uint32_t d = builder.AddVertex({ -h, h, 0.0f }, normal, { 0.0f, 0.0f });
		builder.AddQuad(a, b, c, d);
		return builder.Build("Quad", material);
	}

	namespace
	{
		// Adds a flat cap disc at height y facing up (+Y) or down (-Y).
		void AddCap(MeshBuilder& builder, float radius, float y, uint32_t segments, bool up)
		{
			const glm::vec3 normal(0.0f, up ? 1.0f : -1.0f, 0.0f);
			const uint32_t center = builder.AddVertex({ 0.0f, y, 0.0f }, normal, { 0.5f, 0.5f });
			const uint32_t first = static_cast<uint32_t>(builder.Positions.size());
			for (uint32_t segment = 0; segment <= segments; segment++)
			{
				const float angle = static_cast<float>(segment) / static_cast<float>(segments) * glm::two_pi<float>();
				const glm::vec3 direction = RingDirection(angle);
				builder.AddVertex(direction * radius + glm::vec3(0.0f, y, 0.0f), normal, { 0.5f + direction.x * 0.5f, 0.5f - direction.z * 0.5f * (up ? 1.0f : -1.0f) });
			}
			for (uint32_t segment = 0; segment < segments; segment++)
			{
				// Angle increases counter-clockwise when viewed from +Y (sin/cos order), so upward caps keep that order.
				if (up)
					builder.AddTriangle(center, first + segment, first + segment + 1);
				else
					builder.AddTriangle(center, first + segment + 1, first + segment);
			}
		}

		// Side wall between two rings (bottom radius/height to top radius/height).
		void AddWall(MeshBuilder& builder, float bottomRadius, float bottomY, float topRadius, float topY, uint32_t segments)
		{
			const float slope = (bottomRadius - topRadius) / std::max(topY - bottomY, 1e-6f);
			const uint32_t first = static_cast<uint32_t>(builder.Positions.size());
			for (uint32_t segment = 0; segment <= segments; segment++)
			{
				const float u = static_cast<float>(segment) / static_cast<float>(segments);
				const glm::vec3 direction = RingDirection(u * glm::two_pi<float>());
				const glm::vec3 normal = glm::normalize(direction + glm::vec3(0.0f, slope, 0.0f));
				builder.AddVertex(direction * bottomRadius + glm::vec3(0.0f, bottomY, 0.0f), normal, { u, 1.0f });
				builder.AddVertex(direction * topRadius + glm::vec3(0.0f, topY, 0.0f), normal, { u, 0.0f });
			}
			for (uint32_t segment = 0; segment < segments; segment++)
			{
				const uint32_t bottom = first + segment * 2;
				const uint32_t top = bottom + 1;
				builder.AddQuad(bottom, bottom + 2, top + 2, top);
			}
		}
	}

	Ref<Mesh> MeshFactory::CreateCylinder(float radius, float height, uint32_t segments, AssetHandle material)
	{
		segments = std::max(segments, 3u);
		MeshBuilder builder;
		const float h = height * 0.5f;
		AddWall(builder, radius, -h, radius, h, segments);
		AddCap(builder, radius, h, segments, true);
		AddCap(builder, radius, -h, segments, false);
		return builder.Build("Cylinder", material);
	}

	Ref<Mesh> MeshFactory::CreateCone(float radius, float height, uint32_t segments, AssetHandle material)
	{
		segments = std::max(segments, 3u);
		MeshBuilder builder;
		const float h = height * 0.5f;
		// A tiny top radius keeps per-segment normals well defined at the apex.
		AddWall(builder, radius, -h, radius * 1e-4f, h, segments);
		AddCap(builder, radius, -h, segments, false);
		return builder.Build("Cone", material);
	}

	Ref<Mesh> MeshFactory::CreateCapsule(float radius, float halfHeight, uint32_t segments, uint32_t rings, AssetHandle material)
	{
		segments = std::max(segments, 3u);
		rings = std::max(rings, 2u);
		MeshBuilder builder;

		// Rows from the top pole to the bottom pole: hemisphere rings with the cylinder in between (two rows at the equator).
		struct Row
		{
			float Polar;  // Angle from +Y for the normal
			float Offset; // Vertical offset of the sphere center
		};
		std::vector<Row> rows;
		for (uint32_t ring = 0; ring <= rings; ring++)
			rows.push_back({ static_cast<float>(ring) / static_cast<float>(rings) * glm::half_pi<float>(), halfHeight });
		for (uint32_t ring = 0; ring <= rings; ring++)
			rows.push_back({ glm::half_pi<float>() + static_cast<float>(ring) / static_cast<float>(rings) * glm::half_pi<float>(), -halfHeight });

		const float totalHeight = 2.0f * (halfHeight + radius);
		for (const Row& row : rows)
		{
			for (uint32_t segment = 0; segment <= segments; segment++)
			{
				const float u = static_cast<float>(segment) / static_cast<float>(segments);
				const glm::vec3 normal = RingDirection(u * glm::two_pi<float>()) * glm::sin(row.Polar) + glm::vec3(0.0f, glm::cos(row.Polar), 0.0f);
				const glm::vec3 position = normal * radius + glm::vec3(0.0f, row.Offset, 0.0f);
				builder.AddVertex(position, normal, { u, (halfHeight + radius - position.y) / totalHeight });
			}
		}

		const uint32_t stride = segments + 1;
		for (uint32_t row = 0; row + 1 < rows.size(); row++)
		{
			for (uint32_t segment = 0; segment < segments; segment++)
			{
				const uint32_t topLeft = row * stride + segment;
				const uint32_t bottomLeft = topLeft + stride;
				if (row != 0)
					builder.AddTriangle(topLeft, bottomLeft, topLeft + 1);
				if (row + 2 != rows.size())
					builder.AddTriangle(topLeft + 1, bottomLeft, bottomLeft + 1);
			}
		}
		return builder.Build("Capsule", material);
	}

	Ref<Mesh> MeshFactory::CreateTorus(float majorRadius, float minorRadius, uint32_t majorSegments, uint32_t minorSegments, AssetHandle material)
	{
		majorSegments = std::max(majorSegments, 3u);
		minorSegments = std::max(minorSegments, 3u);
		MeshBuilder builder;
		for (uint32_t major = 0; major <= majorSegments; major++)
		{
			const float u = static_cast<float>(major) / static_cast<float>(majorSegments);
			const glm::vec3 ringDirection = RingDirection(u * glm::two_pi<float>());
			for (uint32_t minor = 0; minor <= minorSegments; minor++)
			{
				const float v = static_cast<float>(minor) / static_cast<float>(minorSegments);
				const float angle = v * glm::two_pi<float>();
				const glm::vec3 normal = ringDirection * glm::cos(angle) + glm::vec3(0.0f, glm::sin(angle), 0.0f);
				builder.AddVertex(ringDirection * majorRadius + normal * minorRadius, normal, { u, v });
			}
		}

		const uint32_t stride = minorSegments + 1;
		for (uint32_t major = 0; major < majorSegments; major++)
		{
			for (uint32_t minor = 0; minor < minorSegments; minor++)
			{
				const uint32_t a = major * stride + minor;
				const uint32_t b = a + stride;
				builder.AddQuad(a, b, b + 1, a + 1);
			}
		}
		return builder.Build("Torus", material);
	}

}
