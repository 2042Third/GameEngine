#pragma once

#include "StrataScript/Host.h"
#include "StrataScript/Value.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class AudioSource;
	class Component;
	class RigidBody;
	class TransformComponent;

	// Reference to an entity of the scene the script runs in, by UUID. A plain value: copy it freely and store it in
	// fields. Every operation on an entity that no longer exists is a harmless no-op that returns an empty result.
	class Entity
	{
	public:
		constexpr Entity() = default;
		constexpr explicit Entity(uint64_t id)
			: m_ID(id)
		{
		}

		uint64_t GetID() const { return m_ID; }
		// True if the entity exists. Destroyed entities stay valid until the end of the frame.
		bool IsValid() const;
		explicit operator bool() const { return IsValid(); }
		bool operator==(const Entity& other) const = default;

		std::string GetName() const;
		void SetName(std::string_view name);
		// Gameplay label (empty when the entity has none). Setting an empty tag removes it.
		std::string GetTag() const;
		void SetTag(std::string_view tag);

		bool IsActive() const;
		bool IsActiveInHierarchy() const;
		void SetActive(bool active);

		Entity GetParent() const;
		// A null parent makes the entity a root. Returns false if the change would create a cycle.
		bool SetParent(Entity parent, bool keepWorldTransform = true);
		std::vector<Entity> GetChildren() const;

		// Destroys the entity and its descendants at the end of the frame.
		void Destroy();

		// Components by registered name ("Camera", "MeshRenderer", ...), case-insensitive.
		bool HasComponent(std::string_view component) const;
		bool AddComponent(std::string_view component);
		bool RemoveComponent(std::string_view component);
		Component GetComponent(std::string_view component) const;

		// Reflected property of a component. T: bool, int32_t, uint32_t, int64_t, float, glm::vec2/3/4, glm::quat,
		// std::string, Entity or AssetHandle. Enum properties are integers; colors are vec3/vec4.
		template<typename T>
		std::optional<T> GetProperty(std::string_view component, std::string_view property) const;
		template<typename T>
		bool SetProperty(std::string_view component, std::string_view property, const T& value);

		TransformComponent GetTransform() const;
		// The entity's physics body (see Physics.h).
		RigidBody GetRigidBody() const;
		// Playback of the entity's AudioSource component (see Audio.h).
		AudioSource GetAudioSource() const;

		// Script instances on this entity, by class (must be registered with ST_SCRIPT_CLASS) or by class name.
		template<typename T>
		T* GetScript() const;
		template<typename T>
		T* AddScript();
		bool HasScript(std::string_view className) const;
		bool AddScript(std::string_view className);
		bool RemoveScript(std::string_view className);
	private:
		uint64_t m_ID = 0;
	};

	namespace Detail
	{

		template<>
		struct ValueTraits<Entity>
		{
			static constexpr uint32_t Type = StrataScriptValueType_Entity;

			static StrataScriptValue ToValue(const Entity& value)
			{
				StrataScriptValue result = {};
				result.Type = Type;
				result.As.ID = value.GetID();
				return result;
			}

			static bool FromValue(const StrataScriptValue& value, Entity& out)
			{
				if (value.Type != Type)
					return false;
				out = Entity(value.As.ID);
				return true;
			}
		};

	}

	// Generic access to one component of an entity by name; works for every registered component.
	class Component
	{
	public:
		Component(Entity entity, std::string_view name)
			: m_Entity(entity), m_Name(name)
		{
		}

		bool Exists() const { return m_Entity.HasComponent(m_Name); }
		Entity GetEntity() const { return m_Entity; }
		const std::string& GetName() const { return m_Name; }

		template<typename T>
		std::optional<T> Get(std::string_view property) const
		{
			return m_Entity.GetProperty<T>(m_Name, property);
		}

		template<typename T>
		T Get(std::string_view property, const T& fallback) const
		{
			return m_Entity.GetProperty<T>(m_Name, property).value_or(fallback);
		}

		template<typename T>
		bool Set(std::string_view property, const T& value)
		{
			return m_Entity.SetProperty<T>(m_Name, property, value);
		}
	private:
		Entity m_Entity;
		std::string m_Name;
	};

	// Typed access to an entity's Transform (local values are relative to the parent; world values are absolute).
	class TransformComponent
	{
	public:
		explicit TransformComponent(Entity entity)
			: m_Entity(entity)
		{
		}

		Entity GetEntity() const { return m_Entity; }

		glm::vec3 GetTranslation() const { return Detail::FromABIVector3(GetLocal().Translation); }
		glm::quat GetRotation() const { return Detail::FromABIQuat(GetLocal().Rotation); }
		glm::vec3 GetScale() const { return Detail::FromABIVector3(GetLocal().Scale); }
		// Local rotation as Euler angles in radians (pitch, yaw, roll).
		glm::vec3 GetEulerAngles() const { return glm::eulerAngles(GetRotation()); }

		bool SetTranslation(const glm::vec3& translation)
		{
			StrataScriptTransform transform = {};
			Detail::ToABIVector3(translation, transform.Translation);
			return SetLocal(transform, StrataScriptTransformPart_Translation);
		}

		bool SetRotation(const glm::quat& rotation)
		{
			StrataScriptTransform transform = {};
			Detail::ToABIQuat(rotation, transform.Rotation);
			return SetLocal(transform, StrataScriptTransformPart_Rotation);
		}

		bool SetEulerAngles(const glm::vec3& radians) { return SetRotation(glm::quat(radians)); }

		bool SetScale(const glm::vec3& scale)
		{
			StrataScriptTransform transform = {};
			Detail::ToABIVector3(scale, transform.Scale);
			return SetLocal(transform, StrataScriptTransformPart_Scale);
		}

		glm::vec3 GetWorldPosition() const { return Detail::FromABIVector3(GetWorld().Translation); }
		glm::quat GetWorldRotation() const { return Detail::FromABIQuat(GetWorld().Rotation); }
		glm::vec3 GetWorldScale() const { return Detail::FromABIVector3(GetWorld().Scale); }

		bool SetWorldPosition(const glm::vec3& position)
		{
			StrataScriptTransform transform = {};
			Detail::ToABIVector3(position, transform.Translation);
			return SetWorld(transform, StrataScriptTransformPart_Translation);
		}

		bool SetWorldRotation(const glm::quat& rotation)
		{
			StrataScriptTransform transform = {};
			Detail::ToABIQuat(rotation, transform.Rotation);
			return SetWorld(transform, StrataScriptTransformPart_Rotation);
		}

		// World-space directions of the entity's local axes (-Z is forward).
		glm::vec3 GetForward() const { return GetWorldRotation() * glm::vec3(0.0f, 0.0f, -1.0f); }
		glm::vec3 GetRight() const { return GetWorldRotation() * glm::vec3(1.0f, 0.0f, 0.0f); }
		glm::vec3 GetUp() const { return GetWorldRotation() * glm::vec3(0.0f, 1.0f, 0.0f); }

		// Whole local or world transform in one call.
		bool GetLocalTransform(glm::vec3& translation, glm::quat& rotation, glm::vec3& scale) const;
		bool SetLocalTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale);
	private:
		StrataScriptTransform GetLocal() const;
		StrataScriptTransform GetWorld() const;
		bool SetLocal(const StrataScriptTransform& transform, uint32_t parts);
		bool SetWorld(const StrataScriptTransform& transform, uint32_t parts);
	private:
		Entity m_Entity;
	};

	////////////////////////////////////////////////////////////////////////////////
	// Entity
	////////////////////////////////////////////////////////////////////////////////

	inline bool Entity::IsValid() const
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		return m_ID != 0 && host && host->IsEntityValid(Detail::GetContext(), m_ID);
	}

	inline std::string Entity::GetName() const
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		if (!host || m_ID == 0)
			return {};
		return Detail::ReadHostString([&](char* buffer, uint64_t capacity) { return host->GetEntityName(Detail::GetContext(), m_ID, buffer, capacity); });
	}

	inline void Entity::SetName(std::string_view name)
	{
		if (const StrataScriptHostAPI* host = Detail::GetHost(); host && m_ID != 0)
			host->SetEntityName(Detail::GetContext(), m_ID, Detail::ToABIString(name));
	}

	inline std::string Entity::GetTag() const
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		if (!host || m_ID == 0)
			return {};
		return Detail::ReadHostString([&](char* buffer, uint64_t capacity) { return host->GetEntityTag(Detail::GetContext(), m_ID, buffer, capacity); });
	}

	inline void Entity::SetTag(std::string_view tag)
	{
		if (const StrataScriptHostAPI* host = Detail::GetHost(); host && m_ID != 0)
			host->SetEntityTag(Detail::GetContext(), m_ID, Detail::ToABIString(tag));
	}

	inline bool Entity::IsActive() const
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		return host && m_ID != 0 && host->IsEntityActive(Detail::GetContext(), m_ID);
	}

	inline bool Entity::IsActiveInHierarchy() const
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		return host && m_ID != 0 && host->IsEntityActiveInHierarchy(Detail::GetContext(), m_ID);
	}

	inline void Entity::SetActive(bool active)
	{
		if (const StrataScriptHostAPI* host = Detail::GetHost(); host && m_ID != 0)
			host->SetEntityActive(Detail::GetContext(), m_ID, active);
	}

	inline Entity Entity::GetParent() const
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		if (!host || m_ID == 0)
			return {};
		return Entity(host->GetParent(Detail::GetContext(), m_ID));
	}

	inline bool Entity::SetParent(Entity parent, bool keepWorldTransform)
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		return host && m_ID != 0 && host->SetParent(Detail::GetContext(), m_ID, parent.GetID(), keepWorldTransform);
	}

	inline std::vector<Entity> Entity::GetChildren() const
	{
		std::vector<Entity> children;
		const StrataScriptHostAPI* host = Detail::GetHost();
		if (!host || m_ID == 0)
			return children;

		const std::vector<uint64_t> ids = Detail::ReadHostIDs([&](uint64_t* buffer, uint32_t capacity) { return host->GetChildren(Detail::GetContext(), m_ID, buffer, capacity); });
		children.reserve(ids.size());
		for (uint64_t id : ids)
			children.emplace_back(id);
		return children;
	}

	inline void Entity::Destroy()
	{
		if (const StrataScriptHostAPI* host = Detail::GetHost(); host && m_ID != 0)
			host->DestroyEntity(Detail::GetContext(), m_ID);
	}

	inline bool Entity::HasComponent(std::string_view component) const
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		return host && m_ID != 0 && host->HasComponent(Detail::GetContext(), m_ID, Detail::ToABIString(component));
	}

	inline bool Entity::AddComponent(std::string_view component)
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		return host && m_ID != 0 && host->AddComponent(Detail::GetContext(), m_ID, Detail::ToABIString(component));
	}

	inline bool Entity::RemoveComponent(std::string_view component)
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		return host && m_ID != 0 && host->RemoveComponent(Detail::GetContext(), m_ID, Detail::ToABIString(component));
	}

	inline Component Entity::GetComponent(std::string_view component) const
	{
		return Component(*this, component);
	}

	template<typename T>
	std::optional<T> Entity::GetProperty(std::string_view component, std::string_view property) const
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		if (!host || m_ID == 0)
			return std::nullopt;

		StrataScriptValue value = {};
		char stackBuffer[256];
		if (!host->GetProperty(Detail::GetContext(), m_ID, Detail::ToABIString(component), Detail::ToABIString(property), &value, stackBuffer, sizeof(stackBuffer)))
			return std::nullopt;

		std::string largeBuffer;
		if (value.Type == StrataScriptValueType_String && value.As.String.Size > sizeof(stackBuffer))
		{
			// Longer than the stack buffer: read again into a buffer of the reported size.
			largeBuffer.resize(static_cast<size_t>(value.As.String.Size));
			if (!host->GetProperty(Detail::GetContext(), m_ID, Detail::ToABIString(component), Detail::ToABIString(property), &value, largeBuffer.data(), largeBuffer.size()))
				return std::nullopt;
			if (value.As.String.Size > largeBuffer.size())
				value.As.String.Size = largeBuffer.size();
		}

		T result {};
		if (!Detail::ValueTraits<T>::FromValue(value, result))
			return std::nullopt;
		return result;
	}

	template<typename T>
	bool Entity::SetProperty(std::string_view component, std::string_view property, const T& value)
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		if (!host || m_ID == 0)
			return false;
		const StrataScriptValue abiValue = Detail::ValueTraits<T>::ToValue(value);
		return host->SetProperty(Detail::GetContext(), m_ID, Detail::ToABIString(component), Detail::ToABIString(property), &abiValue);
	}

	inline TransformComponent Entity::GetTransform() const
	{
		return TransformComponent(*this);
	}

	inline bool Entity::HasScript(std::string_view className) const
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		return host && m_ID != 0 && host->HasScript(Detail::GetContext(), m_ID, Detail::ToABIString(className));
	}

	inline bool Entity::AddScript(std::string_view className)
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		return host && m_ID != 0 && host->AddScript(Detail::GetContext(), m_ID, Detail::ToABIString(className));
	}

	inline bool Entity::RemoveScript(std::string_view className)
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		return host && m_ID != 0 && host->RemoveScript(Detail::GetContext(), m_ID, Detail::ToABIString(className));
	}

	////////////////////////////////////////////////////////////////////////////////
	// TransformComponent
	////////////////////////////////////////////////////////////////////////////////

	inline StrataScriptTransform TransformComponent::GetLocal() const
	{
		StrataScriptTransform transform = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f } };
		if (const StrataScriptHostAPI* host = Detail::GetHost(); host && m_Entity.GetID() != 0)
			host->GetTransform(Detail::GetContext(), m_Entity.GetID(), &transform);
		return transform;
	}

	inline StrataScriptTransform TransformComponent::GetWorld() const
	{
		StrataScriptTransform transform = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f, 1.0f } };
		if (const StrataScriptHostAPI* host = Detail::GetHost(); host && m_Entity.GetID() != 0)
			host->GetWorldTransform(Detail::GetContext(), m_Entity.GetID(), &transform);
		return transform;
	}

	inline bool TransformComponent::SetLocal(const StrataScriptTransform& transform, uint32_t parts)
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		return host && m_Entity.GetID() != 0 && host->SetTransform(Detail::GetContext(), m_Entity.GetID(), &transform, parts);
	}

	inline bool TransformComponent::SetWorld(const StrataScriptTransform& transform, uint32_t parts)
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		return host && m_Entity.GetID() != 0 && host->SetWorldTransform(Detail::GetContext(), m_Entity.GetID(), &transform, parts);
	}

	inline bool TransformComponent::GetLocalTransform(glm::vec3& translation, glm::quat& rotation, glm::vec3& scale) const
	{
		const StrataScriptHostAPI* host = Detail::GetHost();
		StrataScriptTransform transform = {};
		if (!host || m_Entity.GetID() == 0 || !host->GetTransform(Detail::GetContext(), m_Entity.GetID(), &transform))
			return false;
		translation = Detail::FromABIVector3(transform.Translation);
		rotation = Detail::FromABIQuat(transform.Rotation);
		scale = Detail::FromABIVector3(transform.Scale);
		return true;
	}

	inline bool TransformComponent::SetLocalTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale)
	{
		StrataScriptTransform transform = {};
		Detail::ToABIVector3(translation, transform.Translation);
		Detail::ToABIQuat(rotation, transform.Rotation);
		Detail::ToABIVector3(scale, transform.Scale);
		return SetLocal(transform, StrataScriptTransformPart_All);
	}

}
