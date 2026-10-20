# Worlds

A *world* is a level too large to keep loaded whole: an island, a city, a long road. Strata splits it into square
*cells* that load and unload around the player while the game plays, and around the camera while you edit it. This
guide is for people building worlds in the editor; [Architecture.md](../Architecture.md) ("World partition") explains
how the engine streams them, and the [strata-make-a-game skill](../../.claude/skills/strata-make-a-game/SKILL.md) has the
same steps for agents.

## What a world is

- The **persistent scene** is an ordinary scene that never streams out: the player, cameras, lights, the sky, game
  managers and screen-space UI. Its **World** component names the world's **World asset** (`.stworld`) and scales every
  layer's streaming distance (Distance Scale: lower it for small devices).
- **Layers** divide the content by kind (Terrain, Buildings, Props, Gameplay): each has its own cell size and streaming
  distance, so small props can stream in later and closer than terrain. A layer can be turned off while the game plays
  (a script or the editor's visible layers), and its cells unload.
- **Cells** are the squares of a layer's grid. Each cell that has content is a scene file of its own, in a folder beside
  the world: `Worlds/Island/Props/3_-1.stscene` is the Props layer's cell at column 3, row -1.
- **Streaming sources** decide what loads: an entity with a **Streaming Source** component (usually the player) loads
  the cells within its radius on its layers. Without one, the primary camera streams. Higher-priority sources' cells
  load first.

## Create a world

**New > World** in the Content Browser (or **New World** in its background's menu; agents use `world.create`) writes a
World asset and its persistent scene into the open folder and opens it. It starts with four layers of 64 m cells. The
Asset panel shows a world's layers (cell size, streaming distance, cells, whether they load before a game starts);
change them with the `world.setLayers` command: renaming a layer moves its folder, and removing a layer or changing a
cell size that already has cells is refused unless you let it move those cells to the trash.

## Edit a world

While a world is open:

- The **Hierarchy** shows a strip with the world's name, the loaded and unsaved cells and the **edit layer**: new
  top-level entities go into the edit layer's cell under them. The default, **Persistent**, keeps them in the persistent
  scene, which is right for cameras, lights and managers; pick a layer (Props) to place level content. Search
  `l:Props` for the entities of a layer's cells, `l:persistent` for the persistent scene's, `cell:3,-1` for one cell's.
- The **Inspector** names the selected entity's cell ("Cell: Props (3, -1)").
- Moving an entity across a cell border moves it into the neighboring cell of its layer, in the same undo step.
- Cells stream around the editor camera as you fly. The cells you changed stay loaded until you save, and so do the
  cells of the selection, so nothing you edited unloads under you.
- The viewport's **Cells** chip outlines the cells around the camera: loading (yellow), loaded (ochre), unloading (gray).

## The cell map

**View > Streaming**, section World (or the map button of the Hierarchy's strip), draws every cell of the world:
unloaded, wanted, loading, loaded, unloading and failed cells in their colors, unsaved cells outlined, pinned cells with
a pin, and the streaming sources with their radius. Drag to pan, scroll to zoom, double-click to frame the sources.
Hover a cell for its entities and bodies; right-click it to **Load and pin** it (it stays loaded wherever the camera
goes), **Unpin** it or **Show in Content Browser**. While the game plays, the map shows the game's streaming.

## Save and cook

**Save** writes only what changed: the persistent scene when it changed, each cell you edited, new cells (with their
`.meta` files) and the World asset's cell list. Other cells' files are left as they are.

**Cook** (the Asset panel's Cook button for a World asset, or `world.cook`) measures every changed cell: its entities, physics bodies,
bounds and the size of what it loads. It writes them into the World asset's index and sizes the persistent scene's
physics for the busiest place any source can stand, so the physics world starts with room for every body and never
grows while the game plays. Exports cook every
world for you; cook yourself to see the numbers (and the warnings: entity IDs two cells share, cells beyond 20 km,
empty cells).

## Play and export

**Play** starts from what the editor has loaded: the persistent scene and the cells around the camera, with your
unsaved edits. Further cells stream around the game's sources from their saved files. An exported game starts the
persistent scene like any start scene: it waits for the cells of layers marked **Load Before Start** around its sources
(with their collision), shows its loading scene meanwhile, and then streams the rest.

## Scripts

`StrataScript/World.h` lets scripts prepare streaming:

```cpp
// Before teleporting the player far away: ask for the cells there, wait until they are in, then move.
m_Prefetch = World::RequestCellsAround(destination, 10.0f);
// ... in OnUpdate:
if (World::IsLoadedAround(destination, 10.0f))
{
	player.GetRigidBody().Teleport(destination);
	m_Prefetch.Release();
}
```

`World::SetLayerEnabled("Props", false)` turns a layer off (its cells unload) and `World::IsLayerEnabled` reads it.
In a scene that is no world, these calls fail and the Console says so once.

## Limits

- Keep worlds within **20 km** of the origin: positions are 32-bit floats, precise to about 2 mm there. Origin
  rebasing for larger worlds is planned.
- Distant cells are not drawn at all (there are no simplified far versions yet): match fog or the camera's far plane to
  the layers' streaming distances (the Asset panel, or `world.info`, shows them).
- Moving bodies belong to the cell they loaded with and unload with it: objects that roam the whole world (vehicles,
  the player) belong in the persistent scene.
- References between entities of different cells only resolve while both cells are loaded.
- Prefab changes reach a cell's instances when the cell loads in the editor (the cell is then marked changed).
- Changing the cell size of a layer that has cells is refused.
