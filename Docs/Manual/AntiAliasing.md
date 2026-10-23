# Anti-aliasing

How Strata smooths jagged edges: FXAA and TAA, how many frames TAA keeps and when it trails, its presets and every
tunable, and its limits.
This guide is for people working in the editor; [DeveloperGuide.md](../DeveloperGuide.md) (Rendering, "Motion vectors
and TAA") explains how the renderer computes it.

## FXAA or TAA

Anti-aliasing is set on the scene's **Post Process** component: **Anti Aliasing** turns it on, **Anti Aliasing
Method** chooses how.

- **FXAA** smooths each frame on its own, by blurring along the edges it finds in the image. It costs little and never
  trails, but edges still crawl a little as things move, thin details (grass, rails, distant branches) flicker, and the
  noise of soft shadows and ambient occlusion stays. It is the component's default, and the right choice for 2D games
  and pixel art, whose crisp pixels TAA would soften.
- **TAA** (temporal anti-aliasing) moves every frame's samples by a different fraction of a pixel (eight positions in
  turn) and blends each frame with the image of the frames before it, fetched where each surface was in the previous
  frame. Edges come out smooth and steady, thin details stop flickering, and the noise of soft shadows and ambient
  occlusion averages away. New projects made from the Basic 3D template and the 3D samples use it.

## How many frames TAA keeps

After the image starts again (the first frame of a view, a camera cut, a resize, a view that was hidden), frames 1 to
8 are averaged evenly: at the eighth frame every sample position counts once. From then on each new frame weighs 1/8
and the older frames fade: the newest 6 frames hold more than half of the image, the newest 18 more than 90%. Captures
and the project's picture are taken at the eighth frame of a still view.

Motion shortens this: a surface that moves weighs its current frame more (see **Motion Response**), and the Sharp
preset's terms below raise the current frame's weight in slow pans, under transparent surfaces and where a surface was
just uncovered.

## Trails and how they go away

Every frame, the history of a pixel is kept consistent with what the pixel shows now: it may not leave the colors of
the pixel and its eight neighbors in this frame. When something moves, its old color is outside the colors around the
pixel it left, so its trail disappears at once. A trail stays where the neighborhood happens to contain the old color
too: a dark object passing over a background with bright spots (sunlit leaves, glints) keeps a faint copy of itself
beside the spots for a few frames.

- **Tight** clipping (the Sharp preset) keeps the history in a narrower box around the average color of the
  neighborhood, which removes most of those trails. It gains most where the surroundings are smooth, and less beside
  glints, whose brightness widens the box.
- **Disocclusion rejection** (Sharp) drops the history where a surface was just uncovered, so a moving object leaves
  no ghost on the wall behind it.
- **Transparent Weight** (Sharp) helps transparent surfaces, which have no motion of their own: without it a moving
  glass pane or particle drags its image behind it.

## Presets

**TAA Preset** chooses the look:

| Preset | Look |
| --- | --- |
| Sharp (the default) | Crisp: tighter history clipping, slow pans resampled less, uncovered surfaces drop their history, transparent surfaces take mostly the current frame, a faster response to motion, and a light sharpening of the displayed image only. |
| Original | The first TAA, unchanged: softer and steadier. Texture detail and slow pans look blurrier than with Sharp. |
| Custom | Every tunable below, as you set it. |

Their values:

| Tunable | Original | Sharp |
| --- | --- | --- |
| History Frames | 8 | 8 |
| Motion Response | 0.03125 | 0.0625 |
| Max Motion Weight | 0.5 | 0.5 |
| Resample Weight | 0 | 0.25 |
| History Clamp | Range | Tight (variance) |
| Clip Width | (not used) | 1.25 |
| Transparent Weight | 0 | 0.5 |
| Disocclusion Rejection | off | on |
| Disocclusion Threshold | (not used) | 0.05 |
| Sharpening | 0 | 0.5 |

Switching the preset takes effect on the next frame without starting the image again, so you can compare them while the
game plays: open a sample, press Play, select the entity with the Post Process component and switch **TAA Preset** in
the Inspector while you move.

## The tunables

The tunables are greyed out unless the preset is Custom; their tooltips say what the preset in effect uses.

- **History Frames**: about how many frames the image averages; the current frame weighs at least 1 / this. At 8, every
  sample position counts once. More is smoother and steadier but lags behind lighting changes; fewer reacts faster but
  edges crawl. Below 8, a capture taken at the eighth frame holds a mix weighted toward the newest frames instead of the
  even mean of the eight positions (the same every time).
- **Motion Response**: how quickly moving surfaces let go of the history. At 0.0625, motion of 8 pixels a frame weighs
  the current frame 0.5. Higher keeps moving things crisper and their trails shorter, but they get less anti-aliasing
  while they move. 0: motion does not matter.
- **Max Motion Weight**: the most the current frame weighs, however fast things move. 1 lets very fast motion drop the
  history: sharp, but aliased.
- **Resample Weight**: the current frame's least weight where the history is read between pixels, as in slow pans; it
  is largest halfway between them. Each frame of a slow pan reads the history between pixels again, which softens it a
  little more every time: Original keeps about half of fine texture detail in a slow pan, Resample Weight 0.25 about
  three quarters. Higher keeps slowly moving texture crisper, but its edges get less anti-aliasing while they move (at
  0.5, the stair-steps of edges show again in slow pans). 0: off.
- **History Clamp**: **Range** limits the history to the range of the colors around the pixel; **Tight (variance)**
  limits it to a narrower box around their average, which removes more ghosting and trails (see above).
- **Clip Width** (Tight only): the box's half width, in standard deviations of the colors around the pixel. Lower
  removes more trails, but lets edges flicker and lets noise through.
- **Transparent Weight**: the current frame's least weight where a blended transparent surface covers the pixel (half
  cover or more counts as full). 0 treats transparent surfaces like everything else, so they ghost when they move; 1
  drops their history, so they never ghost, but their edges shimmer.
- **Disocclusion Rejection**: drop the history where a surface was just uncovered (something nearer covered it in the
  previous frame).
- **Disocclusion Threshold**: how much nearer the previous frame's surface must have been, as a fraction of the
  distance, for the pixel to count as uncovered. Lower catches thin gaps, such as a hand in front of a wall, but may drop
  the history of surfaces moving fast away from the camera.
- **Sharpening**: sharpens the displayed image after TAA, never the history, to restore the texture detail the
  averaging softens. Strong edges are sharpened much less and overshoot is bounded, so halos stay faint. 0 is off.

### Starting from a preset

Choosing **Custom** in the Inspector starts from the look in effect: from Original, Custom shows Original's values;
from Sharp, Sharp's. If you tuned Custom before, switching to a preset and back to Custom keeps your tuning, and each
choice is one undo step.

Files, scripts and agents set the values they give. This `component.set` starts Custom from Original's values (to
change one technique against the first TAA):

```json
{"entity": "<post process entity>", "component": "PostProcess", "values": {"TAAPreset": "Custom", "TAAHistoryFrames": 8,
 "TAAMotionResponse": 0.03125, "TAAMaxMotionWeight": 0.5, "TAAResampleWeight": 0, "TAAHistoryClamp": "MinMax",
 "TAATransparentWeight": 0, "TAADisocclusionRejection": false, "TAASharpening": 0}}
```

## Limits

- **Surfaces moving fast away from the camera.** Motion on screen says nothing about distance, so a surface that
  recedes faster than the threshold each frame (about half a meter a frame at 10 m with 0.05) looks uncovered to
  disocclusion rejection and loses its history while it does: its edges are not smoothed for those frames.
- **The uncovered band.** Where a moving object uncovers the background, the band starts from one frame and then
  averages in: its edges stay less smooth than the rest of the image for about 17 frames.
- **Slow pans under Original.** Original keeps about half of fine texture detail while the camera pans slowly; Sharp
  keeps about three quarters and sharpens the image. Choose Sharp (or raise Resample Weight) where slow camera motion
  over detailed textures matters.
