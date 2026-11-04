# Background transitions without AFBC

The background fade limit is enabled by default. A background control retains
at most one outgoing image and one incoming image. If selection changes while
that pair is fading, it finishes that transition and then loads the newest
pending selection. Intermediate pending names are replaced without decoding,
uploading or retaining more textures. Returning to the current incoming image
withdraws a pending selection. Existing motion suppression remains independent.

The final background can therefore lag selection by the remaining transition
and image-loading time. The limit avoids a brightness jump from abruptly
removing a still-visible layer. Controls classified as foreground keep their existing fade behavior. Background classification uses the existing declared-coverage and
optional selection-image rules; this is not a measurement of occluded pixels.

## Device comparison

Merge these GUI elements into advancedsettings.xml and restart Kodi:

```xml
<advancedsettings>
  <gui>
    <limitbackgroundfades>true</limitbackgroundfades>
    <backgroundblendres>1080</backgroundblendres>
    <asynctextureupload>false</asynctextureupload>
    <afbcfanart>false</afbcfanart>
    <afbcimages>false</afbcimages>
  </gui>
</advancedsettings>
```

Both optimizations work with ordinary textures, synchronous uploads and AFBC
textures. They do not change the JPEG cache or fanartres/imageres. Keep the
existing motion-delay options disabled when comparing actual crossfades;
otherwise those options can suppress the workload.

For a native-resolution comparison, set backgroundblendres to 0 (its default).
For the old interrupted-stack behavior, set limitbackgroundfades to false.
Compare one switch at a time and keep the skin/view/input sequence unchanged.
Check perceived smoothness with debug logging off as well as collecting a short
capture with it on; logging and its statistics overlay add work.

## Lower-resolution blending

On Linux Amlogic GLES, backgroundblendres limits the temporary blend height and
its width to the corresponding 16:9 bound. A 3840x2160 GUI with 1080 selected
uses a 1920x1080 ordinary RGBA target. Smaller viewports draw directly. The
setting accepts 0..2160; 0 disables this path. No private Mali import, ION
allocation, CPU image resize or AFBC support is needed.

Only a ready, wholly translucent background fade enters this target. Opaque
passes, stereo, missing images, unsupported/incomplete targets and completed
fades use the normal path. This preserves front-to-back opaque occlusion.
The ordinary GUI shader draws the fading images at the reduced viewport,
then one bilinear upsample composites the result at the original image depth.
Premultiplied blending preserves translucent fanart brightness and alpha.
Source textures keep their original dimensions throughout. Once the fade ends,
Kodi samples the original image directly at GUI resolution again; a 2160p
source becomes fully detailed again rather than retaining the reduced result.

The helper preserves the enclosing framebuffer, viewport, scissor and depth
state, including an existing Dolby Vision GUI composite. It fills the temporary
target before applying the destination damage scissor to avoid filtered holes
at damage edges. One framebuffer, texture and small shader are reused per GUI
EGL context and retired with that exact context. A 1080p target uses about
7.9 MiB of RGBA storage and has no depth attachment. Creation failure disables
further attempts for that context and leaves ordinary rendering available.
There are no readbacks, CPU comparisons or added GPU completion waits.

The first allocation logs `GUI background blend: active; temporary target
1920x1080`. That confirms admission and target creation, not a hardware
performance improvement. There is one extra full-resolution upsample, so a
Wall that already occludes most fanart may gain less or regress. Device testing
is required; this option cannot remove expensive control processing or late
poster uploads. It is deliberately independent of AFBC and defaults to off.

## Host validation

Run `python3 tools/background-blend/test-fades.py` to compile and exercise the
production image transition methods under ASan/UBSan. The fixture controls
image readiness and covers rapid selections, reversals, pending cache policy,
resource retirement, existing motion suppression and disabled/foreground
compatibility, including 2,000 retargets.

Run `python3 tools/background-blend/test-integration.py` to check the actual
render admission and effective-alpha methods, including debug-off execution,
opaque passes, stereo, incomplete readiness, disabled settings and fallback.

Run `python3 tools/background-blend/test-blend.py` with EGL/GLES headers and
Mesa installed. Set GLES_HEADERS to a Khronos include directory when needed.
It compiles the production helper under ASan/UBSan and compares real software
GLES RGBA output against native composition through 4K, including gradients,
translucency, depth occlusion, damage clipping, state restoration, target
reuse/resize, exception cleanup, injected incomplete-framebuffer fallback
and retirement with colliding names in another context.
Readback belongs only to this test. Mesa does not emulate G52 performance.
