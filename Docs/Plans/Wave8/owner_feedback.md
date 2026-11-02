# The owner's feedback after 0.7.0 (2026-11-01, verbatim)

"Make prioritization yourself, but showcase needs to land this wave, you can update the showcase later if there are more
things, here is the feed back:"

1. Contact shadow is terrible; shimmering, and inconsistent. Fix or default to turn off. (don't prioritize this at all,
   just default it to off, unless it is a easy fix)
2. Add SH irradiance probes. (have this if radiance cascade is preventatively hard to implement, or skip this)
3. Unlock the number of shadowed point and spot light, don't limit it to 16. (easy to do)
4. Double check specular lighting, or show the specular light bouncing off of metal sheets like the side of a bus or oil
   tank or wet ground. (pretty important for the showcase)
5. Implement radiance cascade (using path of exile 2 technique)
6. Volumetric and light cones (less urgent)
7. Sub surface scattering (not urgent)
8. In the physics playground, when shooting a ball a bit away from the playground in the air, while the ball is in front
   of the camera the shadow behind jumps or changes. You can see this bug in the default position and shooting balls and
   observe the shadows of the walls jump a bit while the ball shoots from the camera to the playground. (pretty weird
   bug, investigate and work on it)
9. Add animation system from esoterica (evaluate if this can be done in wave 8, if not then record and push into wave 9,
   but this is really really important)
10. Have drag and drop from the content browser to scene with preview ghost at the ray-cast of the mouse pointer into the
    scene when the ray hit an entity that make sense. (should be easy to do)
11. Currently the physics frame rate is constant or something, change this to an option or something and/or make it tick
    by tick following the frame refresh. Currently physics refresh implementation is locked at 60 and is terrible at high
    frames (character appear to be jittering). (easy to do)
12. Expose the editor idle frame rate limit thing. Have reasonable choices exposed. Like the timeout length (with 0 for
    unlimited), and options. (easy to do)
13. Make a button in performance view to pause either the editor render view or pause the scrolling frame breakdown,
    otherwise I can't hover my mouse on a frame breakdown and see what is composed of it in terms of time cost because it
    is literally scrolling at a thousand frames a second. (easy to do)
14. Change to slang for hlsl for shaders due to possible console port in the future
15. Make sure glfw is wrapped for future console port

Added by the owner the same day (verbatim): "This is the esoterica engine that the animation system should be based on
https://github.com/BobbyAnguelov/Esoterica. Also radiance cascade, you will need to research as well."

So Strata's animation system is to be based on Esoterica's (item 9), and radiance cascades (item 5) are to be researched from
their published sources before they are designed.
