# Lighting

How a scene is lit and how its brightness reaches the screen: the lights and their units, exposure, the sky and fog,
shadows and reflection probes.
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

New lights, new scenes and the samples use small values instead (a 3 lux sun, a sky under 1 nit, 10 cd for a new point
light), which look right at a manual exposure of 0; new scenes and most samples turn on automatic exposure (see below).
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

Exposure decides how bright the scene looks, like a camera's. It lives on the **Post Process** component:

- **Exposure** (in EV, stops): with **Auto Exposure** off, each step of 1 doubles the brightness on screen. A camera
  setting of EV100 corresponds to an **Exposure** of about 0.5 - EV100 (a sunny day, EV100 15, is about -14.5).
- **Auto Exposure** measures the scene's average brightness each frame and brings it to middle gray, like a camera's
  automatic mode; **Exposure** then brightens or darkens that result. **Auto Exposure Min EV** and **Max EV** bound the
  average brightness it adapts to, in stops of nits (EV100 minus 3): a night scene stays dark when its Min EV is above
  its brightness, and **Auto Exposure Speed** sets how fast it adapts (0 at once).

A night scene lit by a full moon, for example, uses manual exposure around +4, or automatic exposure with Min EV high
enough that the night does not look like day.

## The sky

Select nothing to see the scene's settings in the Inspector: the **Sky** section shows the sky light that lights the
scene, or offers **Add Procedural Sky** when there is none (the scene is then lit only by its lights, over black).

- **Procedural**: a gradient from **Zenith Color** through **Horizon Color** to **Ground Color**, with the sun's disk
  where the directional light comes from (**Sun Size** degrees across, **Sun Intensity** nits times the light's color).
  The colors are luminances in nits.
- **Environment Map**: an HDR image (an equirectangular `.hdr` texture) around the scene, turned by **Rotation**.
  Without a map, **Ambient Color** lights the scene evenly instead.
- **Atmosphere**: Earth's sky, computed from the directional light the way air and haze scatter sunlight: deep blue
  overhead and paler toward the horizon at noon, orange and red around a low sun, dark once it has set. The light's
  **Intensity** is the sun's illuminance above the air (about 120,000 lux for the real sun); the atmosphere dims and
  reddens it on its way down, so the scene's sunlight turns warm at sunset by itself, and the sun's disk (**Sun Size**
  degrees across) carries its light. **Atmosphere Density** thickens the air (1 is Earth's), **Atmosphere Haze** adds
  haze, the whitish glow around the sun, and **Ground Albedo** is how much light the ground sends back up into the sky.
  Use real values with it: a 120,000 lux sun and automatic exposure (**Max EV** 16). A sun that moves keeps the sky and
  its own light up to date at once; the light the sky casts on the scene follows every half degree, computed over a few
  frames and then switched to.

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

The directional light casts shadows up to **Shadow Distance** from the camera, in cascades that are finer near the
camera. **Cascade Blend** hides the seams between the cascades and fades the shadows out at the distance; **Shadow
Softness** is the light's apparent size (the sun is about 0.5 degrees), which softens shadows further from their casters.
**Shadow Bias** and **Shadow Normal Bias** remove the speckles ("acne") a surface can cast on itself: raise them a little
when lit surfaces show dark stripes, lower them when shadows detach from their casters.

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

- **Box Projection** (on by default) makes reflections land where the walls are, as in a room; turn it off for open
  spaces, where what the probe saw is far away.
- **Blend Distance** fades the probe out over that many meters before its box's faces, into the sky's lighting or a
  weaker probe's. Where boxes overlap, the higher **Priority** wins, then the smaller box; two probes blend at most.
- **Intensity** brightens or darkens what the probe shows.
- At most 16 probes in view light a scene, the strongest ones. The renderer prepares a probe's picture once, when the
  probe first comes into view (its reflections appear a few frames later), and keeps it (about 1 MB of video memory
  each); afterwards a probe costs only a little shading where its box is on screen.
