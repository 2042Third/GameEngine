#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	// Number of user collision layers; RigidBodyComponent::Layer and CharacterControllerComponent::Layer range from 0 to
	// c_PhysicsLayerCount - 1.
	constexpr uint32_t c_PhysicsLayerCount = 32;

	// A scene's physics layers (SceneSettings::PhysicsLayers): their names and which of them collide. Bodies and character
	// controllers belong to one layer each; two of them collide only if their layers do. Everything collides by default,
	// and layers have no names until a scene gives them some. The physics system applies the matrix when it creates its
	// world and whenever it changes (also while playing: scripts' Physics::SetLayersCollide changes the running scene's).
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

		// The layer of a name (ignoring case), or of "Layer <index>" for unnamed layers; nothing for unknown names.
		std::optional<uint32_t> FindLayer(std::string_view name) const;
		// The name, or "Layer <index>" without one.
		std::string GetDisplayName(uint32_t layer) const;
		// Why `name` cannot name a layer (control characters, too long, or another layer's name), or nothing when it can. An
		// empty name clears a layer's name.
		std::optional<std::string> ValidateName(uint32_t layer, std::string_view name) const;
	};

}
