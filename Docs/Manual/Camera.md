# The camera's image

How Strata turns the light of a scene into the picture a camera would take: exposure, the tone curve, color, bloom, the
vignette, motion blur, grain and color fringes and color lookups, all set on the scene's **Post Process** component, depth
of field, set on the **Camera**, what each costs, and three recipes.
This guide is for people working in the editor; [DeveloperGuide.md](../DeveloperGuide.md) (Rendering, "Light units and
exposure" and "Post-processing") explains how the renderer computes it. Anti-aliasing has its own page:
[AntiAliasing.md](AntiAliasing.md).

The first active entity with a Post Process component sets the look; a scene without one uses the defaults below, which
are also what a new component gets.

## Exposure

Light in Strata has real units (lux, candela, nits: [Lighting.md](Lighting.md)), so a sunny street is thousands of times
brighter than a lamp-lit room. Exposure scales that light to the display, as a camera's aperture and shutter do.

- **Auto Exposure** (on) meters the image and maps its average brightness to middle gray. **Exposure** adds stops on top
  (+1 shows the scene twice as bright); without Auto Exposure it is the only exposure.
- **Metering:** **Center Weighted** (the default) counts the middle of the image most and its edges a quarter, as a
  camera's center-weighted meter, so a bright sky at the top or a dark corner moves the exposure less. **Average** weighs
  every pixel alike (Strata 0.6.0's). The darkest 10% and the brightest 10% of the pixels are left out (**Auto Exposure
  Low Percentile**, **High Percentile**), so deep shadows and light sources do not move it.
- **Adaptation:** like eyes, the exposure follows a brighter scene faster (**Auto Exposure Light Speed**, 3 per second:
  leaving a tunnel, the glare fades within a second) than a darker one (**Auto Exposure Speed**, 1.5 per second). 0
  jumps at once.
- **The key** (**Auto Exposure Key**, 0.12): a camera that metered every scene to middle gray would show a dim room as
  bright as a sunny street. The key compares what the camera meters with a gray card lit by the scene's directional
  light and keeps part of the difference: a room five stops darker than that card shows 0.6 stops darker (at most 2
  stops). It only darkens: a scene brighter than the card is lit by more than the sun (an overcast sky, dusk, lamps),
  and showing it brighter would make an overcast day lighter than a sunny one. 0 meters every scene to middle gray. It
  needs a directional light, and a night lit by the moon (the directional light) is not darkened by it: set Exposure for
  that, as for a scene that should look bright, such as snow in the sun (+0.5 to +1).
- **Auto Exposure Min EV**, **Max EV** bound the metered average (in log2 nits; EV100 is 3 more). Real sunlight needs
  Max EV 16.

What a scene meters against the directional light's gray card, and what the key at 0.12 does with it:

| Scene | Meters | The key |
| --- | --- | --- |
| Sunny exterior (sun and sky) | near the sun's card | about 0 |
| Overcast exterior (a dim sun, a bright sky) | 2 to 3 stops above the sun's card (the sky lights it) | 0 |
| Dusk (the sun fading out, the sky and lamps lighting the scene) | above the fading sun's card | 0 |
| Interior lit through windows | 5 to 7 stops below the card | 0.6 to 0.85 stops darker |
| Night under the moon (the moon is the directional light) | near the moon's card | about 0 |

The sky light still reaches into closed rooms (walls do not block it yet), so Strata's interiors meter brighter than
real ones and the key darkens them less than the table says. Agents read what the camera metered in the `renderStats`
of `viewport.capture` and `editor.perf`: `exposure.ev100`, `exposure.keyStops` and `exposure.cardEv100` (the card).

## Tone

The tone curve maps the exposed light, which can be thousands of times brighter than white, onto the display.

- **Filmic** (the default) is Strata's camera-like curve: a metered middle gray shows as middle gray (so a new project
  needs no exposure compensation), highlights roll off to white over six and a half stops without clipping, bright
  saturated colors go to white without changing their hue (a red light stays red, skin and foliage keep theirs), and
  shadows keep the scene's own colors: a dark sky or a shaded blue flower stays as saturated as it is, not a deeper
  blue. **Toe** (0.5) sets how deep the shadows go: higher crushes the darkest tones toward black, lower keeps more
  detail in them. **Shoulder** (0.5) sets how softly highlights reach white: 5 stops above middle gray at 0, 6.5 at 0.5,
  8 at 1.
- **ACES** is punchier: darker mid-tones (it shows middle gray about 0.8 stops darker, so scenes made for it carry +0.8
  to +1 Exposure), skies, skin and foliage in shadow about a third more saturated than Filmic's, and bright reds
  skewed toward orange. **AgX** is flatter and less saturated. **Khronos Neutral** keeps base colors exact up to the
  highlights. **Reinhard** and **None** (the exposed color, clipped) are for special cases.
- **Contrast** (1) steepens or flattens the image around middle gray before the curve; **Saturation** (1) scales the
  colors after it.

Scenes keep the curve they were saved with: only new Post Process components and scenes without one get Filmic.

## Color

On the scene's light, before the tone curve:

- **White Balance Temperature** (6500 K, neutral) names the light that should look white: 3200 K makes tungsten-lit
  walls white (and turns everything lit by daylight blue), higher values warm the image. **White Balance Tint** moves
  along green-magenta: positive adds magenta, negative green.
- **Color Slope** (a gain per channel), **Color Offset** (light added per channel, in fractions of middle gray: it tints
  the shadows most) and **Color Power** (a contrast per channel around middle gray) grade the image; (1, 1, 1), (0, 0,
  0) and (1, 1, 1) change nothing.

### Color lookups

A grade made in another tool (a colorist's look, a film emulation) comes as a 3D lookup table in a `.cube` file, the
format grading tools export. Put the file in the project's assets: it imports as a **Color Lookup**, and its import
setting **Size** chooses the points the stored table has each way (17, 33, the default, or 65: more follow a grade's fine
changes more closely and take more memory, 39 KB, 287 KB and 2.2 MB; File keeps the file's own size). Only 3D tables
import; a file that is not one says on which line it went wrong.

On the Post Process, **Color Lookup** names the table and **Color Lookup Intensity** (1) how much of it shows. It applies
to the finished image, after the tone curve and the saturation, as grading tools apply a look to the picture they show:
grade from an image of the scene with Strata's curve (a capture of the viewport), so the table starts from what the
player will see. While the table loads, the image shows without it.

## Bloom

Bright light scatters in a lens: **Bloom** (on) spreads a small share of the light (**Bloom Intensity**, 0.04) into a
soft glow. **Bloom Radius** (0.059) sets how far it reaches, as a fraction of the image height: the halo covers the same
share of the picture at every resolution. **Bloom Threshold** (0) limits the glow to light brighter than it (after
exposure); 0, as in a real lens, lets all light scatter a little.

## Vignette

A lens lets less light reach the corners of the picture than its center (the cos^4 law of the angle from its axis).
**Vignette** (0.12) is the share of that fall-off: 0 none, 1 a real lens's full fall-off. At 0.12 the corners of a 60
degree view darken by about 10%, the middles of its edges much less; wider views darken more. It acts on the light before
the tone curve, as a lens's does. Scenes saved before 0.7.0 keep their number (a stored 0.2 darkens those corners by
16.5%).

## Motion blur

A film camera's shutter stays open for part of each frame, so what moves blurs along its motion. **Motion Blur** (off)
turns it on:

- **Motion Blur Shutter Angle** (180 degrees): the share of a frame the shutter is open, in degrees of 360. 180 blurs
  over half a frame's motion, as film cameras do; 90 halves the blur.
- **Motion Blur Max Length** (0.02): the longest blur, as a fraction of the image height (43 pixels at 2160 rows).
- Blurs shorter than a pixel at 1080 rows (two at 2160) are dropped: slow pans stay sharp, and the center of a turning
  view stays exact while its edges, which move faster, blur a little.
- Only what moves on screen blurs: a still scene seen by a still camera shows exactly as without motion blur, and a
  character the camera follows stays sharp over the blurred background. The first frame and camera cuts never blur.
- Editor views never blur: only the Game view (and a Scene view looking through the game's camera) while the game plays.
  Captures and thumbnails never blur.
- Players can turn it off: the **Motion blur** row of the Graphics Quality settings (Scene or Off, the same under every
  preset; [Settings.md](Settings.md)). The quality's **Motion blur samples** row sets its samples per pixel (Low: off,
  Medium 8, High 12, Ultra 16): more samples show less fine streak noise along fast-moving edges.
- Transparent surfaces blur with what lies behind them; text and the game's UI never blur.

The Basic 3D template and the samples leave it off: motion blur suits fast games seen through a moving camera (racing,
action), where it reads as a camera's; slow, precise and 2D games are better without it.

## Depth of field

A real lens shows sharp only what lies at its focus: nearer and farther things blur into soft disks, more the wider its
aperture. Turn on **Depth Of Field** on the scene's camera (perspective cameras only):

- **Focus Distance** (10 m): meters from the camera to the sharpest plane. Or **Focus Entity**: an entity kept in focus
  (its distance along the camera's view), such as the player or a speaking character.
- **Aperture** (f/2.8): the f-number. Lower blurs more (f/1.4, a portrait lens wide open, isolates a subject), higher keeps
  more in focus (f/8 to f/16 for landscapes).
- **Sensor Height** (24 mm, full frame): with the field of view it sets the focal length; a larger sensor at the same view
  blurs more (36 for medium format), a smaller one less (15.6 APS-C, 8.8 a 1-inch sensor).
- A wide view (60 degrees) focused 10 m away barely blurs the far background at f/2.8 (about 1.4 pixels across at
  2160 rows), as a real wide lens does; near things blur strongly. Long lenses (a narrow field of view) blur much more.
  No blur is wider than 2% of the image height.
- What is in focus stays exactly as without depth of field; a blurred foreground spreads over what lies behind it.
- The Game view, a Scene view through the game's camera and captures through it show it, playing or not; the editor
  camera never does. Players can turn it off: the **Depth of field** row of the Graphics Quality settings (Scene or Off,
  the same under every preset; [Settings.md](Settings.md)).

## Grain and color fringes

Film and lenses have flaws that make a picture feel photographed; use them sparingly (both are off by default, and
players can turn each off with the **Film grain** and **Chromatic aberration** rows of the Graphics Quality settings):

- **Film Grain** (0): fine noise, new every frame, strongest in the mid-tones as film's is and the same on every
  channel (no color speckle). 1 spreads a mid-gray by about 10 codes without changing its brightness, whatever the
  anti-aliasing (it is added after FXAA and the morphological anti-aliasing, which would smooth it); 0.1 to 0.3 reads
  as texture rather than noise.
- **Chromatic Aberration** (0): a lens's lateral color fringes, red and blue drawn apart toward the image's corners; 1
  shifts each half a pixel at the corners of a 3840x2160 image (the same share of any image) and nothing at the center.
- **Lens Flare** (0): what a lens adds around very bright light (the sun, a lamp's bulb, a reflection of the sun): faint
  tinted ghosts mirrored through the image's center, a halo, and six rays around the sun's disk while it is in view and
  nothing covers it. Only light far brighter than white makes them, and even the sun leaves faint ghosts; 0.3 to 0.6
  suits a sunny exterior. Flares come from the bloom's image, which is made for them also with Bloom off.

## What it costs

GPU time at 3840x2160 on an RTX 5090 (the medians of Strata's look-dev captures of the samples and of its perf tests):

| Feature | Cost |
| --- | --- |
| Exposure, bloom, tone curve, color, vignette and dither together (`GPU.Post`) | about 0.18 ms without anti-aliasing (0.15 before 0.7.0) |
| The same with FXAA, or with TAA's sharpening | about 0.21 ms, 0.22 ms |
| Motion blur (`GPU.MotionBlur`, 12 samples), the camera turning 16 pixels a frame | about 0.18 ms |
| Motion blur, the camera still | about 0.11 ms (it still reads every pixel) |
| Depth of field (`GPU.DepthOfField`), everything far blurred at the largest circle | about 0.20 ms |
| Lens flares (in `GPU.Post`), the sun in view | about 0.035 ms more |

Off, motion blur costs nothing (no pass, no target, no motion vectors it would ask for), and so do depth of field and lens
flares (a pass at a quarter of the resolution, plus the bloom's when Bloom is off). Grain, color fringes and a color
lookup are part of the tone mapping: a few more operations per pixel when on.

## Recipes

- **A sunny day:** Filmic, Auto Exposure on, Exposure 0, Center Weighted, Key 0.12, Bloom 0.04, Vignette 0.12, with a
  real sun (about 100,000 lux, or the atmosphere sky's 120,000) and Max EV 16. If the sky fills most of the frame and the
  ground looks dark, raise Exposure by 0.25 to 0.5.
- **An interior:** the same, with the key at 0.12 to 0.2 so the room reads darker than the street outside (as far as its
  light is darker; the key never brightens); a lamp-lit room takes White Balance Temperature 3000 to 4000 to look
  neutral, or stays at 6500 to keep its warmth.
- **A night:** Auto Exposure on with Exposure -1 to -2 (a night should look dark, not like a dim day), White Balance
  Temperature 4000 to 5000 for a cool moonlit cast (the camera takes the light for a warmer one), Toe 0.6, Bloom Intensity 0.06 so lamps glow. Without automatic
  exposure, a scene lit by a full moon takes a manual Exposure around +4.
