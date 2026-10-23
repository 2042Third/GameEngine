# Lighting

How a scene is lit and how its brightness reaches the screen: the lights and their units, exposure, the sky and fog.
This guide is for people working in the editor; [DeveloperGuide.md](../DeveloperGuide.md) (Rendering, "Light units and
exposure") explains how the renderer computes it.

## Lights and their units

Strata's lights use the units of real light, on one scale, so values taken from the real world or from a lighting
reference work as they are. Hover a light's **Intensity** in the Inspector to see its unit.

- A **Directional Light** is the sun or the moon: it lights the whole scene from one direction (the entity's forward
  axis). Its **Intensity** is illuminance in **lux**: how much light falls on a surface facing it. It is the only light
  that casts shadows.
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

The renderer stores brightness as it is, up to about 65,000 nits: under a real 100,000 lux sun, white surfaces reach
about 32,000 nits and shiny highlights are cut off. For daylight scenes, use a fraction of the real values (all of them
divided by the same number) with the exposure raised to match: a 100 lux sun at exposure -5 looks like a 100,000 lux sun
at -15.

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

**Show Background** draws the sky behind the scene (otherwise the background is the clear color) and **Background
Blur** softens it.

## Height fog

**Add Height Fog**, in the Sky section, creates fog that is densest at **Base Height** and thins with height
(**Height Falloff**). **Density** says how much it hides (0.02 hides half of what lies 35 m away), **Start Distance**
keeps it away from the camera and **Max Opacity** caps it, so the sky at the horizon shows through. With **Color From
Sky** the fog takes the sky's light, so it meets the horizon without a seam; **Sun Scattering** makes it glow toward the
sun. The first active height fog in a scene applies.

## Shadows

The directional light casts shadows up to **Shadow Distance** from the camera, in cascades that are finer near the
camera. **Cascade Blend** hides the seams between the cascades and fades the shadows out at the distance; **Shadow
Softness** is the light's apparent size (the sun is about 0.5 degrees), which softens shadows further from their casters.
**Shadow Bias** and **Shadow Normal Bias** remove the speckles ("acne") a surface can cast on itself: raise them a little
when lit surfaces show dark stripes, lower them when shadows detach from their casters.
