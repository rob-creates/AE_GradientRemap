# Design: Multi-Knot Wide-Gamut Gradient Remap

This supersedes `Claude.md`'s 2-stop-only scope. `Claude.md` is kept for history; this
document is the source of truth for the multi-knot, multi-mode design going forward.

## WYSIWYG reconciliation

`Claude.md`'s "no colour-space conversion, interpolate directly in working space" rule
now applies only to **knot authoring/display**: the colour a user picks for a knot is
always the literal working-space value, shown WYSIWYG, never converted for storage or
display. What happens **between** knots is a per-mode choice — some modes convert to an
intermediate space (linear light, OKLab) for the blend math and convert back before
writing the pixel. Because a correct round-trip conversion returns exactly the authored
value at t=0/t=1, WYSIWYG at the knots is preserved by construction. This is enforced by
`TestWysiwygEndpoints()` in `GradientRemap/src/tests/main.cpp`, not just assumed.

## Build order

1. **Phase 0 — Scaffolding.** Vendor the AE SDK, get its own `Skeleton` sample building
   and loading in AE, before writing project-specific plugin code.
2. **Phase 1 — Data model + interpolation core (this repo state).** AE-independent:
   `GradientRemapCore` has zero AE/Cocoa dependencies and is validated via
   `GradientRemapCoreTests` (`ctest`).
3. **Phase 2 — Minimal AE integration.** Wrap the validated core behind a throwaway
   fixed set of stock params (not the final custom UI), across all three bit depths.
   Resolve the real working-colour-space query and alpha default here (see Open
   Questions).
4. **Phase 3 — Full custom multi-knot gradient-bar UI.** `PF_ADD_ARBITRARY2` knot-list
   param + `PF_CUSTOM_UI_INFO`/`PF_Event` drawing and hit-testing, up to 256 knots.
5. **Phase 4 — CSV export utility.** Separable; ships after Phase 3.

## Data model

```cpp
struct GradientKnot { float position; float r, g, b, a; }; // working-space, straight alpha
struct GradientData {
    InterpMode interpolation_mode;
    std::vector<GradientKnot> knots; // sorted by position, size in [2, 256]
};
```

Flatten format (explicit little-endian, no pointers — see `GradientData::Flatten()`):
```
[u32 magic 'GRRM'][u32 version][u8 interp_mode][u8 knot_count][u16 reserved]
knot_count * [f32 position][f32 r][f32 g][f32 b][f32 a]
```

When wired into AE's `PF_ADD_ARBITRARY2`, all required callbacks (`NEW`/`DISPOSE`/
`COPY`/`FLAT_SIZE`/`FLATTEN`/`UNFLATTEN`/`INTERP`/`COMPARE`) must thin-wrap into this
struct's methods. `COMPARE_FUNC` must catch every change (including knot reordering) or
AE's render cache will go stale. Resolve the checked-out arbitrary data once per
`PF_Cmd_SMART_PRE_RENDER`, precomputing each knot's linear-light/OKLab representation
once per render rather than once per pixel.

## Interpolation modes

One dispatch function (`EvaluateGradient` in `Interpolation.cpp`), sharing a knot-bracket
search and differing only in the blend step:

- **NaiveLerp** — direct working-space RGBA lerp, no conversion. Matches the original
  2-stop brief exactly for 2-knot gradients.
- **LinearLight** — linearize via `WorkingSpaceTransform::ToLinear()` (currently a
  hardcoded sRGB EOTF stand-in — see Phase 2 note below), lerp, re-encode. Alpha always
  lerps linearly regardless of mode.
- **OKLCH** — linearize, convert to OKLab, then polar OKLCH; lerp L and C linearly, lerp
  hue via the shortest angular path (wrap delta into `(-pi, pi]` before interpolating);
  convert back OKLCH→OKLab→linear→working space. Uses Björn Ottosson's standard
  sRGB↔OKLab matrices (D65-referenced); see `Interpolation.cpp` for exact coefficients.
  Degenerate/near-zero-chroma knots (greys, black, white) skip hue interpolation
  entirely (an achromatic knot's hue angle is meaningless noise) and just inherit the
  other knot's hue.

**Known v1 simplification**: the OKLab matrices assume Rec.709/sRGB primaries. If the
queried working space later turns out to use wider primaries (Rec.2020, ACEScg), hue
results will be subtly off until the matrices are re-derived for that primary set (open
question below) — this is a deliberate, documented v1 limitation, not an oversight.

`GradientRemap/src/tests/main.cpp`'s `TestOklchAvoidsLuminanceDip()` demonstrates and
guards the concrete property motivating OKLCH: a naive red→green lerp passes through a
perceptually dark/muddy midpoint (its OKLab lightness dips measurably below the
endpoints' mean), while OKLCH's midpoint lightness tracks the endpoint mean closely,
because OKLCH lerps perceptual lightness directly instead of raw gamma-encoded RGB.

## `WorkingSpaceTransform` — Phase 2 replacement point

`WorkingSpaceTransform::ToLinear`/`ToWorkingSpace` in `Interpolation.cpp` currently
hardcode the sRGB EOTF/OETF as a Phase-1 stand-in. Phase 2 replaces only the bodies of
these two functions with AE's actual queried working-space transform (see Open Question
1) — no caller in `Interpolation.cpp` needs to change, since every blend mode is written
against this interface rather than the sRGB curve directly.

## CSV export (Phase 4)

Flat CSV, header row, one row per knot, no metadata/comment rows, no clamping (HDR
values >1.0 preserved so the export reflects authored data, not a display-clamped
view), Table-DAT-friendly for TouchDesigner:
```
index,position,r,g,b,a
0,0.000000,0.000000,0.000000,0.000000,1.000000
1,0.250000,0.802000,0.104000,0.104000,1.000000
```
Export-only for v1; import is an explicitly out-of-scope future follow-on. Trigger via
a `PF_ADD_BUTTON` handled in `PF_Cmd_USER_CHANGED_PARAM`, invoking a native
`NSSavePanel` isolated in a small `.mm` file (`CSVExport.mm`) behind a plain C++
function signature so the rest of the codebase stays AppKit-free.

## Open questions

The SDK (`AfterEffectsSDK_26.5_MacOS`) is now vendored at the project root. Two of the
original open questions are resolved by direct header inspection; the rest still need
resolving against real behaviour/testing.

1. **RESOLVED.** Working-colour-space query: `AEGP_ColorSettingsSuite6`
   (`kAEGPColorSettingsSuite` = `"PF Color Settings Suite"`,
   `kAEGPColorSettingsSuiteVersion6`, frozen AE 25.1) is declared in
   `AfterEffectsSDK/Examples/Headers/AE_GeneralPlug.h:3432`. Key entry points:
   - `AEGP_GetNewWorkingSpaceColorProfile(plugin_id, compH, &profile)` — the actual ICC
     working-space profile for a comp; `AEGP_GetColorProfileApproximateGamma(profile,
     &gamma)` gives an approximate gamma for linearization.
   - `AEGP_IsOCIOColorManagementUsed` / `AEGPD_GetOCIOWorkingColorSpace` — AE 25+ can use
     OCIO instead of classic ICC; branch on this before assuming an ICC profile exists.
   - `AEGPD_IsColorSpaceAwareEffectsEnabled` — a project-level toggle; if off, treat the
     project as legacy/non-colour-managed (fall back to the sRGB stand-in).
   - `AEGPD_GetWorkingColorSpaceId` — a GUID identifying the working space, useful for
     cache-key purposes (see Open Question 2).
   - The vendored `Examples/Util/AEGP_SuiteHandler.h` convenience wrapper only goes up to
     `ColorSettingsSuite4` — acquire `ColorSettingsSuite6` directly via
     `AEFX_AcquireSuite`/`AEFX_SuiteScoper`, not through `AEGP_SuiteHandler`.
   - No SDK example exercises this suite, so the exact call sequence (which AEGP suite
     bridges a `PF_InData::effect_ref` to a `compH`, as `SmartyPants.cpp`'s `PreRender`
     does via `AEGP_PFInterfaceSuite1`/`AEGP_LayerSuite8`) still needs to be written and
     tested against a real project with a non-default working space before Phase 2's
     `WorkingSpaceTransform` can be upgraded from the Phase 1 sRGB stand-in.
2. Whether querying working-space via the AEGP-from-PF bridge needs manual
   cache-invalidation (e.g. folding `AEGPD_GetWorkingColorSpaceId`'s GUID into data that
   participates in `COMPARE_FUNC`) so a mid-session project colour-settings change
   doesn't leave stale renders cached. Still unresolved — needs empirical testing.
3. **RESOLVED.** AE's `PF_EffectWorld`/`PF_LayerDef` pixel buffers delivered to effects
   are **straight (un-premultiplied) alpha**. Evidence: `PF_RenderRequest::
   preserve_rgb_of_zero_alpha` (`AE_Effect.h:2514`) is documented as "whether the effect
   should attempt to preserve RGB when A=0" — meaningful only if RGB and A are stored
   independently; under premultiplied storage RGB is mathematically forced to 0 wherever
   A=0, so "preserving" it would be a no-op. `PF_ModeFlags`/`PF_ALPHA_PREMUL`/
   `PF_ALPHA_STRAIGHT` (`AE_Effect.h:391-399`) only ever appear as *parameters to*
   explicit `premultiply`/`unmultiply` utility callbacks (`AE_EffectCB.h:722-730`) that
   an effect calls if it needs the other representation for its own math — they don't
   describe an incoming/negotiable buffer format. `GradientRemap_Main.cpp`'s `PreRender`
   sets `req.preserve_rgb_of_zero_alpha = TRUE` accordingly.
4. Full mandatory `PF_ADD_ARBITRARY2` callback set for this SDK version (confirm against
   `PF_ArbitraryData.h`/`AE_Effect.h`'s arbitrary-data section; watch for newer callbacks
   such as a possible `GET_RANDOM_SEED` requirement) — not yet needed until Phase 3.
5. How to invoke a colour picker from a custom-UI double-click event — likely either a
   direct `PF_AppSuite` dialog call or a "hidden helper `PF_ADD_COLOR` param" trick —
   deferred to Phase 3.
6. Minimum supportable AE version (max of whatever versions introduced
   `PF_ADD_ARBITRARY2`, Drawbot, and `ColorSettingsSuite6`, which is itself frozen at
   AE 25.1) — the working-space suite alone likely puts the floor at AE 25.1+.
7. `COMPARE_FUNC`/keyframe-`INTERP_FUNC` semantics when knots are reordered — whether
   stable per-knot IDs are needed beyond a memberwise struct compare — deferred to
   Phase 3.
8. Re-derive the OKLab conversion matrices for the actual working-space primaries if
   Open Question 1's investigation reveals a non-Rec.709 working space is common enough
   to matter.

## Build notes: PiPL resources via Rez (Phase 0/2 finding)

Xcode's native "Resources" build phase compiles a PiPL `.r` file with `/usr/bin/Rez`
into a flat `Contents/Resources/<Name>.rsrc` file inside the bundle — verified by
building the SDK's own `Skeleton` sample directly with `xcodebuild` (BUILD SUCCEEDED,
universal arm64/x86_64). `CMakeLists.txt` replicates this as an explicit `POST_BUILD`
custom command so a plain `cmake --build` (no `-G Xcode` needed) produces a loadable
plugin.

**Gotcha:** `Rez` defaults to writing into the classic HFS+ **resource fork** of the
output path, not its data fork — the file appears to exist but is 0 bytes to every
normal tool (`ls`, `cp`, bundle packaging), while the real 674+ bytes sit invisibly at
`<path>/..namedfork/rsrc`. Xcode's own invocation (captured from a real build log) passes
`-useDF -script Roman -define __MACH__ -d SystemSevenOrLater=1`; `-useDF` is the flag
that matters here. Always pass it when invoking `Rez` outside of Xcode's build system.

## Build notes: PiPL/code consistency checks (Phase 2 finding)

AE cross-checks three PiPL fields against what `GlobalSetup()` actually sets at
runtime and throws a non-fatal dialog (effect still loads) on any mismatch:
`AE_Effect_Version` vs. `out_data->my_version`, and `AE_Effect_Global_OutFlags`/
`_2` vs. `out_data->out_flags`/`out_flags2`. These are **not** simple values you can
eyeball — they're bit-packed. `AE_Effect_Version` must equal `PF_VERSION(major,
minor, bug, stage, build)`'s exact packed bit layout (see `PF_VERSION` macro in
`AE_Effect.h:142`); the two out-flags fields must equal the OR of every individual
`PF_OutFlag_*`/`PF_OutFlag2_*` enum bit set in code. `GradientRemapPiPL.r` now carries
the correct computed literals with comments pinning them to the exact `GlobalSetup()`
flags they must track — **whenever `GlobalSetup()`'s flags or `GradientRemap.h`'s
version numbers change, these three PiPL literals must be recomputed and updated
together**, or AE will show the same mismatch dialogs again.

## Phase 2 status

`GradientRemap/src/plugin/GradientRemap_Main.cpp` implements the Phase 2 minimal
integration: a fixed 4-knot stock-param UI (`PF_ADD_COLOR` + `PF_ADD_FLOAT_SLIDER` per
knot, luma-formula and interpolation-mode popups, clamp/remap-alpha checkboxes) driving
`GradientRemapCore::EvaluateGradient` through real `PF_Cmd_SMART_PRE_RENDER`/
`PF_Cmd_SMART_RENDER`, dispatching to `Iterate8Suite2`/`Iterate16Suite2`/
`IterateFloatSuite2` by pixel format. Builds cleanly and produces a well-formed
`GradientRemap.plugin` bundle (confirmed: valid Mach-O bundle, non-empty PiPL resource,
correct Info.plist/PkgInfo). Reference pattern for the SmartFX PreRender/SmartRender
structure was `AfterEffectsSDK/Examples/Effect/SmartyPants/SmartyPants.cpp`.

**Bug found and fixed (2026-09-25):** the effect rendered solid black regardless of
parameters. Root cause: `ActuallyRender`'s three `iterate_origin` calls passed
`&in_data->extent_hint` as the iteration `area`. That field is documented as "full
resolution width/height of source layer" / "intersection of input and output extents"
on `PF_InData`, but it's populated for the classic `PF_Cmd_RENDER` path and is not
reliable during `PF_Cmd_SMART_RENDER` — it read as an empty/degenerate rect, so
`iterate_origin` processed zero pixels and the output buffer was left at whatever
`checkout_output` handed back (reads as solid black), regardless of what the pixel
callback would have computed. Fix: pass `NULL` for `area`, which the
`PF_Iterate*Suite2` headers explicitly document as meaning "all pixels"
(`AE_EffectCBSuites.h`). Also added a defensive `PF_COPY` passthrough in `SmartRender`
for the case where `gradient.IsValid()` ever fails, so a future bug of this shape
degrades to "effect does nothing" instead of "solid black" — much faster to diagnose.

## Phase 2 polish (2026-09-25)

Closed out three of the four known Phase 2 simplifications:

- **Real working-colour-space query, wired in.** `GradientRemap_ColorSpace.h/.cpp`
  implements `GradientRemap_RegisterWithAEGP` (called once from `GlobalSetup`, via
  `AEGP_UtilitySuite3::AEGP_RegisterWithAEGP`, storing the returned `AEGP_PluginID` in a
  file-scope global — the pattern used by the SDK's own `HistoGrid.cpp`) and
  `GradientRemap_QueryWorkingSpaceGamma` (called once per `SmartRender`, bridging
  `effect_ref` → `AEGP_LayerH` → `AEGP_CompH` via `AEGP_PFInterfaceSuite1`/
  `AEGP_LayerSuite8` exactly as `SmartyPants.cpp`'s `PreRender` does, then querying
  `AEGP_ColorSettingsSuite6` — acquired directly via `AEFX_SuiteScoper`, bypassing
  `AEGP_SuiteHandler`'s convenience wrapper which only goes up to v4). Returns 0
  (→ precise sRGB curve) whenever the host is Premiere, the project isn't
  colour-space-aware (`AEGPD_IsColorSpaceAwareEffectsEnabled`), or the project uses OCIO
  (`AEGP_IsOCIOColorManagementUsed` — OCIO transform math is still unimplemented and
  explicitly out of scope here); otherwise returns `AEGP_GetColorProfileApproximateGamma`
  of the comp's working-space profile (`AEGP_GetNewWorkingSpaceColorProfile`).
  - **Core API change**: `GradientRemapCore`'s `WorkingSpaceTransform::ToLinear/
    ToWorkingSpace` and `EvaluateGradient` now take an optional `gamma` parameter
    (default `0.0f` = the original precise sRGB curve, so every existing Phase 1 test
    still passes unchanged). `gamma > 0` uses a plain power-law curve instead. This is a
    parameter, not global mutable state, specifically because `PF_OutFlag2_
    SUPPORTS_THREADED_RENDERING` means concurrent renders (potentially of different
    comps with different working spaces) must not share a "current transform".
  - **Known remaining simplification**: a plain approximate-gamma power curve is a
    much cruder model than the real ICC profile (no primaries/matrix, no LUT) — good
    enough to track roughly-2.2-vs-linear-ish differences, not a colour-accurate
    pipeline. Full ICC-profile-based transforms would be a substantially larger effort.
  - New test: `TestWorkingSpaceGammaParameter` in `GradientRemap/src/tests/main.cpp`
    guards that a nonzero gamma still preserves WYSIWYG endpoints exactly while
    actually changing the interior blend.
- **8bpc dithering added.** `GradientRemap_Main.cpp`'s `RemapPixel8` applies a
  stateless 8x8 Bayer ordered-dither offset (from `(x,y)` only) before quantizing,
  gated behind the new "Dither (8bpc)" checkbox; 16/32bpc paths are untouched (enough
  precision that banding isn't a concern).

Still open:
- Out-of-range extrapolation (`EvaluateGradientExtrapolated` in
  `GradientRemap_Main.cpp`) is a simple linear extension in raw working-space channel
  values from the boundary knot pair, independent of interpolation mode/gamma — a
  deliberate plugin-layer simplification, not run through OKLab/linear-light for the
  extrapolated region.

## UI/design changes from user feedback (2026-09-25)

- **Luma Formula removed from the UI, hardcoded to Rec.709.** The 709/601/average
  choice added no real value and worked against "colour space handled without
  intervention" (601 assumes SD/NTSC primaries, average is perceptually arbitrary —
  neither is a better match for a modern sRGB/Rec.709-primaries working space than
  709 itself). `ComputeLuma` in `GradientRemap_Main.cpp` now hardcodes Rec.709
  coefficients with no formula parameter at all.
- **"Clamp Input to [0,1]" renamed to "Clamp Range (32bpc)"** to make clear it's a
  32bpc-only control (per the original brief: 8/16bpc are inherently clamped).
- **"Remap Alpha" checkbox removed** (source alpha now always passes through
  unchanged). The locked-in design for alpha remapping is a **second, independent
  alpha-knot gradient** — not a toggle on the colour gradient's own alpha. It remaps
  the *same* source luminance through its own knot list to produce a new alpha value,
  which is then **multiplied into the existing source alpha** (not replacing it).
  Visually it's drawn on the **opposite side of the gradient bar from the colour
  knots** (colour below, alpha above), matching Photoshop's gradient editor convention.
  This only makes sense with the real Phase 3 gradient-bar UI (there's no "opposite
  side of a bar" in the current fixed stock-param UI), so it's deferred there rather
  than built as more Phase 2 throwaway UI — Phase 3's data model should carry two
  independent `GradientData`-shaped knot lists (or a single struct with two knot
  vectors) from the start.
- **Dithering bug found and fixed: dithering the wrong value.** The first
  implementation added the Bayer dither offset to the *final output* RGB right before
  quantizing — the textbook approach, but it doesn't help here because the *input* is
  itself 8bpc: t only takes ~256 distinct values across a frame, and wherever the
  gradient's local slope between two knots is steep, a single 1-LSB step in input luma
  can jump *several* LSBs in output colour. That's a real step in the transfer curve,
  not rounding noise, and dithering only the output has no intermediate lookup values
  to dither between — it just adds jitter within each already-visible band without
  merging them. Confirmed visually by the user (dithering had "some effect" but didn't
  break up the bands). Fix: dither **t itself**, before the gradient lookup (8bpc path
  only, in `RemapPixel8`) — neighbouring pixels sharing the same nominal 8-bit source
  luma now sample slightly different points along the transfer curve, spreading a
  multi-LSB jump into a smooth-looking dithered transition instead of a hard edge. This
  is analogous to "shaper" dithering used before 3D LUT application in colour pipelines.
  **Root cause found (2026-09-25): the problem was never our maths, it was upstream.**
  Two dithering amplitudes were tried (a "textbook" +/-0.5 input-LSB, then an
  empirical 6x-stronger version) and neither visibly broke up the banding. The
  breakthrough came from a user-run diagnostic: viewing the **raw, un-remapped source**
  (the Shape Layer's own "Gradient Fill", no effect applied) in Photoshop showed the
  *same* hard banding as our effect's output, and a small Photoshop blur trivially
  smoothed it. That rules out our transfer-curve math entirely — **AE's own gradient
  rendering is coarser than the theoretical 8-bit limit** at 8bpc project depth (16bpc
  is smooth, confirming it's a real precision issue, not a display/viewer artifact).
  No amount of *pure per-pixel dithering* can fix this: dithering only jitters the ONE
  sample a pixel already has, and can't discover that a real neighbouring pixel holds
  different information. A real spatial blur can, because it reads genuinely different
  neighbouring pixels — exactly what the user's manual blur test proved.
  - **Fix**: `RemapPixel8` now reads real neighbouring source pixels (a 5x5, radius-2
    box blur of Rec.709 luma, via direct `PF_EffectWorld` buffer access with edge
    clamping — see `SampleLuma8Clamped`/`BlurredLuma8` in `GradientRemap_Main.cpp`)
    before the gradient lookup, plus a small residual Bayer jitter on top to smooth the
    blur kernel's own step. This is a genuine architecture change from the original
    brief's "sub-LSB offset on the float result" design (which assumes an
    already-continuous signal, wrongly in this case) — a documented, justified
    departure given the empirical root cause.
  - **Tradeoff**: this softens real high-frequency detail in non-gradient source
    content by the blur radius, which is why it stays opt-in behind the same checkbox
    (renamed "Reduce Banding (8bpc)" to reflect what it now actually does) rather than
    always-on.
  - **Not yet re-confirmed by the user** — needs a fresh install/test pass.

**Note:** removing/reordering stock params changes their ordinal indices, which is how
AE matches saved effect parameter values on reload — any `.aep` with the effect already
applied from before this change will likely show scrambled parameter values (or a
mismatch warning) and should have the effect removed and re-applied fresh.

## Anti-banding blur: hold colour/alpha at shape edges (2026-09-25)

User feedback: the box blur (above) was bleeding transparent/background colour into
pixels near a shape's alpha edge, visually softening the shape's outline even though
alpha itself was never touched. Fixed by making the blur **alpha-weighted**: each
neighbour's luma contribution is scaled by that neighbour's own alpha
(`SampleLumaAlpha8Clamped`/`BlurredLuma8` in `GradientRemap_Main.cpp`), so transparent
neighbours contribute ~0 weight and the blur near an edge effectively only averages
with other nearby opaque pixels, pulling toward the interior rather than the empty area
outside. Falls back to the unblurred centre luma if the whole neighbourhood is
transparent.

## Interpolation path (Cinema 4D-style), separate from colour space (2026-09-25)

User feedback identified two genuinely orthogonal axes that "Interpolation" had been
conflating: what colour **space** a blend happens in (Naive/LinearLight/OKLCH — this
control's actual job) versus what **path/shape** the parametric position takes between
knots (previously always a hard piecewise-linear lerp). The latter is modelled on
Cinema 4D's gradient path options (a named subset: **Blend** and **Cubic Bias** are not
implemented). New `InterpPath` enum in `GradientData.h`: `Linear` (unchanged prior
behaviour), `Step` (no blend — hold the segment's starting knot across the whole
segment), `Smooth` (smoothstep ease within each segment only), `Cubic` (Catmull-Rom
spline through *all* knots — default per user preference: "gives the smoothest path
through multiple knots, less plateau at each knot").

**Why Cubic matters, precisely**: piecewise-linear segments (or segments eased
independently of their neighbours, i.e. Smooth) are only *value*-continuous at each
knot — the *derivative* can jump discontinuously from one segment's slope to the
next's. Human vision is very sensitive to exactly this kind of discontinuity (Mach
banding), which is what the user was seeing as "visible ridges." Catmull-Rom gives a
continuous (non-zero) derivative across every interior knot, removing the artifact at
its source. `GradientRemapCoreTests`' `TestPathCubicReducesRidgeAtInteriorKnot`
demonstrates this numerically: at a 3-knot gradient's interior knot with deliberately
mismatched segment slopes, Linear's one-sided-derivative discontinuity is ~25x larger
than Cubic's.

**Implementation notes:**
- `GradientData` format bumped to `kVersion = 2` (adds the `path` byte) — safe with no
  migration path since Phase 2's stock-param UI never actually flattens/unflattens
  `GradientData` through AE (only `GradientRemapCoreTests` exercises `Flatten`/
  `Unflatten` today); this will matter once Phase 3's `PF_ADD_ARBITRARY2` persists it.
- Cubic needs 4 knots (the bracketing pair plus one neighbour on each side), using
  **reflection** (`2*near - far`) for the phantom point at a gradient boundary rather
  than the more common "duplicate the endpoint" rule. Reflection is what makes a plain
  2-knot gradient's Cubic path degenerate to *exactly* Linear (proven algebraically and
  guarded by `TestPathCubicDegeneratesToLinearFor2Knots`) — duplication would introduce
  a half-magnitude tangent and visibly differ from Linear even with nothing to smooth.
- **Bug caught before shipping**: an initial version reflected the boundary phantom
  point in raw working-space RGB and *then* converted to the target space (linear-light
  or OKLCH). That breaks the degenerate-2-knot guarantee for OKLCH specifically, because
  RGB-space reflection and OKLCH-space reflection are different operations (OKLab's
  transform is nonlinear) — caught immediately by
  `TestPathCubicDegeneratesToLinearFor2Knots` failing after the path feature landed.
  Fixed by reflecting in whichever space each mode actually interpolates in (raw RGB for
  Naive, linear RGB for LinearLight, OKLCH L/C/h for OKLCH — with hue reflected in the
  same pairwise-unwrapped frame as the real endpoints).
- Uses standard **uniform** Catmull-Rom parameterisation (ignoring actual knot spacing)
  — a known simplification; very unevenly spaced knots could show mild overshoot that a
  non-uniform/centripetal parameterisation would avoid. Not yet needed in practice.
- New UI param: "Path" popup (`GRADREMAP_PATH`), items "Cubic|Ease|Linear|Step",
  default Cubic, alongside the existing "Interpolation" popup in `ParamsSetup`.

### Bug found and fixed: OKLCH+Cubic hue overshoot ("cyan blip") (2026-09-25)

User-reported: a magenta→green→yellow gradient under OKLCH+Cubic showed an unexpected
cyan intrusion on the green→yellow segment, where the colour should simply move from
green toward yellow. Switching to Ease (no cross-segment spline) fixed it, correctly
pointing at Cubic's spline maths as the cause.

**Root cause**: plain Catmull-Rom computes the tangent (derivative) at an interior knot
as the *average of the secants on both sides* (`0.5*(next - prev)`). Green's hue sits at
a local extremum in this 3-knot sequence — hue rises sharply coming from magenta, then
must fall going to yellow — so the two adjacent secants have opposite sign, and their
average tangent points the *wrong* way (toward higher/cyan-ward hue) instead of toward
zero. The cubic dutifully follows that tangent before curving back down to yellow,
producing a real, mathematically-expected overshoot past green into cyan. This is a
well-known Catmull-Rom failure mode at local extrema, not specific to hue or to this
codebase — it would show up on any channel (R, G, B, L, C too) wherever three
consecutive knot values aren't monotonic.

**Fix**: replaced the raw Catmull-Rom tangent with a **monotone-safe tangent**
(Fritsch-Carlson style): if the two secants adjacent to a knot disagree in sign (a local
extremum), clamp that knot's tangent to zero instead of averaging; otherwise use the
ordinary Catmull-Rom average unchanged. Implemented as `MonotoneTangent` +
`CubicHermiteScalar` (`Interpolation.cpp`), with `MonotoneCubicScalar` as a drop-in
replacement for the old `CatmullRomScalar` at every call site (Naive/LinearLight/OKLCH
cubic blends all use it uniformly, not just hue — the same overshoot risk applies to
every channel). This is a pure generalization: `CubicHermiteScalar(p1, m1, p2, m2, t)`
with the *unclamped* tangents (`m1=0.5*(p2-p0)`, `m2=0.5*(p3-p1)`) is algebraically
identical to plain Catmull-Rom, so well-behaved (monotonic) knot runs are completely
unaffected — verified by `TestPathCubicDegeneratesToLinearFor2Knots` and
`TestPathCubicReducesRidgeAtInteriorKnot` both still passing with identical numbers.

New regression test `TestPathCubicOKLCHNoHueOvershoot` reproduces the reported
magenta/green/yellow case and asserts hue never leaves the `[green, yellow]` range on
that segment. Verified the test actually catches the bug (not just trivially green) by
temporarily disabling the monotone clamp and confirming the test fails with a real
0.017 rad overshoot in the reported direction, then restoring the fix.

### "Smooth" renamed to "Ease"; reordered second (2026-09-25)

User feedback: "Smooth" was a misnomer — Cubic is smoother overall (continuous
derivative across all knots), while this option only eases within one segment
(zero-derivative at every knot, same "plateau" property as After Effects' own "Easy
Ease" keyframe assistant when applied to consecutive keyframes — which is exactly the
analogy the new name is meant to evoke). Renamed `InterpPath::Smooth` to
`InterpPath::Ease` (a pure identifier rename — the enum's underlying integer value is
unchanged at 2, so `GradientData`'s flatten format is unaffected) and moved it to
second position in the UI popup, after Cubic: **Cubic | Ease | Linear | Step**.

## Threshold-gated debanding blur: don't soften genuine hard transitions (2026-09-25)

User feedback: the anti-banding blur (added to fix real 8-bit source quantization
banding) was also softening **intentional** hard transitions — Step's edges, and in
principle any case where two knots sit close together with strongly contrasting
colours. Blurring is appropriate for the former (spreading a spurious source-quantization
jump across neighbouring pixels) but wrong for the latter (averaging across a
genuinely different, author-intended transition).

**Fix**: `BlurredLuma8` now also reports the (alpha-gated) min/max luma actually
sampled in the blur neighbourhood. `RemapPixel8` evaluates the gradient at both
extremes and measures the resulting colour distance (`colorJump`, max abs
per-channel difference in working-space [0,1]). A new "Debanding Threshold" float
slider (0–1, default 0.5 — directly the value the user suggested as an example) gates
this: `colorJump` well below the threshold blurs fully; at/above it, the blur weight
ramps smoothly to 0 via a smoothstep falloff (avoiding a visible on/off seam of its
own), so the blurred luma degrades back to the unblurred single-pixel value. Real
banding artifacts (a few output LSBs from a coarse-but-gentle-slope source region)
stay comfortably under the default threshold and keep being smoothed; Step's edges and
closely-packed contrasting knots (colour jumps approaching full-scale) fall at or above
it and are left untouched.
**Confirmed working end-to-end in real After Effects (2026-09-25)**, after fixing the
PiPL version/out-flags mismatches and the `extent_hint`/black-output bug documented
above. The user applied the effect to a layer and confirmed the gradient remap renders
correctly and responds to parameter changes.

## Building and testing (Phase 1, today)

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`CoreDumpStrips` writes `.ppm`/`.csv` gradient strips per interpolation mode to
`build/test_output/` for visual inspection (open the `.ppm` files in Preview).
