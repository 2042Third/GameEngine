#include <doctest/doctest.h>

#include "Renderer/MeshTestUtils.h"
#include "Strata/Asset/MeshGeometry.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"

#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace Strata;

namespace
{

	struct NamedMesh
	{
		std::string Name;
		Ref<Mesh> Asset;
		bool Convex = true;
	};

	std::vector<NamedMesh> CreateFactoryMeshes()
	{
		return {
			{ "Cube", MeshFactory::CreateCube() },
			{ "Sphere", MeshFactory::CreateSphere() },
			{ "Plane", MeshFactory::CreatePlane(1.0f, 4), false },
			{ "Quad", MeshFactory::CreateQuad(), false },
			{ "Cylinder", MeshFactory::CreateCylinder() },
			{ "Capsule", MeshFactory::CreateCapsule() },
			{ "Cone", MeshFactory::CreateCone() },
			{ "Torus", MeshFactory::CreateTorus(), false }
		};
	}

	// A 1x1 quad in the XY plane facing +Z with a single submesh.
	Ref<Mesh> CreateTestQuad(std::string* outError = nullptr)
	{
		std::vector<glm::vec3> positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } };
		std::vector<MeshVertexAttributes> attributes(4);
		attributes[0].TexCoord = { 0, 1 };
		attributes[1].TexCoord = { 1, 1 };
		attributes[2].TexCoord = { 1, 0 };
		attributes[3].TexCoord = { 0, 0 };
		std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3 };
		Submesh submesh;
		submesh.Name = "Quad";
		submesh.VertexCount = 4;
		submesh.LODs = { MeshLOD { 0, 6 } };
		return Mesh::Create(std::move(positions), std::move(attributes), std::move(indices), { submesh }, outError);
	}

	using Triangle = std::array<uint32_t, 3>;

	// Triangles with their indices rotated so the smallest comes first (winding preserved), sorted.
	std::vector<Triangle> CanonicalTriangles(const std::vector<uint32_t>& indices)
	{
		std::vector<Triangle> triangles;
		for (size_t index = 0; index + 2 < indices.size(); index += 3)
		{
			Triangle triangle = { indices[index], indices[index + 1], indices[index + 2] };
			while (triangle[0] > triangle[1] || triangle[0] > triangle[2])
				std::rotate(triangle.begin(), triangle.begin() + 1, triangle.end());
			triangles.push_back(triangle);
		}
		std::sort(triangles.begin(), triangles.end());
		return triangles;
	}

}

TEST_SUITE("Renderer.Mesh")
{
	TEST_CASE("Factory meshes have counter-clockwise front faces and consistent vertex data")
	{
		for (const NamedMesh& named : CreateFactoryMeshes())
		{
			CAPTURE(named.Name);
			REQUIRE(named.Asset);
			const Mesh& mesh = *named.Asset;
			CHECK(mesh.GetTriangleCount() > 0);

			const std::vector<glm::vec3>& positions = mesh.GetPositions();
			const std::vector<MeshVertexAttributes>& attributes = mesh.GetAttributes();
			const std::vector<uint32_t>& indices = mesh.GetIndices();

			uint32_t badNormals = 0;
			uint32_t badTangents = 0;
			for (size_t vertex = 0; vertex < positions.size(); vertex++)
			{
				const MeshVertexAttributes& attribute = attributes[vertex];
				badNormals += std::abs(glm::length(attribute.Normal) - 1.0f) > 1e-3f ? 1 : 0;
				const glm::vec3 tangent = glm::vec3(attribute.Tangent);
				const bool tangentValid = std::abs(glm::length(tangent) - 1.0f) < 1e-3f && std::abs(glm::dot(tangent, attribute.Normal)) < 1e-2f
					&& std::abs(std::abs(attribute.Tangent.w) - 1.0f) < 1e-6f;
				badTangents += tangentValid ? 0 : 1;
				CHECK(mesh.GetBounds().Contains(positions[vertex]));
			}
			CHECK(badNormals == 0);
			CHECK(badTangents == 0);

			uint32_t inwardTriangles = 0;
			uint32_t windingMismatches = 0;
			for (const Submesh& submesh : mesh.GetSubmeshes())
			{
				const MeshLOD& lod = submesh.LODs[0];
				for (uint32_t index = lod.IndexOffset; index < lod.IndexOffset + lod.IndexCount; index += 3)
				{
					const uint32_t a = submesh.BaseVertex + indices[index];
					const uint32_t b = submesh.BaseVertex + indices[index + 1];
					const uint32_t c = submesh.BaseVertex + indices[index + 2];
					const glm::vec3 faceNormal = glm::cross(positions[b] - positions[a], positions[c] - positions[a]);
					if (glm::length(faceNormal) < 1e-9f)
						continue; // Degenerate triangles at poles carry no winding

					const glm::vec3 vertexNormal = attributes[a].Normal + attributes[b].Normal + attributes[c].Normal;
					windingMismatches += glm::dot(faceNormal, vertexNormal) > 0.0f ? 0 : 1;
					const glm::vec3 center = (positions[a] + positions[b] + positions[c]) / 3.0f;
					if (named.Convex)
						inwardTriangles += glm::dot(faceNormal, center - mesh.GetBounds().GetCenter()) > 0.0f ? 0 : 1;
				}
			}
			CHECK(windingMismatches == 0);
			CHECK(inwardTriangles == 0);
		}
	}

	TEST_CASE("Flat factory meshes face their documented direction")
	{
		Ref<Mesh> plane = MeshFactory::CreatePlane();
		Ref<Mesh> quad = MeshFactory::CreateQuad();
		REQUIRE(plane);
		REQUIRE(quad);
		for (const MeshVertexAttributes& attribute : plane->GetAttributes())
			CHECK(glm::all(glm::epsilonEqual(attribute.Normal, glm::vec3(0, 1, 0), 1e-5f)));
		for (const MeshVertexAttributes& attribute : quad->GetAttributes())
			CHECK(glm::all(glm::epsilonEqual(attribute.Normal, glm::vec3(0, 0, 1), 1e-5f)));
	}

	TEST_CASE("Factory meshes reference the given material")
	{
		const AssetHandle material = UUID(0x1234);
		Ref<Mesh> cube = MeshFactory::CreateCube(2.0f, material);
		REQUIRE(cube);
		REQUIRE(cube->GetSubmeshes().size() == 1);
		CHECK(cube->GetSubmeshes()[0].Material == material);
		CHECK(cube->GetBounds().GetSize().x == doctest::Approx(2.0f));
	}

	TEST_CASE("Mesh serialization round trips")
	{
		Ref<Mesh> source = MeshFactory::CreateSphere(0.5f, 32, 16, UUID(0xABCD));
		REQUIRE(source);
		const std::vector<uint8_t> cooked = source->Serialize();

		std::string error;
		Ref<Mesh> loaded = Mesh::Deserialize(cooked, &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetPositions() == source->GetPositions());
		CHECK(loaded->GetIndices() == source->GetIndices());
		REQUIRE(loaded->GetAttributes().size() == source->GetAttributes().size());
		for (size_t index = 0; index < source->GetAttributes().size(); index++)
		{
			CHECK(loaded->GetAttributes()[index].Normal == source->GetAttributes()[index].Normal);
			CHECK(loaded->GetAttributes()[index].Tangent == source->GetAttributes()[index].Tangent);
			CHECK(loaded->GetAttributes()[index].TexCoord == source->GetAttributes()[index].TexCoord);
		}
		REQUIRE(loaded->GetSubmeshes().size() == source->GetSubmeshes().size());
		for (size_t index = 0; index < source->GetSubmeshes().size(); index++)
		{
			const Submesh& expected = source->GetSubmeshes()[index];
			const Submesh& actual = loaded->GetSubmeshes()[index];
			CHECK(actual.Name == expected.Name);
			CHECK(actual.BaseVertex == expected.BaseVertex);
			CHECK(actual.VertexCount == expected.VertexCount);
			CHECK(actual.Material == expected.Material);
			REQUIRE(actual.LODs.size() == expected.LODs.size());
			for (size_t lod = 0; lod < expected.LODs.size(); lod++)
			{
				CHECK(actual.LODs[lod].IndexOffset == expected.LODs[lod].IndexOffset);
				CHECK(actual.LODs[lod].IndexCount == expected.LODs[lod].IndexCount);
				CHECK(actual.LODs[lod].Error == expected.LODs[lod].Error);
			}
			CHECK(actual.Bounds.Min == expected.Bounds.Min);
			CHECK(actual.Bounds.Max == expected.Bounds.Max);
		}
		CHECK(loaded->Serialize() == cooked);
	}

	TEST_CASE("Cooked meshes keep their levels' errors, and version 2 meshes load without them")
	{
		// Two levels of a quad pair, with an error the cooked bytes hold once.
		const std::vector<glm::vec3> positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } };
		const std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3, 0, 1, 2 };
		constexpr float c_Error = 0.123456f;
		Submesh submesh;
		submesh.Name = "Levels";
		submesh.VertexCount = 4;
		submesh.LODs = { MeshLOD { 0, 6, 0.0f }, MeshLOD { 6, 3, c_Error } };
		std::string error;
		const Ref<Mesh> source = Mesh::Create(positions, std::vector<MeshVertexAttributes>(4), indices, { submesh }, &error);
		REQUIRE_MESSAGE(source, error);
		const std::vector<uint8_t> cooked = source->Serialize();
		const Ref<Mesh> loaded = Mesh::Deserialize(cooked, &error);
		REQUIRE_MESSAGE(loaded, error);
		REQUIRE(loaded->GetSubmeshes()[0].LODs.size() == 2);
		CHECK(loaded->GetSubmeshes()[0].LODs[1].Error == c_Error);

		// Errors that are not finite, or negative, are rejected, created or loaded.
		const auto errorOffset = [&]()
		{
			for (size_t offset = 0; offset + sizeof(float) <= cooked.size(); offset++)
			{
				float value = 0.0f;
				std::memcpy(&value, cooked.data() + offset, sizeof(float));
				if (value == c_Error)
					return offset;
			}
			return cooked.size();
		}();
		REQUIRE(errorOffset < cooked.size());
		for (const float invalid : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1.0f })
		{
			CAPTURE(invalid);
			std::vector<uint8_t> corrupted = cooked;
			std::memcpy(corrupted.data() + errorOffset, &invalid, sizeof(float));
			error.clear();
			CHECK_FALSE(Mesh::Deserialize(corrupted, &error));
			CHECK(error.find("error") != std::string::npos);
			Submesh bad = submesh;
			bad.LODs[1].Error = invalid;
			CHECK_FALSE(Mesh::Create(positions, std::vector<MeshVertexAttributes>(4), indices, { bad }));
		}
		// Versions this build does not know are refused.
		std::vector<uint8_t> future = cooked;
		const uint32_t version = Mesh::c_CookedVersion + 1;
		std::memcpy(future.data() + sizeof(uint32_t), &version, sizeof(uint32_t));
		CHECK_FALSE(Mesh::Deserialize(future));

		// Version 2 wrote its levels without errors: they load with Error 0 (choose by halving).
		const std::vector<uint8_t> version2 = Tests::SerializeMeshVersion2(*source);
		const Ref<Mesh> old = Mesh::Deserialize(version2, &error);
		REQUIRE_MESSAGE(old, error);
		REQUIRE(old->GetSubmeshes()[0].LODs.size() == 2);
		CHECK(old->GetSubmeshes()[0].LODs[1].IndexOffset == 6);
		CHECK(old->GetSubmeshes()[0].LODs[1].IndexCount == 3);
		CHECK(old->GetSubmeshes()[0].LODs[1].Error == 0.0f);
		// Saved again, it is a version 3 mesh.
		const std::vector<uint8_t> resaved = old->Serialize();
		uint32_t resavedVersion = 0;
		std::memcpy(&resavedVersion, resaved.data() + sizeof(uint32_t), sizeof(uint32_t));
		CHECK(resavedVersion == Mesh::c_CookedVersion);
		// Every truncation of the old form fails cleanly too.
		for (size_t length = 0; length < version2.size(); length++)
			CHECK_FALSE(Mesh::Deserialize(std::span<const uint8_t>(version2.data(), length)));
	}

	TEST_CASE("Mesh deserialization rejects corrupt data")
	{
		Ref<Mesh> source = MeshFactory::CreateCube();
		REQUIRE(source);
		const std::vector<uint8_t> cooked = source->Serialize();

		std::string error;
		CHECK_FALSE(Mesh::Deserialize({}, &error));
		CHECK_FALSE(error.empty());

		// Every truncation fails cleanly.
		for (size_t length = 0; length < cooked.size(); length += std::max<size_t>(1, cooked.size() / 97))
		{
			CAPTURE(length);
			CHECK_FALSE(Mesh::Deserialize(std::span<const uint8_t>(cooked.data(), length)));
		}

		std::vector<uint8_t> wrongMagic = cooked;
		wrongMagic[0] ^= 0xFF;
		CHECK_FALSE(Mesh::Deserialize(wrongMagic));

		// Corrupting any single byte never crashes; whatever loads still passes validation.
		for (size_t offset = 0; offset < cooked.size(); offset += 7)
		{
			std::vector<uint8_t> corrupted = cooked;
			corrupted[offset] ^= 0x5A;
			if (Ref<Mesh> mesh = Mesh::Deserialize(corrupted))
			{
				for (const Submesh& submesh : mesh->GetSubmeshes())
				{
					for (const MeshLOD& lod : submesh.LODs)
						CHECK(static_cast<uint64_t>(lod.IndexOffset) + lod.IndexCount <= mesh->GetIndices().size());
				}
			}
		}
	}

	TEST_CASE("Mesh creation validates its input")
	{
		std::string error;
		CHECK(CreateTestQuad(&error));

		auto attempt = [&](std::vector<glm::vec3> positions, std::vector<MeshVertexAttributes> attributes, std::vector<uint32_t> indices, std::vector<Submesh> submeshes)
		{
			error.clear();
			Ref<Mesh> mesh = Mesh::Create(std::move(positions), std::move(attributes), std::move(indices), std::move(submeshes), &error);
			CHECK_FALSE(error.empty());
			return mesh;
		};

		Submesh submesh;
		submesh.VertexCount = 3;
		submesh.LODs = { MeshLOD { 0, 3 } };
		const std::vector<glm::vec3> triangle = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };

		CHECK_FALSE(attempt({}, {}, {}, { submesh }));
		CHECK_FALSE(attempt(triangle, std::vector<MeshVertexAttributes>(2), { 0, 1, 2 }, { submesh }));
		CHECK_FALSE(attempt(triangle, std::vector<MeshVertexAttributes>(3), { 0, 1, 2 }, {}));
		CHECK_FALSE(attempt(triangle, std::vector<MeshVertexAttributes>(3), { 0, 1, 3 }, { submesh }));
		CHECK_FALSE(attempt({ { 0, 0, 0 }, { 1, 0, 0 }, { 0, std::numeric_limits<float>::quiet_NaN(), 0 } }, std::vector<MeshVertexAttributes>(3), { 0, 1, 2 }, { submesh }));

		Submesh badRange = submesh;
		badRange.LODs = { MeshLOD { 0, 2 } };
		CHECK_FALSE(attempt(triangle, std::vector<MeshVertexAttributes>(3), { 0, 1, 2 }, { badRange }));
		badRange.LODs = { MeshLOD { 3, 3 } };
		CHECK_FALSE(attempt(triangle, std::vector<MeshVertexAttributes>(3), { 0, 1, 2 }, { badRange }));
		Submesh outside = submesh;
		outside.BaseVertex = 1;
		CHECK_FALSE(attempt(triangle, std::vector<MeshVertexAttributes>(3), { 0, 1, 2 }, { outside }));
	}

	TEST_CASE("Mesh utilities generate normals, tangents, optimized indices and LODs")
	{
		const std::vector<glm::vec3> positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } };
		std::vector<MeshVertexAttributes> attributes(4);
		attributes[0].TexCoord = { 0, 1 };
		attributes[1].TexCoord = { 1, 1 };
		attributes[2].TexCoord = { 1, 0 };
		attributes[3].TexCoord = { 0, 0 };
		const std::vector<uint32_t> indices = { 0, 1, 2, 0, 2, 3 };

		MeshUtils::GenerateNormals(positions, indices, attributes);
		for (const MeshVertexAttributes& attribute : attributes)
			CHECK(glm::all(glm::epsilonEqual(attribute.Normal, glm::vec3(0, 0, 1), 1e-5f)));

		REQUIRE(MeshUtils::GenerateTangents(positions, indices, attributes));
		for (const MeshVertexAttributes& attribute : attributes)
		{
			// U grows along +X, so the tangent points along +X.
			CHECK(glm::all(glm::epsilonEqual(glm::vec3(attribute.Tangent), glm::vec3(1, 0, 0), 1e-4f)));
			CHECK(std::abs(attribute.Tangent.w) == doctest::Approx(1.0f));
		}

		Ref<Mesh> sphere = MeshFactory::CreateSphere(0.5f, 64, 32);
		REQUIRE(sphere);
		std::vector<uint32_t> sphereIndices(sphere->GetIndices().begin(), sphere->GetIndices().begin() + sphere->GetSubmeshes()[0].LODs[0].IndexCount);
		std::vector<uint32_t> optimized = sphereIndices;
		MeshUtils::OptimizeIndices(optimized, sphere->GetPositions().size());
		CHECK(CanonicalTriangles(optimized) == CanonicalTriangles(sphereIndices));

		const std::vector<MeshUtils::GeneratedLOD> lods = MeshUtils::GenerateLODs(sphere->GetPositions(), sphere->GetAttributes(), sphereIndices, 3);
		REQUIRE_FALSE(lods.empty());
		size_t previousCount = sphereIndices.size();
		float previousError = 0.0f;
		// The simplifier's error is relative to the extent; the levels' are in the mesh's units, within the tolerance.
		const glm::vec3 size = sphere->GetBounds().GetSize();
		const float extent = std::max({ size.x, size.y, size.z });
		for (const MeshUtils::GeneratedLOD& lod : lods)
		{
			CHECK(lod.Indices.size() % 3 == 0);
			CHECK(lod.Indices.size() < previousCount);
			CHECK(std::all_of(lod.Indices.begin(), lod.Indices.end(), [&](uint32_t index) { return index < sphere->GetPositions().size(); }));
			CHECK(lod.Error > 0.0f);
			CHECK(lod.Error >= previousError);
			CHECK(lod.Error <= MeshUtils::c_MaxLODError * extent * 1.001f);
			previousCount = lod.Indices.size();
			previousError = lod.Error;
		}

		const AABB bounds = MeshUtils::ComputeBounds(positions);
		CHECK(bounds.Min == glm::vec3(0, 0, 0));
		CHECK(bounds.Max == glm::vec3(1, 1, 0));
	}

	TEST_CASE("Mesh utilities reject invalid triangle lists without changing their input")
	{
		std::vector<glm::vec3> positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 } };
		std::vector<MeshVertexAttributes> attributes(3);
		const std::vector<glm::vec3> originalPositions = positions;

		for (const std::vector<uint32_t>& invalid : { std::vector<uint32_t> { 0, 1 }, std::vector<uint32_t> { 0, 1, 3 }, std::vector<uint32_t>() })
		{
			std::vector<uint32_t> indices = invalid;
			CHECK_FALSE(MeshUtils::GenerateSeamTangents(positions, attributes, indices, true));
			CHECK(indices == invalid);
			CHECK(positions == originalPositions);
			CHECK(attributes.size() == 3);

			MeshUtils::OptimizeIndices(indices, positions.size());
			CHECK(indices == invalid); // Out-of-range or incomplete triangles are left alone
			CHECK(MeshUtils::GenerateLODs(positions, attributes, invalid, 2).empty());
		}

		// Mismatched streams are rejected too.
		std::vector<uint32_t> indices = { 0, 1, 2 };
		std::vector<MeshVertexAttributes> tooFew(2);
		CHECK_FALSE(MeshUtils::GenerateSeamTangents(positions, tooFew, indices, false));
		CHECK(indices == std::vector<uint32_t> { 0, 1, 2 });
		CHECK(MeshUtils::ComputeUVDensity(positions, tooFew, indices) == glm::vec3(0.0f));
		CHECK(MeshUtils::GenerateLODs(positions, tooFew, indices, 2).empty());
	}

	TEST_CASE("Generated levels of detail keep texture coordinates and open borders")
	{
		// A flat grid in the XY plane whose U follows x squared: simplifying its positions alone could collapse it to two
		// triangles at no error, stretching the texture across it.
		constexpr uint32_t c_Side = 33;
		std::vector<glm::vec3> positions;
		std::vector<MeshVertexAttributes> attributes;
		for (uint32_t y = 0; y < c_Side; y++)
		{
			for (uint32_t x = 0; x < c_Side; x++)
			{
				const glm::vec2 point(static_cast<float>(x) / (c_Side - 1), static_cast<float>(y) / (c_Side - 1));
				positions.push_back(glm::vec3(point, 0.0f));
				MeshVertexAttributes attribute;
				attribute.Normal = glm::vec3(0.0f, 0.0f, 1.0f);
				attribute.TexCoord = glm::vec2(point.x * point.x, point.y);
				attributes.push_back(attribute);
			}
		}
		std::vector<uint32_t> indices;
		for (uint32_t y = 0; y + 1 < c_Side; y++)
		{
			for (uint32_t x = 0; x + 1 < c_Side; x++)
			{
				const uint32_t corner = y * c_Side + x;
				indices.insert(indices.end(), { corner, corner + 1, corner + c_Side + 1, corner, corner + c_Side + 1, corner + c_Side });
			}
		}

		const std::vector<MeshUtils::GeneratedLOD> lods = MeshUtils::GenerateLODs(positions, attributes, indices, 4);
		REQUIRE_FALSE(lods.empty());
		for (size_t level = 0; level < lods.size(); level++)
		{
			CAPTURE(level);
			const std::vector<uint32_t>& lod = lods[level].Indices;
			// Every vertex of the grid's open border is still there (submeshes meeting along it would part otherwise).
			std::vector<bool> used(positions.size(), false);
			for (uint32_t index : lod)
				used[index] = true;
			for (uint32_t vertex = 0; vertex < positions.size(); vertex++)
			{
				const uint32_t x = vertex % c_Side;
				const uint32_t y = vertex / c_Side;
				if (x == 0 || y == 0 || x + 1 == c_Side || y + 1 == c_Side)
					CHECK_MESSAGE(used[vertex], "border vertex ", vertex, " was simplified away");
			}
			// Every vertex of the grid, located in the level's triangles, finds about its texture coordinates there.
			float worst = 0.0f;
			for (uint32_t vertex = 0; vertex < positions.size(); vertex++)
			{
				const glm::vec2 point(positions[vertex]);
				bool found = false;
				for (size_t corner = 0; corner + 2 < lod.size() && !found; corner += 3)
				{
					const glm::vec2 a(positions[lod[corner]]);
					const glm::vec2 b(positions[lod[corner + 1]]);
					const glm::vec2 c(positions[lod[corner + 2]]);
					const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
					if (std::abs(area) < 1e-9f)
						continue;
					const float wb = ((point.x - a.x) * (c.y - a.y) - (c.x - a.x) * (point.y - a.y)) / area;
					const float wc = ((b.x - a.x) * (point.y - a.y) - (point.x - a.x) * (b.y - a.y)) / area;
					const float wa = 1.0f - wb - wc;
					if (wa < -1e-4f || wb < -1e-4f || wc < -1e-4f)
						continue;
					const glm::vec2 uv = attributes[lod[corner]].TexCoord * wa + attributes[lod[corner + 1]].TexCoord * wb + attributes[lod[corner + 2]].TexCoord * wc;
					worst = std::max(worst, glm::length(uv - attributes[vertex].TexCoord));
					found = true;
				}
				CHECK(found);
			}
			CHECK(worst < 0.05f);
		}
	}

	TEST_CASE("Submeshes know their texture coordinates per unit of their surfaces, per the axis the surfaces face")
	{
		// Texture streaming derives the mip level a submesh shows from it (TextureStreamingPolicy::GetUVPixels).
		const auto densityOf = [](const Ref<Mesh>& mesh)
		{
			REQUIRE(mesh);
			REQUIRE(mesh->GetSubmeshes().size() == 1);
			return mesh->GetSubmeshes().front().UVDensity;
		};
		const auto equals = [](const glm::vec3& a, const glm::vec3& b)
		{
			return glm::all(glm::epsilonEqual(a, b, 1e-4f));
		};
		// Each face of the cube spans its texture once over one unit; a quad and a plane face one axis; larger ones spread
		// the texture thinner.
		CHECK(equals(densityOf(MeshFactory::CreateCube()), glm::vec3(1.0f)));
		CHECK(equals(densityOf(MeshFactory::CreateCube(4.0f)), glm::vec3(0.25f)));
		CHECK(equals(densityOf(MeshFactory::CreateQuad()), glm::vec3(0.0f, 0.0f, 1.0f)));
		CHECK(equals(densityOf(MeshFactory::CreatePlane(1.0f, 4)), glm::vec3(0.0f, 1.0f, 0.0f)));
		CHECK(equals(densityOf(MeshFactory::CreatePlane(100.0f, 1)), glm::vec3(0.0f, 0.01f, 0.0f)));
		CHECK(equals(densityOf(CreateTestQuad()), glm::vec3(0.0f, 0.0f, 1.0f)));
		// A sphere wraps the texture once around (1 / (2 pi r) along its equator, denser toward its poles): every axis is faced.
		const glm::vec3 sphere = densityOf(MeshFactory::CreateSphere(0.5f));
		for (glm::length_t axis = 0; axis < 3; axis++)
		{
			CHECK(sphere[axis] > 0.2f);
			CHECK(sphere[axis] < 1.0f);
		}

		// Texture coordinates that do not change, or that are not finite, have no density; slivers (under a hundredth of the
		// area) do not decide an axis.
		std::vector<glm::vec3> positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 0, 0 }, { 0, 0.001f, 0 }, { 0, 0.001f, 0.001f } };
		std::vector<MeshVertexAttributes> attributes(6);
		std::vector<uint32_t> indices = { 0, 1, 2, 3, 4, 5 };
		CHECK(MeshUtils::ComputeUVDensity(positions, attributes, indices) == glm::vec3(0.0f));
		attributes[1].TexCoord = { 1.0f, 0.0f };
		attributes[2].TexCoord = { 1.0f, 1.0f };
		attributes[4].TexCoord = { 0.0f, 1.0f };
		attributes[5].TexCoord = { 1.0f, 1.0f };
		CHECK(equals(MeshUtils::ComputeUVDensity(positions, attributes, indices), glm::vec3(0.0f, 0.0f, 1.0f)));
		attributes[2].TexCoord = { std::numeric_limits<float>::quiet_NaN(), 1.0f };
		CHECK(MeshUtils::ComputeUVDensity(positions, attributes, std::vector<uint32_t> { 0, 1, 2 }) == glm::vec3(0.0f));
		// The serialized form keeps nothing of it: loading computes it again.
		const Ref<Mesh> cube = MeshFactory::CreateCube(2.0f);
		REQUIRE(cube);
		const Ref<Mesh> loaded = Mesh::Deserialize(cube->Serialize());
		REQUIRE(loaded);
		CHECK(loaded->GetSubmeshes().front().UVDensity == cube->GetSubmeshes().front().UVDensity);
	}

	TEST_CASE("The renderer's meshes give their full-detail triangles to modules that read mesh geometry")
	{
		// Two submeshes in one vertex list: the second's indices count from its base vertex, and only each submesh's first
		// level of detail is read.
		const std::vector<glm::vec3> positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 2, 0, 0 }, { 3, 0, 0 }, { 2, 1, 0 }, { 3, 1, 0 } };
		Submesh first;
		first.VertexCount = 3;
		first.LODs = { MeshLOD { 0, 3 } };
		Submesh second;
		second.BaseVertex = 3;
		second.VertexCount = 4;
		second.LODs = { MeshLOD { 3, 6 }, MeshLOD { 9, 3 } };
		std::string error;
		const Ref<Mesh> mesh = Mesh::Create(positions, std::vector<MeshVertexAttributes>(positions.size()), { 0, 1, 2, 0, 1, 2, 1, 3, 2, 0, 1, 2 },
			{ first, second }, &error);
		REQUIRE_MESSAGE(mesh, error);

		MeshGeometry geometry;
		REQUIRE(MeshGeometryRegistry::Read(*mesh, geometry));
		CHECK(geometry.Positions == positions);
		CHECK(geometry.Indices == std::vector<uint32_t> { 0, 1, 2, 3, 4, 5, 4, 6, 5 });

		// Assets of types no module reads geometry from have none, and leave nothing behind.
		const Ref<Material> material = Material::Create();
		CHECK_FALSE(MeshGeometryRegistry::Read(*material, geometry));
		CHECK(geometry.Positions.empty());
		CHECK(geometry.Indices.empty());
	}
}
