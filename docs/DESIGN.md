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

Known Phase 2 simplifications, to close out before calling Phase 2 done:
- `WorkingSpaceTransform` still uses the Phase 1 sRGB EOTF stand-in (Open Question 1's
  suite is identified but not yet wired in).
- Stock `PF_ADD_COLOR` knots have no alpha channel, so knot alpha is always 1.0 (real
  per-knot alpha arrives with Phase 3's arbitrary-data knot list).
- No dithering yet (deferred to keep this validation pass bounded).
- Out-of-range extrapolation (`EvaluateGradientExtrapolated` in
  `GradientRemap_Main.cpp`) is a simple linear extension in raw working-space channel
  values from the boundary knot pair, independent of interpolation mode — a deliberate
  plugin-layer simplification, not run through OKLab/linear-light for the extrapolated
  region.
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
