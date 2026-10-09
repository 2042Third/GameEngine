---
name: strata-add-component
description: Add a new ECS component (data plus optional runtime behaviour) to the Strata engine so that it is serialized, editable in the inspector, scriptable, reachable through the editor automation commands and covered by tests. Use when adding or changing a component type.
---

# Adding a component

Components are plain structs. Reflection metadata drives everything else: scene and prefab files, the
editor inspector, the editor commands used by automation (`component.add/set/get`), undo/redo and generic
script access. A component that is registered correctly needs no inspector, serializer or command code
of its own.

## 1. Declare the data

Add the struct to `Strata/src/Strata/Scene/Components.h` in the right section. Rules:

- Only reflectable member types: `bool`, `int32_t`, `uint32_t`, `float`, `glm::vec2/3/4`, `glm::quat`,
  `std::string`, enums (registered with `EnumProperty`), asset handles (`AssetHandle`, with
  `AssetProperty`) and entity references (`UUID`, with `EntityProperty`).
- Give every member a sensible default and a comment with units ("World units", "Degrees", "per second").
- Never store engine pointers or runtime handles in a serialized member; keep runtime state in the system
  that owns it (keyed by entity), or mark it `Transient`.

## 2. Register it

In `Strata/src/Strata/Scene/ComponentRegistration.cpp`, register the type with a stable name. The name is
written to files and used by tools; never rename it later.

```cpp
ComponentRegistry::Register<WindZoneComponent>("WindZone")
	.DisplayName("Wind Zone")
	.Category("Physics")
	.Description("Pushes rigid bodies inside its radius")
	.Property("Strength", &WindZoneComponent::Strength, WithTooltip(Range(0.0f, 1000.0f, 0.1f), "Newtons"))
	.Property("Radius", &WindZoneComponent::Radius, Range(0.01f, c_Unbounded, 0.05f))
	.EnumProperty("Mode", &WindZoneComponent::Mode, { { "Directional", 0 }, { "Spherical", 1 } })
	.AssetProperty("Sound", &WindZoneComponent::Sound, AssetType::AudioClip);
```

- Ranges are enforced everywhere (inspector, files, commands, scripts): choose them so invalid states are
  impossible (no negative radii, no zero divisors).
- `Color()` marks vec3/vec4 members as colors; `Slider(min, max)` uses a slider; `WithTooltip` documents
  the property for people and AI agents (the tooltip text is part of `component.list`).
- Flags: `NotRemovable` for components every entity has, `Hidden` for internal ones, `NoSerialize` only
  for data the serializer writes itself.

## 3. Add behaviour (if the component does something at runtime)

Implement a `SceneSystem` (`Scene/SceneSystem.h`) and add its descriptor in `CreateBuiltinSceneSystems`
(`Scene/SceneSystemRegistration.cpp`). The registration order is the update order; set
`RunsInSimulateMode` only for systems that belong in the editor's physics-only simulate mode. React to
edits through EnTT signals (`on_construct`, `on_update`, `on_destroy`). Edits notify through `patch`, which
`ComponentAccess` and `Entity::MarkModified<T>()` emit, so the system must not poll every component each
frame.

## 4. Test it

- `StrataTests/src/Scene/ReflectionTests.cpp`: the properties exist with their ranges, and invalid values
  are rejected.
- `StrataTests/src/Scene/SerializationTests.cpp`: a scene round trip keeps every value; unknown or
  out-of-range values in files load with warnings instead of failing.
- The system's behaviour, with a real `Scene` stepping `OnUpdateRuntime`.
- Editor: `component.add` with values and `component.set` through `EditorCommandRegistry` (see
  `StrataTests/src/Editor/EditorCommandTests.cpp`); undo restores the previous state.
- Rendering features need GPU tests that check pixels (`StrataTests/src/Renderer/GPUSceneRendererTests.cpp`).
- Extend the feature test project so the component is exercised in a real scene.

## 5. Document

Mention the component where the module is described in `AGENTS.md` if it changes conventions. Its
reflection description, categories and tooltips are the user-facing documentation.
