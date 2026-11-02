# Lighting

How a scene is lit and how its brightness reaches the screen: the lights and their units, exposure, the sky and fog,
shadows, reflection probes and screen-space reflections.
This guide is for people working in the editor; [DeveloperGuide.md](../DeveloperGuide.md) (Rendering, "Light units and
exposure") explains how the renderer computes it.

## Lights and their units

Strata's lights use the units of real light, on one scale, so values taken from the real world or from a lighting
reference work as they are. Hover a light's **Intensity** in the Inspector to see its unit.

- A **Directional Light** is the sun or the moon: it lights the whole scene from one direction (the entity's forward
  axis). Its **Intensity** is illuminance in **lux**: how much light falls on a surface facing it.
- A **Point Light** shines in every direction from its position, a **Spot Light** in a cone. Their **Intensity** is
  luminous intensity in **candela** (cd): how bright the light looks from one direction. A lamp sold as P lumens has
  P / 12.57 cd as a point light (an 800 lumen bulb is about 64 cd). A spot light's candela is the brightness along its
  axis: making its cone narrower does not make it brighter. Both fade with the square of the distance and reach zero at
  **Range**.
- The **Sky Light** lights the scene from every direction and draws the background. What it shows is luminance in
  **nits** (candela per square meter): an environment map's pixel values, or the procedural sky's colors, times
  **Intensity**. A uniform sky of L nits puts about 3.14 x L lux on the ground.
- A material's **Emissive Color** times its **Emissive Intensity** is the luminance it emits, in nits: a monitor is
  about 200, a lit sign about 1,000.

Some real values, with the exposure that suits a scene lit by them (see below):

| Light | Value | Exposure |
| --- | --- | --- |
| Direct sun, mid-morning to noon | 32,000 to 100,000 lux | -13 to -15 |
| Clear blue sky, away from the sun | 3,000 to 8,000 nits | -12 |
| Overcast day | 1,000 to 10,000 lux | -8 to -12 |
| Office lighting | 300 to 500 lux | -7 |
| Full moon | 0.1 to 0.3 lux | +3.5 to +5 |
| Household lamp (800 lumens) | 64 cd | |
| Candle | 1 cd | |

New projects use real values: the Basic 3D template opens under the physical atmosphere with a 120,000 lux sun (the Clear
preset below). New lights and scenes made from nothing still use small values (a 3 lux sun, a procedural sky of a fraction
of a nit, 10 cd for a new point light), which look right at a manual exposure of 0; new scenes and most samples turn on
automatic exposure (see below).
Both work: what matters is that the lights of a scene agree with each other, and that the exposure matches them. A
scene twice as bright at one stop less exposure looks exactly the same.

Real daylight values work too: the renderer scales the brightness it stores by a power of two that follows the exposure
(its *pre-exposure*), so a real 100,000 lux sun, its shiny highlights and even the sun's own disk keep their detail
instead of being cut off at the 65,000 nits half floats can hold. The scaling is exact, so it changes nothing else: a
scene lit by a fraction of the real values (all of them divided by the same number, with the exposure raised to match,
e.g. a 100 lux sun at exposure -5) still looks exactly like the real one at -15. With automatic exposure a sunlit scene
meters around 15 stops, so its **Auto Exposure Max EV** must reach that (16, its default since 0.6; scenes saved with
12 keep it until it is raised). The pre-exposure takes
a few frames to find its value after a scene loads; viewport captures and the project's picture wait for it.

## Exposure

Exposure decides how bright the scene looks, like a camera's. It lives on the **Post Process** component, and
[Camera.md](Camera.md) explains it with the rest of the camera's image (metering, adaptation, the exposure key, the tone
curve, color). In the units of this page:

- **Exposure** (in EV, stops): with **Auto Exposure** off, each step of 1 doubles the brightness on screen. A camera
  setting of EV100 corresponds to an **Exposure** of about 0.5 - EV100 (a sunny day, EV100 15, is about -14.5).
- **Auto Exposure Min EV** and **Max EV** bound the average brightness automatic exposure adapts to, in stops of nits
  (EV100 minus 3): a night scene stays dark when its Min EV is above its brightness. Real sunlight needs **Max EV** 16.

## The sky

Select nothing to see the scene's settings in the Inspector: the **Sky** section shows the sky light that lights the
scene, or offers **Add Procedural Sky** when there is none (the scene is then lit only by its lights, over black).

- **Procedural**: a gradient from **Zenith Color** through **Horizon Color** to **Ground Color**, with the sun's disk
  where the directional light comes from (**Sun Size** degrees across, **Sun Intensity** nits times the light's color).
  The colors are luminances in nits.
- **Environment Map**: an HDR image (an equirectangular `.hdr` texture) around the scene, turned by **Rotation**.
  Without a map, **Ambient Color** lights the scene evenly instead. A map photographed under the sun holds the sun
  too, but light from the sky casts no shadows: press **Extract Sun** in the Sky section to make the scene's directional
  light that sun (it comes from where the sky shows it, in its color, as bright as the map's disk times the sky's
  **Intensity**, or a new "Sun" when the scene has no directional light), and the map's **Remove Sun** import setting is
  turned on, which paints the sun's disk out of the sky with the sky around it so that it does not light the scene a
  second time. A map whose sun is clipped (all its brightest pixels alike) gives a light that is at least that bright,
  and the editor says so. Bake the reflection probes again afterwards.
- **Atmosphere**: Earth's sky, computed from the directional light the way air and haze scatter sunlight: deep blue
  overhead and paler toward the horizon at noon, orange and red around a low sun, dark once it has set. The light's
  **Intensity** is the sun's illuminance above the air (about 120,000 lux for the real sun); the atmosphere dims and
  reddens it on its way down, so the scene's sunlight turns warm at sunset by itself, and the sun's disk (**Sun Size**
  degrees across) carries its light. **Atmosphere Density** thickens the air (1 is Earth's), **Atmosphere Haze** adds
  haze, the whitish glow around the sun, and **Ground Albedo** is how much light the ground sends back up into the sky.
  Use real values with it: a 120,000 lux sun and automatic exposure (**Max EV** 16). A sun that moves keeps the sky and
  its own light up to date at once; the light the sky casts on the scene follows every half degree, computed over a few
  frames and then switched to.
  - **Aerial Perspective** (on by default): the air between the camera and distant surfaces dims them and adds its own
    light, so mountains and far trees fade into the sky's blue, as they do outdoors (and toward orange at sunset).
    **Aerial Perspective Scale** multiplies the distances the air is measured over: 1 is Earth's air, in which a few
    hundred meters barely haze; worlds smaller than real ones show the depth of larger ones with a higher scale (4 makes a
    1 km view look like 4 km; the Open World sample uses 6). Turn it off to keep the look of scenes made before 0.7.0.

**Show Background** draws the sky behind the scene (otherwise the background is the clear color) and **Background
Blur** softens it (the atmosphere's background is always sharp).

## Height fog

**Add Height Fog**, in the Sky section, creates fog that is densest at **Base Height** and thins with height
(**Height Falloff**). **Density** says how much it hides (0.02 hides half of what lies 35 m away), **Start Distance**
keeps it away from the camera and **Max Opacity** caps it, so the sky at the horizon shows through. With **Color From
Sky** the fog takes the sky's light, so it meets the horizon without a seam; **Sun Scattering** makes it glow toward the
sun (with the atmosphere, fog takes the sky's color at the horizon in each direction, brighter toward the sun). The first
active height fog in a scene applies.

## Shadows

The directional light casts shadows up to **Shadow Distance** (meters) from the camera, in cascades that are finer near
the camera. **Cascade Blend** hides the seams between the cascades and fades the shadows out at the distance.

The cascades follow what the camera sees: a few frames after the view changes, they cover only the distances from the
nearest to the farthest surface in view, and each one but the farthest shrinks to the surfaces it shades, so the shadow's
detail goes where the surfaces are (a high camera looking down over a level gets several times finer shadows than
cascades starting at the camera). The farthest cascade always reaches the Shadow Distance, so a surface that just came
into view (the camera turned, a door opened) is shadowed at once, a little coarser for the few frames until the cascades
follow. **Cascade Distribution** (0 to 1, 0.9 by default) decides how the cascades share those distances: 0 evenly, 1 each
cascade the same ratio farther than the previous one; higher puts more of the detail near the camera.

**Shadow Softness** is the light's apparent size in degrees: 0.53 is the sun's (the default; scenes saved with another
value keep it). Shadows are sharp where an object touches the ground and soften with the distance from their caster as a
real sun's do: a 0.53 degree sun blurs an edge from 10% to 90% of its light over about 0.64 cm per metre between the
caster and the ground (a pole's shadow 4 m away has a 2.5 cm soft edge), 2 degrees over about 2.4 cm per metre (a hazy
sun), 0 is a point light's hard edge. The soft edges are the same in every frame, with or without anti-aliasing: nothing
in them flickers or crawls. The filter never misses a caster the shadow map holds, however thin, but the map holds only
what is at least about one of its texels wide: far from the camera, where the cascades' texels are a few centimetres
across, a wire or a blade of grass can lose its shadow, and casters smaller than a texel are left out of those cascades.
Near the camera, Contact Shadow Length grounds such small things when the Graphics Quality's Custom Contact Shadows has
steps (no preset draws them: they can shimmer in motion).

**Shadow Bias** and **Shadow Normal Bias** remove the speckles ("acne") a surface can cast on itself: raise them a little
when lit surfaces show dark stripes, lower them when shadows detach from their casters.

**Contact Shadow Length** (meters, 0.3 by default, at most 2) grounds what is smaller than the cascades can show, such as a
foot on the ground or a thin plate on a table: with Custom Contact Shadows the renderer looks this far toward
the sun over what the camera sees and shades where something is in the way. 0 turns them off for the light. Like every
effect worked out from the image, they come from every opaque surface on screen, also from meshes whose **Cast Shadows**
is off, and from nothing off screen or hidden behind something; they fade out toward the shadow distance and the image's
edges, and transparent surfaces neither cast nor receive them.

The project's **Graphics Quality** (Project Settings, [Graphics.md](Graphics.md)) decides what the sun's shadows cost:

| Setting | Low | Medium | High | Ultra |
| --- | --- | --- | --- | --- |
| Shadow resolution (texels each way of every cascade) | 1024 | 2048 | 2048, 4096 from 1440 output rows | 4096, 8192 from 1440 rows |
| Shadow cascades | 3 | 4 | 4 | 4 |
| Shadow Filter Quality (taps of the wide soft edges) | 8 | 16 | 24 | 32 |
| Contact Shadows (steps; off in every preset) | off | off | off | off |

More filter taps only matter where soft edges are wider than a shadow texel (a large Shadow Softness, tall casters): they
cost a little more there and leave less grain for the screen-space blur to remove. Contact shadows, when Custom turns them
on, cost a march of their
steps for each lit pixel nearer than the shadow distance (about 0.2-0.3 ms at 3840x2160 on a current GPU).

To see where the cascades are and how fine they are, choose **Shadows** in the Scene view's debug menu: every surface is
colored by how many screen pixels one shadow texel covers there (green about one, yellow two, red four or more), with the
cascades outlined and their blend bands hatched. Agents read the same as numbers: `viewport.capture {"debugView": "shadows"}`
reports each cascade's distances, texel size and pixels per texel (a capture waits the few frames the cascades take to
follow the view: `shadowFitSettled`).

Point and spot lights cast shadows when **Cast Shadows** is on (it is off by default). A spot light's shadow covers its
cone, a point light's every direction (the renderer draws it as the six faces of a cube). **Shadow Filter Radius** softens
the shadow's edge (0 is sharp, 8 the softest), and **Shadow Bias** and **Shadow Normal Bias** work as for the sun; all
three are measured in texels of the light's shadow map, whose detail follows how large the light's range looks on
screen.

They cost more than the sun's shadows, so use them where they show:

- At most 16 point and spot lights in view cast shadows at a time, the most relevant ones (those around the camera, then
  those whose range looks largest); a light that drops out of the 16 fades its shadow out rather than losing it at once.
- A light's shadow is drawn again only when something in its range moves or changes, or the light itself does: a still
  lantern over a still room costs nothing after its first frame, while a light carried around, or one over moving
  characters, redraws every frame. A point light costs six times what a spot light costs. A few lights redraw per frame;
  the others keep their previous shadow meanwhile, and a light just switched on fades its shadow in over a few frames.
- Keep the lamp's own model out of its light's shadow: turn **Cast Shadows** off on the lamp's mesh (Mesh Renderer), or
  place the light outside it, or the model darkens everything around.

## Reflection probes

Shiny surfaces reflect the sky everywhere unless something tells them what is around them: in a room, a courtyard or
a cave, add a **Reflection Probe** (Add Component, Rendering) where a viewer would stand. Its **Capture** is a picture of
the scene all around the probe (an HDR texture); inside its box (**Extents**, the half size along the entity's axes),
surfaces reflect that picture and take their ambient light from it instead of from the sky.

**Bake**, in the probe's section of the Inspector (or **Bake Reflection Probes** in the Sky section, with nothing
selected, for all of them; agents use `lighting.bakeProbes`), takes the picture: the editor renders what the probe sees, saves it as
`Assets/Lighting/<scene>/<probe>.hdr` and sets **Capture** to it, in one step you can undo. Bake again after you change
the lighting or move things: the new picture replaces the probe's earlier one (which goes to the project's trash). Probes
see each other's pictures, so a second bake adds light that bounced twice. Pictures are not taken while the game runs:
a probe shows the scene as it was baked (a day-night cycle or a moving door is not reflected).

- **Box Projection** (on by default) makes reflections land where the walls are, as in a room; turn it off for open
  spaces, where what the probe saw is far away.
- **Blend Distance** fades the probe out over that many meters before its box's faces, into the sky's lighting or a
  weaker probe's. Where boxes overlap, the higher **Priority** wins, then the smaller box; two probes blend at most.
- **Intensity** brightens or darkens what the probe shows.
- At most 16 probes in view light a scene, the strongest ones. The renderer prepares a probe's picture once, when the
  probe first comes into view (its reflections appear a few frames later), and keeps it (about 1 MB of video memory
  each); afterwards a probe costs only a little shading where its box is on screen.

## Screen-space reflections

Probes and the sky reflect a fixed picture. **Screen Space Reflections** (in the **Post Process**) adds what the screen
shows: a polished floor, a pond or a metal plate reflects the characters and things standing on or beside it, and the
reflections move with them. Each frame the renderer follows the reflected rays across the image and takes the color the
previous frame showed where a ray meets a surface. How depends on the project's Graphics Quality (its **Reflections**
row): at **High** and **Ultra** every pixel traces at full resolution and neighboring pixels share what they found, with or
without TAA, so reflections are sharp and need no history; at **Medium** (Strata 0.6.0's) they trace at half resolution
and need TAA to smooth them; at **Low** there are none.

- Only what is on screen can be reflected. Things behind the camera, beside the view or hidden behind others are not:
  there the probes and the sky reflect as before. Reflections fade toward the screen's edges.
- **Screen Space Reflections Max Roughness** (0.5): rougher surfaces get none, and they fade out over the last quarter
  below it. These reflections are sharp, so keep it low; matte materials (roughness near 1, as most low-poly kits use)
  show none at all.
- **Screen Space Reflections Intensity** (1): how much they replace the probes' and the sky's reflection.
- Transparent surfaces do not get them.
- At Medium they need TAA (Anti Aliasing Method TAA): with FXAA or without anti-aliasing they are not drawn. Cost there:
  about half a millisecond of GPU time at 1920x1080 on an Apple M2 Max with every surface reflecting, and about 20 MB of
  video memory per view. At High and Ultra they work with any anti-aliasing and take about 230 MB of video memory per
  view at 3840x2160 (about 60 MB at 1920x1080).
- The first frame of a view has nothing to reflect from yet: reflections appear from the second frame on (captures wait
  for it).

## Materials that keep their energy

A rough metal scatters the light it receives between the tiny facets of its surface before it leaves; Strata counts that
light, so a white metal reflects all the light around it however rough it is, and rough metals look as bright as their
color says (since 0.7.0; before, a metal of roughness 1 lost about two thirds of its light and looked dull gray). Colored
metals such as copper or gold keep less at high roughness, as real ones do: each bounce between the facets takes some of
the light. Materials made darker to look right before may now be set to their measured colors.

Highlights hold at 4K without sparkling: where a surface's normal map or curvature changes faster than the pixels can
show, its highlight spreads over the pixels it covers instead of flickering between single bright pixels (specular
anti-aliasing, since 0.7.0). Distant normal-mapped and curved surfaces therefore look slightly rougher than their
material says; flat surfaces are unchanged. Reflections that a normal map would turn into the surface itself fade out
instead of showing what lies below the surface. At the High and Ultra graphics quality the sky's reflections are filtered
at 512 texels per face (256 at Low and Medium), so mirror-like surfaces reflect a sharp sky at 4K.

## Sun and sky presets

The Sky section of the Inspector has four presets, each a sky and a sun of real values in one undo step (agents run
`lighting.applySkyPreset`): **Clear** (the physical atmosphere and a 120,000 lux sun 40 degrees up: sunny, deep blue
shadows), **Hazy** (thicker air: a paler sky and softer contrast), **Overcast** (an even gray sky of about 7,700 lux and a
faint, very soft sun) and **Golden Hour** (the sun 6 degrees up: warm light and long shadows). They keep the sun's direction
around the horizon and make the sky and the sun a scene lacks. A real sun would turn the image white under a manual
exposure (an **Exposure** of 0 suits a 3 lux sun), so a preset turns the Post Process's **Auto Exposure** on (its
**Exposure** compensation back to 0) and raises its **Auto Exposure Max EV** to 16 when lower; undo brings the old
exposure back.

How sunny a scene looks depends most on its **sun to sky ratio**: the light flat ground gets from the sun over what it gets
from the sky (the lighting debug views' legend names it).

| Light | Sun : sky on flat ground |
| --- | --- |
| Clear noon | 6 to 10 |
| Hazy day | 3 to 5 |
| Golden hour | about 1 to 2 |
| Overcast | below 0.3 |

Below 3 a sunny scene looks flat and gray; far above 10 its shadows turn black. New procedural skies are balanced for the
default 3 lux sun (about 8).

Models from kits whose exporter writes sRGB colors into glTF's linear base color factors (Kenney's, for one) look washed
out: lighter and paler than their artist chose. Turn on **Base Color Factors Are sRGB** in their import settings.

## Ambient occlusion

The **Post Process**'s **Ambient Occlusion** darkens the light from the sky and the surroundings in creases and corners and
under objects, where less of the sky is seen, so things sit on the ground. It is computed from the image each frame, clean
without TAA.

- **Ambient Occlusion Radius** (1 m): how far around a surface occluders darken it. On screen it stays between 0.3% and 12%
  of the view's height: far away objects keep a thin contact line, close-ups do not turn into dark halos.
- **Ambient Occlusion Intensity** (1): how strongly; above 1 darker.
- **Ambient Occlusion Multi Bounce** (on): bright surfaces reflect light into their own creases, so white plaster stays
  light in its corners while dark soil darkens fully. It also applies to materials' occlusion maps.
- Its cost follows the **Graphics Quality**'s ambient occlusion quality (Low, Medium, High, Ultra: more samples per pixel).
  The occlusion only sees what is on screen: things behind others or outside the view do not occlude.

## Debug views

The **debug view** chip on the viewport's toolbar has five views that show the scene's light instead of its image, each
with a legend:

- **Lighting**: every surface white, lit as the scene lights it. Shadows, the sky's color and how bright the sun is
  against the sky show without the materials' colors in the way.
- **AO**: the ambient occlusion that darkens the indirect light, white where nothing occludes.
- **Indirect**: only the light from the sky, reflection probes and reflections, without the sun and the other lights.
- **Luminance**: how bright each part of the image really is, in bands one exposure value (EV100) wide: each band twice
  as bright as the one below it. An interior lit by a window is about EV 7 to 9, a sunlit street about 14 to 15, the
  sunlit sky above 13.
- **Albedo**: the materials' base colors. Real materials lie between about 0.03 (charcoal, fresh asphalt) and 0.85 (fresh
  snow): darker ones are marked blue, brighter ones striped red, with their share of the image in the legend. Colors
  outside that range make lighting look wrong however it is set up.

Every view's legend names the **sun to sky ratio** on flat ground: how much more light a horizontal surface in the sun gets
than one in the shade. A clear day gives 6 to 10, a hazy one 3 to 5, golden hour about 1 to 2, an overcast day below 0.3.
