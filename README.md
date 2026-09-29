# Gradient Remap for After Effects

![Gradient Remap demo](docs/images/demo.gif)

A simple colour remap effect for Adobe After Effects with precise colour placement, created as an alternative to Colorama and CC Toner.

## Features

- **Gradient bar editor:** click to add a knot, drag to move it (knots can pass each
  other), drag off the bar to delete, double-click to pick a colour. The **Knot Position**
  field sets the selected knot's position numerically.
- **Colour Space:** OKLCH (default, perceptually smooth hue/chroma), Native (straight
  working-space blend) or Linear Light.
- **Interpolation:** Cubic, Ease, Linear or Step.
- **Range:** Offset, Cycles and Loop (Cycle / Wave / Bounce) to shift and repeat the
  gradient across the input range.
- **Save / Load Gradient** export and share colour gradients in CSV format (TouchDesigner Table DAT
  compatible).
- 8, 16 and 32 bpc, with optional banding reduction at 8 bpc and range clamping at 32 bpc.

Appears in After Effects under **Effect > Color Correction > Gradient Remap**.

## Install (macOS)

The current release is built for macOS only. It requires an Apple Silicon Mac running
macOS 12 or later, and is signed and notarized. A Windows version is not yet available.

1. Download the zip from the [latest release](https://github.com/rob-creates/AE_GradientRemap/releases/latest)
   and unzip it.
2. Copy `GradientRemap.plugin` into `/Applications/Adobe After Effects <version>/Plug-ins/`.
3. Restart After Effects.

When replacing an existing copy, delete the old `GradientRemap.plugin` first so After
Effects doesn't reuse its cached plugin info.

## Build

Requires CMake, Xcode command-line tools and the
[After Effects SDK](https://developer.adobe.com/after-effects/) (not included), unpacked
to `AfterEffectsSDK/` at the repo root.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build   # core colour/interpolation tests
```

The plugin bundle is written to `build/GradientRemap.plugin`. `scripts/release.sh` builds,
signs, notarizes and packages a release (needs a Developer ID certificate and a
`notarytool` keychain profile).

## Licence

Freeware under the MIT licence, provided as-is with no support or maintenance. See
[LICENSE.txt](LICENSE.txt).

Adobe and After Effects are trademarks of Adobe Inc. This project is not affiliated with
or endorsed by Adobe.
