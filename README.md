# Text Effect Tools — Flame Text Source (OBS plugin)

An OBS Studio **input source** that renders user text as a living flame and
emits glowing spark particles that overshoot the glyphs. Built on the
`obs-plugintemplate` toolchain and FreeType, reusing the font handling proven
in the sibling `dokavendor` plugin.

## What it does

- Renders any text string in a system font (chosen via OBS's font picker).
- Draws the text shape as **fire** with a GPU fragment shader: FBM-noise
  turbulence scrolls upward over time, with a white/yellow hot base fading to
  red and smoky tips. It always shimmers (driven by a `time` uniform).
- Emits **spark particles** from the top edge of the text. Particles are
  simulated on the CPU (no compute shaders / GPGPU) and drawn additively.
- Uses a padded canvas so flame and sparks can spill outside the glyph bounds —
  the reason this is a *source* and not a *filter*.

## Architecture

| File | Role |
| --- | --- |
| `src/plugin-main.c` | Module entry, registers the source |
| `src/flametext-source.c/.h` | Source lifecycle, properties UI, render loop |
| `src/flametext-text.c/.h` | FreeType → single padded coverage-mask texture |
| `src/flametext-particles.c/.h` | CPU spark pool (struct array), additive draw |
| `src/flametext-font-resolve-win.c` | Font face name → file path (Windows) |
| `data/effects/flame.effect` | Rising-flame fragment shader |
| `data/effects/spark.effect` | Additive ember sprite shader |

### Text rendering decision (spec §2.1 / §7)

The spec left the text-rendering approach (A/B/C) open. Because `dokavendor`
already ships a working **FreeType** integration plus a Windows font-name → path
resolver, this plugin reuses that path (**option A**): full FreeType rasterization
with system fonts. No separate text engine is bundled beyond FreeType.

The difference from `dokavendor`: instead of per-glyph colored quads, the text
is baked into **one grayscale coverage mask** (`GS_R8`) sized to the padded
canvas. The flame shader samples that mask as its fuel source, and the spark
emitter reads the mask's text band for emission positions.

## Render pipeline (per frame)

1. `flametext-text` builds the coverage mask on settings change (under the
   graphics lock). The text sits low in the canvas, leaving ~2.4× font-size of
   headroom above for flame and sparks.
2. `video_tick(dt)` advances the flame clock and steps the particle simulation
   (delta-time based, frame-rate independent).
3. `video_render`:
   - Flame pass: full-canvas sprite with `flame.effect` (alpha blend).
   - Spark pass: one batched `gs_render_start` draw of all live embers with
     `spark.effect` (additive blend).

## Properties

- **Text**, **Font** (system font picker)
- Flame: **height**, **sway speed**, **color temperature**, **intensity**
- Sparks: **emission rate**, **initial speed**, **lifetime**, **size**,
  **spread**, **color** (color picker)

## Build (Windows / CMake)

```
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

The configure step downloads the OBS sources / prebuilt deps pinned in
`buildspec.json`. FreeType is resolved via `find_package` (vcpkg manifest
`vcpkg.json`, or the obs-deps prefix). Copy the built `.dll` and the `data/`
folder into your OBS plugin directory, e.g.
`%AppData%\obs-studio\plugins\effect-tools\bin\64bit\`.

## License (GPLv2) — source availability

OBS Studio is GPLv2 and so is this plugin. If you distribute binaries you must
make the corresponding source available to recipients. Ship
`dist/SOURCE-NOTICE.txt` alongside the binary with a link to this repository.
