#pragma once

#include "Strata/Core/Base64.h"
#include "Strata/Core/JsonUtils.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace Strata::Tests
{

	// A skinned arm as a glTF document: a square tube along +Y from 0 to 2 (rings of 8 vertices every 0.25 units, 72
	// vertices) skinned to three joints Shoulder (y = 0), Elbow (y = 1) and Wrist (y = 2) below an "Armature" node, drawn by
	// the node "Arm" (whose own transform glTF ignores). Vertices below 0.75 follow the shoulder, those from 1.25 to 1.75 the
	// elbow, those at 2 the wrist; the rings at 0.75, 1 and 1.25 blend the shoulder and the elbow. Two animations: "Wave"
	// (the elbow turns 0 -> 90 -> 0 degrees about +Z over 1 second, linear, sampled at 30 keys per second) and "Reach" (the
	// shoulder steps to x = 1 at 0.5 s, the wrist turns along a cubic spline over 2 seconds).
	struct SkinnedArmOptions
	{
		bool Animations = true;
		bool InverseBindMatrices = true;
		float ArmNodeOffset = 5.0f; // Translation of the node that draws the mesh (ignored by glTF for skinned meshes)
		// The "Armature" node's rotation about +X and uniform scale. The vertices, the joints' translations and the inverse bind
		// matrices are authored in the armature's space, as Blender and Mixamo export them, so that the arm still stands along
		// +Y from 0 to 2 once posed: -90 degrees is a Z-up file (the arm along +Z under the armature, like CesiumMan's Z_UP
		// node); 90 degrees with scale 0.01 is a Mixamo-like file (centimeters, the arm along -Z). In its bind pose (drawn
		// without the skeleton) the arm lies along an axis of the ground.
		float ArmatureRotationDegrees = 0.0f;
		float ArmatureScale = 1.0f;
	};

	// The arm of SkinnedArmOptions{.ArmatureRotationDegrees = -90}: a Z-up file.
	inline SkinnedArmOptions ZUpArmOptions()
	{
		SkinnedArmOptions options;
		options.ArmatureRotationDegrees = -90.0f;
		return options;
	}

	// The arm of SkinnedArmOptions{.ArmatureRotationDegrees = 90, .ArmatureScale = 0.01}: a Mixamo-like file in centimeters.
	inline SkinnedArmOptions MixamoArmOptions()
	{
		SkinnedArmOptions options;
		options.ArmatureRotationDegrees = 90.0f;
		options.ArmatureScale = 0.01f;
		return options;
	}

	inline nlohmann::json CreateSkinnedArmDocument(const SkinnedArmOptions& options = {})
	{
		std::vector<uint8_t> buffer;
		nlohmann::json views = nlohmann::json::array();
		nlohmann::json accessors = nlohmann::json::array();
		auto add = [&](const void* data, size_t size, size_t count, const char* type, int componentType, bool normalized = false) -> size_t
		{
			views.push_back({ { "buffer", 0 }, { "byteOffset", buffer.size() }, { "byteLength", size } });
			const auto* bytes = static_cast<const uint8_t*>(data);
			buffer.insert(buffer.end(), bytes, bytes + size);
			while (buffer.size() % 4 != 0)
				buffer.push_back(0);
			nlohmann::json accessor = { { "bufferView", views.size() - 1 }, { "componentType", componentType }, { "count", count }, { "type", type } };
			if (normalized)
				accessor["normalized"] = true;
			accessors.push_back(accessor);
			return accessors.size() - 1;
		};

		// Rings of a 0.2-wide square tube; each ring's corners twice (one per face), so faces have their own normals.
		std::vector<glm::vec3> positions;
		std::vector<glm::vec3> normals;
		std::vector<uint16_t> joints;   // 4 per vertex (unsigned short)
		std::vector<uint8_t> weights;   // 4 per vertex (normalized unsigned byte)
		std::vector<uint16_t> indices;
		const glm::vec2 corners[4] = { { -0.1f, -0.1f }, { 0.1f, -0.1f }, { 0.1f, 0.1f }, { -0.1f, 0.1f } };
		const int rings = 9;
		for (int ring = 0; ring < rings; ring++)
		{
			const float y = ring * 0.25f;
			for (int face = 0; face < 4; face++)
			{
				const glm::vec2 a = corners[face];
				const glm::vec2 b = corners[(face + 1) % 4];
				const glm::vec2 middle = (a + b) * 0.5f;
				const glm::vec3 normal = glm::normalize(glm::vec3(middle.x, 0.0f, middle.y));
				for (const glm::vec2& corner : { a, b })
				{
					positions.push_back(glm::vec3(corner.x, y, corner.y));
					normals.push_back(normal);
					uint16_t first = 0;
					uint16_t second = 1;
					uint8_t firstWeight = 255;
					if (y < 0.7f)
					{
						first = 0;
					}
					else if (y < 1.3f)
					{
						// 0.75: 3/4 shoulder; 1.0: half; 1.25: 1/4 shoulder.
						first = 0;
						second = 1;
						firstWeight = static_cast<uint8_t>(y < 0.8f ? 191 : (y < 1.1f ? 128 : 64));
					}
					else if (y < 1.9f)
					{
						first = 1;
					}
					else
					{
						first = 2;
					}
					joints.insert(joints.end(), { first, second, 0, 0 });
					weights.insert(weights.end(), { firstWeight, static_cast<uint8_t>(255 - firstWeight), 0, 0 });
				}
			}
		}
		for (int ring = 0; ring + 1 < rings; ring++)
		{
			for (int face = 0; face < 4; face++)
			{
				// Counter-clockwise seen from outside: bottom-left, bottom-right, top-right, top-left.
				const uint16_t bottom = static_cast<uint16_t>(ring * 8 + face * 2);
				const uint16_t top = static_cast<uint16_t>(bottom + 8);
				indices.insert(indices.end(), { bottom, static_cast<uint16_t>(top + 1), static_cast<uint16_t>(bottom + 1), bottom, top, static_cast<uint16_t>(top + 1) });
			}
		}

		// From the arm's space (Y-up, meters) into the armature's: the inverse of the armature node's transform.
		const bool transformed = options.ArmatureRotationDegrees != 0.0f || options.ArmatureScale != 1.0f;
		const glm::quat armatureRotation = glm::angleAxis(glm::radians(options.ArmatureRotationDegrees), glm::vec3(1.0f, 0.0f, 0.0f));
		const glm::mat4 toArmature = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f / options.ArmatureScale)) * glm::mat4_cast(glm::inverse(armatureRotation));
		auto armaturePoint = [&](const glm::vec3& point) { return transformed ? glm::vec3(toArmature * glm::vec4(point, 1.0f)) : point; };
		if (transformed)
		{
			for (glm::vec3& position : positions)
				position = armaturePoint(position);
			for (glm::vec3& normal : normals)
				normal = glm::normalize(glm::inverse(armatureRotation) * normal);
		}

		glm::vec3 minimum(1e9f);
		glm::vec3 maximum(-1e9f);
		for (const glm::vec3& position : positions)
		{
			minimum = glm::min(minimum, position);
			maximum = glm::max(maximum, position);
		}
		const size_t positionAccessor = add(positions.data(), positions.size() * sizeof(glm::vec3), positions.size(), "VEC3", 5126);
		accessors[positionAccessor]["min"] = { minimum.x, minimum.y, minimum.z };
		accessors[positionAccessor]["max"] = { maximum.x, maximum.y, maximum.z };
		const size_t normalAccessor = add(normals.data(), normals.size() * sizeof(glm::vec3), normals.size(), "VEC3", 5126);
		const size_t jointAccessor = add(joints.data(), joints.size() * sizeof(uint16_t), positions.size(), "VEC4", 5123);
		const size_t weightAccessor = add(weights.data(), weights.size(), positions.size(), "VEC4", 5121, true);
		const size_t indexAccessor = add(indices.data(), indices.size() * sizeof(uint16_t), indices.size(), "SCALAR", 5123);

		// Inverse bind matrices: the inverses of the joints' bind transforms (translations along +Y).
		std::vector<float> inverseBind;
		for (float y : { 0.0f, 1.0f, 2.0f })
		{
			const glm::mat4 matrix = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f) - armaturePoint(glm::vec3(0.0f, y, 0.0f)));
			const float* values = &matrix[0][0];
			inverseBind.insert(inverseBind.end(), values, values + 16);
		}

		nlohmann::json skin = { { "name", "ArmSkin" }, { "joints", { 2, 3, 4 } }, { "skeleton", 1 } };
		if (options.InverseBindMatrices)
			skin["inverseBindMatrices"] = add(inverseBind.data(), inverseBind.size() * sizeof(float), 3, "MAT4", 5126);

		const glm::vec3 boneStep = armaturePoint(glm::vec3(0.0f, 1.0f, 0.0f));
		nlohmann::json armature = { { "name", "Armature" }, { "children", { 2 } } };
		if (transformed)
		{
			armature["rotation"] = { armatureRotation.x, armatureRotation.y, armatureRotation.z, armatureRotation.w };
			armature["scale"] = { options.ArmatureScale, options.ArmatureScale, options.ArmatureScale };
		}
		nlohmann::json document = {
			{ "asset", { { "version", "2.0" } } },
			{ "scene", 0 },
			{ "scenes", nlohmann::json::array({ { { "nodes", { 0, 1 } } } }) },
			{ "nodes", nlohmann::json::array({
				{ { "name", "Arm" }, { "mesh", 0 }, { "skin", 0 }, { "translation", { options.ArmNodeOffset, 0.0, 0.0 } } },
				armature,
				{ { "name", "Shoulder" }, { "children", { 3 } } },
				{ { "name", "Elbow" }, { "translation", { boneStep.x, boneStep.y, boneStep.z } }, { "children", { 4 } } },
				{ { "name", "Wrist" }, { "translation", { boneStep.x, boneStep.y, boneStep.z } } } }) },
			{ "meshes", nlohmann::json::array({ { { "name", "Arm" }, { "primitives", nlohmann::json::array({ {
				{ "attributes", { { "POSITION", positionAccessor }, { "NORMAL", normalAccessor }, { "JOINTS_0", jointAccessor }, { "WEIGHTS_0", weightAccessor } } },
				{ "indices", indexAccessor } } }) } } }) },
			{ "skins", nlohmann::json::array({ skin }) }
		};

		if (options.Animations)
		{
			// Wave: 31 keys (30 per second) of the elbow's rotation, 0 -> 90 -> 0 degrees about +Z; reduced on import.
			std::vector<float> waveTimes;
			std::vector<float> waveRotations;
			for (int key = 0; key <= 30; key++)
			{
				const float time = key / 30.0f;
				waveTimes.push_back(time);
				const float angle = glm::radians(90.0f * (1.0f - std::abs(1.0f - 2.0f * time)));
				const glm::quat rotation = glm::angleAxis(angle, glm::vec3(0.0f, 0.0f, 1.0f));
				waveRotations.insert(waveRotations.end(), { rotation.x, rotation.y, rotation.z, rotation.w });
			}
			const size_t waveTimeAccessor = add(waveTimes.data(), waveTimes.size() * sizeof(float), waveTimes.size(), "SCALAR", 5126);
			accessors[waveTimeAccessor]["min"] = { 0.0 };
			accessors[waveTimeAccessor]["max"] = { 1.0 };
			const size_t waveValueAccessor = add(waveRotations.data(), waveRotations.size() * sizeof(float), waveTimes.size(), "VEC4", 5126);

			const std::vector<float> stepTimes = { 0.0f, 0.5f };
			const std::vector<float> stepValues = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
			const size_t stepTimeAccessor = add(stepTimes.data(), stepTimes.size() * sizeof(float), 2, "SCALAR", 5126);
			accessors[stepTimeAccessor]["min"] = { 0.0 };
			accessors[stepTimeAccessor]["max"] = { 0.5 };
			const size_t stepValueAccessor = add(stepValues.data(), stepValues.size() * sizeof(float), 2, "VEC3", 5126);

			const std::vector<float> cubicTimes = { 0.0f, 2.0f };
			const glm::quat end = glm::angleAxis(glm::radians(45.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			const std::vector<float> cubicValues = { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, end.x, end.y, end.z, end.w, 0, 0, 0, 0 };
			const size_t cubicTimeAccessor = add(cubicTimes.data(), cubicTimes.size() * sizeof(float), 2, "SCALAR", 5126);
			accessors[cubicTimeAccessor]["min"] = { 0.0 };
			accessors[cubicTimeAccessor]["max"] = { 2.0 };
			const size_t cubicValueAccessor = add(cubicValues.data(), cubicValues.size() * sizeof(float), 6, "VEC4", 5126);

			document["animations"] = nlohmann::json::array({
				{ { "name", "Wave" },
				  { "samplers", nlohmann::json::array({ { { "input", waveTimeAccessor }, { "output", waveValueAccessor }, { "interpolation", "LINEAR" } } }) },
				  { "channels", nlohmann::json::array({ { { "sampler", 0 }, { "target", { { "node", 3 }, { "path", "rotation" } } } } }) } },
				{ { "name", "Reach" },
				  { "samplers", nlohmann::json::array({
					  { { "input", stepTimeAccessor }, { "output", stepValueAccessor }, { "interpolation", "STEP" } },
					  { { "input", cubicTimeAccessor }, { "output", cubicValueAccessor }, { "interpolation", "CUBICSPLINE" } } }) },
				  { "channels", nlohmann::json::array({
					  { { "sampler", 0 }, { "target", { { "node", 2 }, { "path", "translation" } } } },
					  { { "sampler", 1 }, { "target", { { "node", 4 }, { "path", "rotation" } } } } }) } } });
		}

		document["buffers"] = nlohmann::json::array({ { { "byteLength", buffer.size() }, { "uri", "data:application/octet-stream;base64," + Base64::Encode(buffer) } } });
		document["bufferViews"] = views;
		document["accessors"] = accessors;
		return document;
	}

	inline std::vector<uint8_t> CreateSkinnedArmGltf(const SkinnedArmOptions& options = {})
	{
		const std::string text = JsonUtils::Dump(CreateSkinnedArmDocument(options));
		return std::vector<uint8_t>(text.begin(), text.end());
	}

}
