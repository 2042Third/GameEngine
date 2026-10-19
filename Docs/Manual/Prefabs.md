# Prefabs

A *prefab* is a group of entities saved as an asset (`.stprefab`) that you place in scenes as many times as you like.
Each placed copy is an *instance*: it stays linked to its prefab, so a change of the prefab reaches every instance, while
each instance can still keep values of its own. Imported models (glTF) are placed the same way and stay linked to their
import. This guide is for people working in the editor; [Architecture.md](../Architecture.md) (Editor, "Prefab
instances") explains how the engine keeps instances in step.

## Make a prefab

Select the entities in the Hierarchy and drag them into a folder of the Content Browser, or right-click them and choose
**Create Prefab** (it goes to the project's `Prefabs` folder). The entities become the prefab's first instance (they keep
their place in the scene).

## Place instances

Drag the prefab from the Content Browser into the Scene view: the instance rests on the surface under the cursor. Its
Asset tab's **Add to Scene** (also in the Content Browser's context menu) places one where the view looks. Several
instances of one prefab among the same siblings get numbered names ("Crate", "Crate (1)", ...).

An instance's root has its own place and name: moving, rotating, scaling or renaming the root never counts as a change of
the instance.

## Overrides: values of an instance's own

Change a value of an instance (a light's intensity, a script's field, a component you add or remove) and it becomes an
*override*: that instance keeps it, whatever the prefab says later. The Inspector shows the instance's prefab bar at the
top: the prefab, **Select** (the prefab in the Content Browser), **Open** and the **Overrides** menu, which lists every
override of the instance with **Apply All** and **Revert All**. Overridden values are marked:

- the property's label is bold, with a bar in the accent color at the row's left; right-click it for **Apply to Prefab**
  (the prefab takes this value; every other instance follows) or **Revert** (the instance takes the prefab's value again);
- the header of a component with overrides is marked too, and its menu has **Apply to Prefab** and **Revert to Prefab**
  for the whole component.

Models are not edited in the editor: their instances offer Revert only.

## Change the prefab

**Apply All** (or a property's **Apply to Prefab**) writes the instance into the prefab file. Every other instance of the
prefab in the open scene takes the change at once, except where it overrides it, and the whole change is one undo step:
undo restores the file and the instances. Instances in scenes that are not open follow when their scene opens, unless
they were changed since they last took their prefab's values (the Hierarchy then marks them *outdated*: revert them and
make their changes again; Apply refuses an outdated instance, whose older values would overwrite the prefab's later
changes).

Editing the `.stprefab` file another way (another editor, a version control update) reaches the instances the same way:
the editor notices the file changed.

## Revert and unpack

**Revert All** gives the instance its prefab's values again: entities keep their IDs, entities the prefab lacks go and
entities the instance lacks come back; the root keeps its place and name. **Unpack Prefab** (the Hierarchy's context
menu) ends the link: the entities stay, as ordinary entities.

## For agents

The same through commands: `prefab.create`, `prefab.instantiate` (also many at once: `{prefab, instances: [...]}`),
`prefab.status` (the overrides, whether the instance is modified or outdated), `prefab.apply` and `prefab.revert` (the
whole instance, or one override with `component` and `property`) and `prefab.unpack`. The
[editor automation skill](../../.claude/skills/strata-editor-automation/SKILL.md) has the details.
