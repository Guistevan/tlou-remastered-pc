# RTX vertical stripes (#58)

## What caused the stripes

The bands stayed at fixed screen columns while recognizable wall features moved. In the original Capitólio recording the dominant spacing was 27.7 pixels after desktop scaling. The synchronized native 1080p capture showed the same pattern at 30 pixels in the final image and 15 pixels in the half-resolution ambient occlusion map. The native image measured essentially the same amplitude immediately before and after FXAA. The AO map already contained the bands before the image reached FXAA or the window scaler.

The AO compute shader `aaae0737` samples the center depth and normal using normalized UVs. When full-resolution inputs feed a half-resolution AO map, an AO pixel center lands exactly between input texels. Small floating-point or sampler differences can change which neighbor is selected. Replacing those two reads removed the dominant stripe pattern on the tested RTX 5060. This isolates the unstable center reads in the affected configuration. It does not identify a specific NVIDIA driver implementation defect.

## What changed

For each axis, the shader calculates the source index with integer arithmetic:

```text
sourceIndex = min(((2 * aoIndex + 1) * sourceSize) / (2 * aoSize), sourceSize - 1)
```

The division uses positive integers. The shader reads depth and normals with `texelFetch`, using the actual source and AO texture dimensions. The AO sample kernel, its two filters, material lighting and FXAA are preserved. The same arithmetic works for different render sizes and avoids a screen-space blur or a blanket reduction of AO. The tested shader entry has no fixed render-size tag.

## Reproduce the fix on Windows

1. Use port v0.4.0 with CUSA00552 update 1.11. Run the affected port and close it after it has cached the shader. The launcher shows the existing cache root under **Folders**. It contains a `CUSA00552` subdirectory.
2. Use Python 3. The port's bundled `python/python.exe` works. Provide the directory containing `spirv-cross.exe`, `glslangValidator.exe`, `spirv-dis.exe` and `spirv-as.exe`. The validated tools came from the RenderDoc 1.38 Windows distribution's `plugins/spirv` folder.
3. Extract this bundle and run the following from the bundle directory. Replace the example paths. The destination must be a new directory.

```powershell
& 'C:/path/to/port/python/python.exe' .\build_cache.py `
    'C:/path/to/original-cache' 'C:/path/to/issue-58-cache' `
    --spirv-tools 'C:/path/to/RenderDoc/plugins/spirv' `
    --runtime-out 'C:/path/to/port/gow3_pc/out'
```

4. In the launcher, choose the new **cache root**, then start the game. The build copies the previous shader entries and preserves their priority list, checks the original SPIR-V and cache metadata, and builds the replacement using your own cache. Driver pipeline binaries are rebuilt locally. Your saves and original cache are untouched.
5. For an A/B test or rollback, close the game and switch the launcher back to the previous cache root. For best comparison, stay by the same wall and repeat the same camera movement in each run.

The builder is deliberately guarded against an unrecognized shader or cache format. The reproducible fix targets the verified shader permutation from the Windows v0.4.0 build. Integrating equivalent integer center sampling into the recompiler would make it available automatically after cache regeneration or with other shader permutations.

## Validation and captures

- **GeForce RTX 5060, driver 621.14.** In the Capitólio, the fixed-column component fell by about 97% in the AO mask and 92% in the final image over 15 sampled frames per run. [Capitólio before and after](capitol-before-after.png)
- The hospital wall by the double door was checked at **1920×1080 and 2560×1440**. The target grid no longer stood out in the 1080p analysis, and the 1440p capture confirmed the shader was used with a 1280×720 AO mask. [Hospital at 1080p](hospital-1080p.png) · [Hospital at 1440p](hospital-1440p.png)
- The player reports testing the fix on an RTX 5060 across **most chapters**. The instrumented comparisons in this bundle are from the Capitólio and the hospital.
- Integer center mapping was checked for all 25 resolutions listed in the Windows launcher, including 4K and ultrawide. The captures validate the two resolutions above in-game.
- [Fixed columns during the original camera sweep](column-tracking.png) · [Synchronized render stages before the fix](capitol-render-stages.png)

The masks in the before/after figures use the same amplified contrast. The two camera angles are close but not pixel-identical. The percentages refer to the measured fixed-frequency component, not a guarantee of zero visual differences in every scene.
