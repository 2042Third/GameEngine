#pragma once

#include <Strata/Reflection/Property.h>

#include <array>
#include <string>
#include <string_view>

namespace Strata
{

	class Scene;

	// Drag-and-drop payload types shared by the panels.
	namespace DragDrop
	{
		constexpr const char* c_Entities = "STRATA_ENTITIES"; // UUIDs of entities of the active scene (one or more, in hierarchy order)
		constexpr const char* c_Asset = "STRATA_ASSET";       // AssetHandle
	}

	struct PropertyEditResult
	{
		bool Changed = false;  // The value was modified this frame
		bool Finished = false; // An interactive edit (drag, text input) ended this frame, or a discrete one (a click) was made
		// Vectors and rotations: the components (Euler angles) the edit changed, also when they were already at the new value
		// for the value shown (an axis label resetting a mixed axis).
		std::array<bool, 4> ChangedComponents = {};
		// Rotations: the Euler angles (degrees) the widget showed before the edit and shows after it. They are what was
		// edited: reading them back from the rotation could give the other set of angles of the same rotation (a yaw past 90
		// degrees folds back), or another split of pitch and roll (at a yaw of exactly 90 degrees).
		glm::vec3 EulerAnglesBefore = glm::vec3(0.0f);
		glm::vec3 EulerAngles = glm::vec3(0.0f);
	};

	// How DrawPropertyWidget shows a value that several entities share (the inspector's multi-selection) and what its
	// axis labels do.
	struct PropertyWidgetOptions
	{
		// The value differs between the entities: shown as mixed ("--") until it is edited. For vectors, colors and
		// rotations, MixedComponents marks the components (x, y, z, w; Euler angles of rotations) that differ.
		bool Mixed = false;
		std::array<bool, 4> MixedComponents = {};
		// The value the axis labels of vectors and rotations reset their component to (clicking "X" sets x to the default's
		// x). Null: the labels only show the axes.
		const PropertyValue* DefaultValue = nullptr;
		// The ItemProbe key prefix of the widget (e.g. "Inspector.Transform.Translation"): the field is recorded under it and
		// the axis labels under "<prefix>.X" and so on, so tests can find them. Empty: nothing is recorded.
		std::string_view ProbeKey;
		// Unsigned whole numbers: what the field says while the value is 0, where 0 stands for a choice rather than none
		// ("Auto" for capacities the engine sizes). Dragging or typing a number sets one as usual.
		std::string_view ZeroLabel;
	};

	// Draws an editor widget for a reflected property and edits `value` in place, filling the width it is given:
	// - vectors and rotations (as Euler angles in degrees) as one field per component behind an axis label in the theme's
	//   axis colors (X Cinnabar, Y Malachite, Z Azurite); clicking a label resets that component (PropertyWidgetOptions);
	// - colors as fields of their channels behind R, G, B, A labels in the same colors, with a swatch after them (the
	//   swatch alone across the field where the fields would be too narrow to read); the swatch opens a picker with the
	//   channels under it. Mixed colors show "--" in each channel that differs and on the swatch;
	// - numbers as people write them ("0.8", "-4": up to three decimals, without a sign on zero); a value or text that does
	//   not fit ends in an ellipsis and shows whole in a tooltip; mixed multi-line text shows "--" while it is empty;
	// - entity references as a button that opens a searchable list of the scene's entities (and accepts entities dragged
	//   from the hierarchy), asset references as one that opens a searchable, virtualized list of the assets of the
	//   property's type (and accepts dragged assets).
	PropertyEditResult DrawPropertyWidget(const PropertyInfo& property, PropertyValue& value, Scene& scene, const PropertyWidgetOptions& options = {});

	namespace UI
	{

		// A two-column table of properties (labels left, widgets right) as the inspectors show them. Rows come from
		// PropertyLabel; call EndPropertyTable only when BeginPropertyTable returned true. All property tables of a window
		// share one label width, which the user changes by dragging the divider between the columns.
		bool BeginPropertyTable(const char* id);
		void EndPropertyTable();
		// How a property's label shows what is special about its value.
		struct PropertyLabelOptions
		{
			// A prefab instance's own value (an override of its prefab): the label in SemiBold with an accent bar at the row's
			// left edge.
			bool Overridden = false;
			// Why the property has no effect now (empty: it has): the label in the disabled color, the reason in its tooltip.
			std::string_view InactiveReason;
		};
		// Starts a row: the label (with its tooltip) in the first column, then moves to the second for the widget. A label
		// wider than its column ends in an ellipsis and its tooltip names it whole. With a probe key, the probe records the
		// label and the text it shows under "<probeKey>.Label" (its color: the accent while overridden; disabled while
		// inactive, the reason then under "<probeKey>.Inactive").
		void PropertyLabel(std::string_view label, std::string_view tooltip = {}, std::string_view probeKey = {}, const PropertyLabelOptions& options = {});

		// The type icon of an asset type (UI/Icons.h), as lists and pickers show assets.
		const char* GetAssetTypeIcon(AssetType type);
		// The name of an asset type for people ("Audio Clip"), where AssetTypeToString gives the identifier ("AudioClip").
		const char* GetAssetTypeDisplayName(AssetType type);

	}

}
