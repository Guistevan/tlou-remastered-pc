# Volumetric light banding on RTX 5060

This opt-in workaround builds a separate Vulkan shader cache for CUSA00552. It was tested on Windows with game version 01.11 at 1920×1080. The reporter verified Pittsburgh sunlight, two sewer openings, the pursuing truck's spotlight, and enemy flashlights in the hospital. Screenshots for each scene are in [docs/images/issue-55](../../docs/images/issue-55).

## Building and selecting the cache

1. With the ordinary cache selected, visit the affected scenes so the runtime can compile the original modules and save their pipeline keys. Close the game. The input directory must contain the CUSA00552 subdirectory. Keep this cache for rollback.
2. Use the launcher and probe script from this change. The builder needs Python, the runtime's xxHash shared library and the SPIRV-Cross and glslangValidator executables. The tested executables came from the RenderDoc 1.38 Windows distribution. Pass their paths explicitly if they are outside PATH.
3. From the checkout, run the following PowerShell command, replacing the two cache paths with local paths. The output must be a new directory whose parent exists.

```powershell
python tools/volumetrics/build_cache.py "D:/caches/original" "D:/caches/issue-55" --xxhash "gow3_pc/out/libxxhash.dll" --cross "spirv-cross.exe" --glslang "glslangValidator.exe"
```

4. Select the new cache root in the launcher's cache folder setting and restart the game. To revert, select the original root and restart. A missing module or pipeline key means the input does not contain the full tested set. An unexpected module hash requires reviewing the translation for that build.

## What changes

The translated volumetric passes store visibility along discrete lines radiating from the projected light. Interpolation across these lines can leave visible radial wedges, especially where a foreground object cuts through the beam. Mixing samples from different scene depths also spreads the foreground silhouette into the background. Increasing blur alone softened the light while leaving those discontinuities visible in playtesting. The underlying reason GTX and RTX differ is still open. The reporter observed the artifact on an RTX 5060 and reported the original PS4 and GTX appearance as references.

For the supported 512×512 visibility atlas, the compute shaders integrate the visible portion of each camera ray with 96 staggered shadow samples. Adjacent pixels sample different positions in each interval. The resolve reads visibility and scene depth together and weights a 6×6 neighborhood by spatial distance and depth agreement. An encoded marker selects the corresponding resolve. The original atlas format remains readable.

- Directional light computes the ray interval inside the light volume, clips it against scene depth, and samples the corresponding shadow cascade. This confines the contribution when looking through an opening or crossing the volume boundary.
- The truck spotlight intersects the ray with the finite cone and range of the source. Samples use the active shadow-map type and, for array maps, the light's slice.
- Weapon flashlights have an additional projected texture. Each visibility sample includes its projected RGB intensity, preserving the flashlight pattern. In the hospital capture the compute pass was 0xcdecdf59 and the resolve was 0xca25dbad.
- The builder matches SHA-256 fingerprints for all eight original modules, updates each replaced module's XXH3 metadata, and records the affected pipeline keys. At launch, the probe prepends these keys to the runtime warm-up order. On-demand recompilation had replaced a modified variant during testing.

The comparison images come from actual gameplay and photo mode with different camera positions. The user checked moving lights and camera transitions in game. Performance across GPUs, other game versions, and a driver-level cause have not been established.
