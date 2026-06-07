# Font Effect Tools (OBS source plugin)

An OBS Studio **input source** that renders user text as a living flame and
emits glowing spark particles that overshoot the glyphs. Built on the
`obs-plugintemplate` toolchain and FreeType, reusing the font handling proven
in the sibling `dokavendor` plugin.

In OBS the source appears in **Add Source → "Font Effect Tools"** (internal id
`flame_text_source`).

## What it does

- Renders any text string in a system font (chosen via OBS's font picker).
- Draws the text shape as **fire** with a GPU fragment shader: FBM-noise
  turbulence scrolls upward over time, with a white/yellow hot base fading to
  red and smoky tips. It always shimmers (driven by a `time` uniform).
- Emits **spark particles** from the text, either off the **top** or the
  **bottom** edge (selectable). Particles are simulated on the CPU (no compute
  shaders / GPGPU) and drawn additively with a soft **bloom** glow.
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
| `data/effects/spark.effect` | Additive ember sprite shader (with bloom) |

### Text rendering approach

`dokavendor` already ships a working **FreeType** integration plus a Windows
font-name → path resolver, so this plugin reuses that: full FreeType
rasterization with system fonts. No separate text engine is bundled beyond
FreeType.

The difference from `dokavendor`: instead of per-glyph colored quads, the text
is baked into **one grayscale coverage mask** (`GS_R8`) sized to the padded
canvas. The flame shader samples that mask as its fuel source, and the spark
emitter reads the mask's text band (top or bottom) for emission positions.

## Render pipeline (per frame)

1. `flametext-text` builds the coverage mask on settings change (under the
   graphics lock). The text sits low in the canvas, leaving ~2.4× font-size of
   headroom above for flame and sparks.
2. `video_tick(dt)` advances the flame clock and steps the particle simulation
   (delta-time based, frame-rate independent).
3. `video_render`:
   - Flame pass: full-canvas sprite with `flame.effect` (alpha blend).
   - Spark pass: one batched `gs_render_start` draw of all live embers with
     `spark.effect` (additive blend). Per-particle `(heat, brightness)` is
     carried in `TEXCOORD1`; the shader's hot core + wide halo produces bloom.

## Properties

| Property | Range | Default |
| --- | --- | --- |
| Text | — | `test` |
| Font (system picker) | — | Impact |
| Flame height | 0.0 – 0.2 | 0.10 |
| Sway speed | 0.1 – 3.0 | 1.10 |
| Color temperature | 0.5 – 2.0 | 1.00 |
| Flame intensity | 0.3 – 2.5 | 1.00 |
| Spark emission rate | 0 – 1500 | 150 |
| Spark initial speed | 40 – 600 | 140 |
| Spark lifetime (s) | 0.3 – 3.0 | 2.25 |
| Spark size | 1 – 20 | 2.0 |
| Spark spread | 0.0 – 1.0 | 1.00 |
| Spark color | color picker | pale yellow `#FFFFDC` |
| Spark origin | above / below the text | above |
| Spark bloom | 0.0 – 3.0 | 1.0 |

## Build (Windows / CMake)

```
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

The configure step downloads the OBS sources / prebuilt deps pinned in
`buildspec.json`. FreeType is resolved via `find_package` (vcpkg manifest
`vcpkg.json`, or the obs-deps prefix).

> **Windows SDK note:** the committed `windows-x64` preset pins SDK
> `10.0.22621`. If that SDK is not installed, add a local (git-ignored)
> `CMakeUserPresets.json` that inherits `windows-x64` and overrides
> `architecture` to your installed SDK (e.g. `x64,version=10.0.26100`), then
> build with `--preset local`.

### Packaging

A `dist` install component emits exactly the tree that ships:

```
cmake --install build_x64 --config RelWithDebInfo --component dist --prefix package
```

```
package/
├─ bin/
│   ├─ font-effect-tools.dll
│   └─ font-effect-tools.pdb
└─ font-effect-tools/
    ├─ effects/{flame,spark}.effect
    └─ locale/{en-US,ja-JP}.ini
```

### Install into OBS

The module name is `font-effect-tools`, so the OBS data directory **must** be
named `font-effect-tools` (matching the DLL basename). Place the two parts as:

- `bin/font-effect-tools.dll` → `<obs>/obs-plugins/64bit/font-effect-tools.dll`
- `font-effect-tools/` (effects + locale) →
  `<obs>/data/obs-plugins/font-effect-tools/`

(`<obs>` is your OBS install, e.g. `C:\Program Files\obs-studio`.) Restart OBS
fully afterwards.

## License (GPLv2) — source availability

OBS Studio is GPLv2 and so is this plugin. If you distribute binaries you must
make the corresponding source available to recipients. Ship
`dist/SOURCE-NOTICE.txt` alongside the binary with a link to this repository.
