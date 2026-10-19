#pragma once

#include <Strata/Core/UUID.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/Property.h>
#include <Strata/Scene/Entity.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	class EditorContext;

	// Sets one property of an entity's component as an undoable edit of the active scene. Consecutive edits of the
	// same property merge into one undo step until the undo stack's merge is broken (call
	// UndoStack::BreakMerge when a drag or text edit ends). Returns false (nothing changed) for invalid values.
	bool SetPropertyWithUndo(EditorContext& context, Entity entity, const ComponentInfo& component, const PropertyInfo& property, const PropertyValue& value,
		std::string* outError = nullptr);

	// Components of a property value: x, y, z, w of vectors and colors, the Euler angles (pitch, yaw, roll) of rotations.
	using PropertyComponentMask = std::array<bool, 4>;

	// The value of a property of the same component on several entities, as the inspector shows a multi-selection.
	struct SharedPropertyValue
	{
		PropertyValue Value;       // The first entity's
		bool Mixed = false;        // Another entity's value differs
		// Per component of vectors and colors, and per Euler angle of rotations (as the inspector shows them): another
		// entity's differs there.
		PropertyComponentMask MixedComponents = {};
	};
	// Nullopt when the entities are empty or one of them lacks the component.
	std::optional<SharedPropertyValue> GetSharedProperty(std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property);
	// Why the property has no effect on any of the entities (the first entity's PropertyInfo::InactiveReason), or "" when it
	// has an effect on one of them: a multi-selection shows a property inactive only when it is inactive on every entity.
	std::string GetSharedInactiveReason(std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property);

	// Sets a property on several entities as one undoable edit (one SceneEditTransaction), merging with the previous edit of
	// the same property of the same entities like the single-entity form. `shown` is what the widget started from: for
	// vectors and colors only the components that differ between it and `value` are written (for rotations: the Euler angles),
	// so editing X of entities whose Y differs keeps each one's Y (and clicking an axis label resets that axis only); when
	// `value` equals `shown`, every entity gets it whole. Fails, changing nothing, when a value is invalid or an entity lacks
	// the component.
	bool SetPropertyWithUndo(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property,
		const PropertyValue& shown, const PropertyValue& value, std::string* outError = nullptr);
	// The same, writing the given components of a vector, color or rotation (the others keep each entity's own): what an
	// edit changed, also when the value shown (the first entity's) already had it there, e.g. an axis label resetting X of
	// entities whose X differs.
	bool SetPropertyComponentsWithUndo(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property,
		const PropertyValue& value, const PropertyComponentMask& components, std::string* outError = nullptr);
	// One edit of a rotation that several entities share, as Euler angles (degrees, as the inspector shows them): the drag of
	// an angle's field, a typed value or an axis label's reset. Each entity's own angles are read once, when the edit begins,
	// and every update replaces the edited angles in them. Reading them from the rotations again on every update would turn
	// the entities the wrong way: reading gives the set of angles with a yaw within [-90, 90] (a rotation has two), so a yaw
	// dragged past 90 degrees would fold back, and a rotation whose yaw is exactly 90 degrees keeps only the difference of
	// its pitch and roll, so a drag through 90 would lose them. Main thread only.
	class RotationAnglesEdit
	{
	public:
		// Starts an edit of the rotation `property` of the entities (of one scene) from `shown`, the angles the widget showed
		// before the edit: the first entity's. The others' are read in the form closest to them, and an entity whose rotation
		// is the first one's (no angle shown as differing) takes them whole. Nullopt (outError says why) when the property is
		// not a rotation, there are no entities, they are not of one scene or one lacks the component.
		static std::optional<RotationAnglesEdit> Begin(std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property,
			const glm::vec3& shown, std::string* outError = nullptr);

		// Whether an update of that property of these entities continues this edit: they are its entities, in its order, and
		// each still has the rotation the edit gave it last. After an undo, a command or another edit of them, a new edit
		// must begin from the angles shown then.
		bool Continues(std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property) const;

		// Sets the angles marked in `components` (pitch, yaw, roll) to those of `angles` on every entity, and the first entity's
		// rotation to `angles` exactly (the widget built its rotation from them), as one undoable edit that merges with the
		// previous updates like SetPropertyWithUndo until the undo stack's merge is broken. Fails, changing nothing, when the
		// edit does not continue (Continues) or a value is invalid.
		bool Update(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property,
			const glm::vec3& angles, const PropertyComponentMask& components, std::string* outError = nullptr);
	private:
		RotationAnglesEdit() = default;
	private:
		std::string m_Component;
		std::string m_Property;
		std::vector<UUID> m_Entities;
		std::vector<glm::vec3> m_Angles;    // Each entity's Euler angles, with the updates so far
		std::vector<glm::quat> m_Rotations; // What each entity's rotation is while the edit continues
	};

	// Whether component.set (and so the inspector's Reset and Paste Values) can set the property: it is neither read-only
	// nor transient.
	bool IsSettableProperty(const PropertyInfo& property);
	// Whether the component has a settable property (a prefab link, all read-only, has none).
	bool HasSettableProperties(const ComponentInfo& component);

	// The property values of a new component of this type, in the form component.set takes: what the inspector's Reset
	// restores.
	nlohmann::json GetDefaultComponentValues(const ComponentInfo& component);
	// The same values by property, in the order of ComponentInfo::Properties (nullopt for read-only and transient ones): what
	// the inspector's axis labels reset a component of a vector to.
	std::vector<std::optional<PropertyValue>> GetDefaultPropertyValues(const ComponentInfo& component);

}
