#include <doctest/doctest.h>

#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"

#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

using namespace Strata;

namespace
{

	struct NamedMesh
	{
		std::string Name;
		Ref<Mesh> Mesh;
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
			REQUIRE(named.Mesh);
			const Mesh& mesh = *named.Mesh;
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
			}
			CHECK(actual.Bounds.Min == expected.Bounds.Min);
			CHECK(actual.Bounds.Max == expected.Bounds.Max);
		}
		CHECK(loaded->Serialize() == cooked);
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

		const std::vector<std::vector<uint32_t>> lods = MeshUtils::GenerateLODs(sphere->GetPositions(), sphereIndices, 3);
		REQUIRE_FALSE(lods.empty());
		size_t previousCount = sphereIndices.size();
		for (const std::vector<uint32_t>& lod : lods)
		{
			CHECK(lod.size() % 3 == 0);
			CHECK(lod.size() < previousCount);
			CHECK(std::all_of(lod.begin(), lod.end(), [&](uint32_t index) { return index < sphere->GetPositions().size(); }));
			previousCount = lod.size();
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
			CHECK(MeshUtils::GenerateLODs(positions, invalid, 2).empty());
		}

		// Mismatched streams are rejected too.
		std::vector<uint32_t> indices = { 0, 1, 2 };
		std::vector<MeshVertexAttributes> tooFew(2);
		CHECK_FALSE(MeshUtils::GenerateSeamTangents(positions, tooFew, indices, false));
		CHECK(indices == std::vector<uint32_t> { 0, 1, 2 });
	}
}
