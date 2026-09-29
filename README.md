# Gradient Remap for After Effects

![Gradient Remap demo](docs/images/demo.gif)

A luminance-to-gradient colour remap effect for Adobe After Effects: a precise,
multi-knot alternative to Colorama and Tint. Black maps exactly to the first knot and white
exactly to the last, with no knot-blending drift.

## Features

- **Gradient bar editor:** click to add a knot, drag to move it (knots can pass each
  other), drag off the bar to delete, double-click to pick a colour. The **Knot Position**
  field sets the selected knot's position numerically.
- **Colour Space:** OKLCH (default, perceptually smooth hue/chroma), Native (straight
  working-space blend) or Linear Light.
- **Interpolation:** Cubic, Ease, Linear or Step.
- **Range:** Offset, Cycles and Loop (Cycle / Wave / Bounce) to shift and repeat the
  gradient across the input range.
- **Save / Load Gradient** as a simple `position,r,g,b,a` CSV (TouchDesigner Table DAT
  compatible).
- 8, 16 and 32 bpc, with optional banding reduction at 8 bpc and range clamping at 32 bpc.

Found under **Effect > Color Correction > Gradient Remap**.

## Install (macOS)

Copy `GradientRemap.plugin` into `/Applications/Adobe After Effects <version>/Plug-ins/`
and restart After Effects. When replacing an existing copy, delete the old bundle first
so After Effects doesn't reuse its cached plugin info.

## Build

Requires CMake, Xcode command-line tools and the
[After Effects SDK](https://developer.adobe.com/after-effects/) (not included), unpacked
to `AfterEffectsSDK/` at the repo root.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build   # core colour/interpolation tests
```

The plugin bundle is written to `build/GradientRemap.plugin`.

## Licence

Freeware under the MIT licence, provided as-is with no support or maintenance. See
[LICENSE.txt](LICENSE.txt).

Adobe and After Effects are trademarks of Adobe Inc. This project is not affiliated with
or endorsed by Adobe.
