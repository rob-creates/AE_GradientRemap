# AE Gradient Remap Plugin — Handoff Brief

## Goal
Simple, robust black→white (2-stop) gradient/colour remap effect for After Effects,
built as a C++ SmartFX plugin. Motivated by limitations in Colorama (imprecise
pure black/white due to knot blending) and Tint (awkward colour repositioning).

## Core behaviour
- Compute luminance from input pixel (selectable formula: Rec.709 / Rec.601 / average)
- Lerp between two user-defined colour stops (black-point colour, white-point colour)
  based on luminance value
- **No colour space conversion.** Interpolate directly in AE's project working space —
  gradient editor swatches must match scene output exactly (WYSIWYG). Do not convert
  to/from linear light.
- Precise 0/1 mapping by construction (direct 2-point lerp, no spline/knot blending —
  this is the core fix over Colorama)

## Parameters
- `PF_ADD_COLOR` × 2 — black-point colour, white-point colour
- `PF_ADD_POPUP` — luma formula (709 / 601 / average)
- `PF_ADD_FLOAT_SLIDER` × 2 — black/white input levels (optional built-in contrast remap)
- `PF_ADD_CHECKBOX` — clamp float input to [0,1] before mapping (optional; only relevant
  for 32bpc since integer formats are inherently clamped)
- `PF_ADD_CHECKBOX` — remap alpha (apply same lerp logic to alpha channel)
- `PF_ADD_CHECKBOX` — dither output (8bpc only)

## Bit depth handling
- Build the 32bpc float path first — canonical implementation
- 8bpc / 16bpc are normalized-to-float wrappers around the same core lerp function,
  not separate implementations
- Use `PF_MAX_CHAN8` / `PF_MAX_CHAN16` for normalization, not hardcoded 255/32768
- 32bpc float pixels can exceed [0,1] (HDR) or go negative — clamp toggle decides
  whether to clip or let the lerp extrapolate past the chosen stop colours

## Alpha
- Need to confirm (check current AE SDK headers): does AE deliver premultiplied or
  straight alpha to effects by default, and what flag (if any) requests the other?
  Don't assume — grep `AE_Effect.h` / relevant suite headers.
- Once confirmed: alpha remap is the same working-space lerp applied to the alpha
  channel, gated behind its own checkbox — no additional colour handling required

## Dithering (8bpc only)
- Ordered dithering (small Bayer matrix, e.g. 8×8) or interleaved gradient noise
- Add as sub-LSB offset to the float result immediately before quantizing to 8-bit
- Stateless, per-pixel from (x,y) — gate behind bit-depth check, skip for 16/32bpc

## Architecture
- SmartFX (`PF_Cmd_SMART_RENDER`)
- Iterate via `PF_iterate8Suite` / `PF_iterate16Suite` / `PF_iterateFloatSuite`,
  or normalize-to-float internally and dispatch from one core function
- Preserve alpha unchanged unless the alpha-remap checkbox is enabled

## Maintenance profile (context, not action items)
- CPU-only, no GPU kernel, no temporal state — low API surface area for AE SDK churn
- Expect: macOS re-signing/notarization on cert expiry, occasional rebuild on major
  AE SDK version bumps (infrequent for effects this simple)