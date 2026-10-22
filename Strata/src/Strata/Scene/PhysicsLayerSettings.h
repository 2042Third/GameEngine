#pragma once

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// Number of user collision layers; RigidBodyComponent::Layer and CharacterControllerComponent::Layer range from 0 to
	// c_PhysicsLayerCount - 1.
	constexpr uint32_t c_PhysicsLayerCount = 32;

	// Collision layers: their names and which of them collide. Bodies and character controllers belong to one layer each; two
	// of them collide only if their layers do. Everything collides by default, and layers have no names until the project
	// gives them some. The table is the project's (the settings section Scene.CollisionLayers, ProjectCollisionLayers); each
	// scene works on a copy of it (Scene::GetPhysicsLayers), which the physics system applies when it creates its world and
	// whenever it changes (also while playing: scripts' Physics::SetLayersCollide changes the running scene's).
	struct PhysicsLayerSettings
	{
		static constexpr size_t c_MaxNameLength = 64;
		// A collision mask with every layer's bit.
		static constexpr uint32_t c_AllLayersMask = 0xFFFFFFFFu;

		// Empty for a layer without a name (shown as "Layer <index>").
		std::array<std::string, c_PhysicsLayerCount> Names;
		// Bit j of CollisionMasks[i] is set when layer i collides with layer j; kept symmetric by SetCollide.
		std::array<uint32_t, c_PhysicsLayerCount> CollisionMasks;

		PhysicsLayerSettings()
		{
			CollisionMasks.fill(c_AllLayersMask);
		}

		bool operator==(const PhysicsLayerSettings& other) const = default;

		// Layers outside [0, c_PhysicsLayerCount) are ignored (false).
		void SetCollide(uint32_t layerA, uint32_t layerB, bool collide)
		{
			if (layerA >= c_PhysicsLayerCount || layerB >= c_PhysicsLayerCount)
				return;
			const uint32_t bitA = 1u << layerA;
			const uint32_t bitB = 1u << layerB;
			if (collide)
			{
				CollisionMasks[layerA] |= bitB;
				CollisionMasks[layerB] |= bitA;
			}
			else
			{
				CollisionMasks[layerA] &= ~bitB;
				CollisionMasks[layerB] &= ~bitA;
			}
		}

		// Both masks must allow the pair.
		bool DoCollide(uint32_t layerA, uint32_t layerB) const
		{
			if (layerA >= c_PhysicsLayerCount || layerB >= c_PhysicsLayerCount)
				return false;
			return (CollisionMasks[layerA] & (1u << layerB)) != 0 && (CollisionMasks[layerB] & (1u << layerA)) != 0;
		}

		// Whether a layer collides with every layer (false for layers outside [0, c_PhysicsLayerCount)).
		bool CollidesWithAll(uint32_t layer) const
		{
			return layer < c_PhysicsLayerCount && CollisionMasks[layer] == c_AllLayersMask;
		}

		bool IsDefaultMatrix() const
		{
			for (uint32_t mask : CollisionMasks)
			{
				if (mask != c_AllLayersMask)
					return false;
			}
			return true;
		}

		// Lets every pair of layers collide (the names stay).
		void ResetMatrix()
		{
			CollisionMasks.fill(c_AllLayersMask);
		}

		// No names and every pair colliding: the table of a project (or 0.5.0 scene) that sets no layers.
		bool IsDefault() const;

		// The layer of a name (ignoring case), or of "Layer <index>" for unnamed layers; nothing for unknown names.
		std::optional<uint32_t> FindLayer(std::string_view name) const;
		// The name, or "Layer <index>" without one.
		std::string GetDisplayName(uint32_t layer) const;
		// Why `name` cannot name a layer (control characters, too long, or another layer's name), or nothing when it can. An
		// empty name clears a layer's name.
		std::optional<std::string> ValidateName(uint32_t layer, std::string_view name) const;

		// The stored form, the value of the project section and what 0.5.0 wrote into every scene document:
		// {"Names": {"<index>": "<name>", ...}, "NoCollisions": [[a, b], ...]}, the named layers by index (without leading
		// zeros) and the pairs of layers that do not collide (each once, a <= b, in order), so a table where everything collides
		// stores an empty list.
		nlohmann::json ToJson() const;
		// Reads the stored form. What it holds that is not a name or a pair is skipped and described in outProblems (one
		// message each; a member of the wrong type as a whole), so callers decide whether a problem refuses the value (the
		// project section) or only warns (scene documents). Names are checked in the order of their keys, each against those
		// read before it.
		static PhysicsLayerSettings FromJson(const nlohmann::json& json, std::vector<std::string>* outProblems = nullptr);
	};

}
