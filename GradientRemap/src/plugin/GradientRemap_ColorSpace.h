#pragma once

#include "GradientRemap.h"

// AEGP_ColorSettingsSuite6-based query of AE's actual project working colour space,
// used to derive a real approximate gamma for GradientRemapCore's WorkingSpaceTransform
// instead of the Phase 1 hardcoded sRGB stand-in. See docs/DESIGN.md's "Open questions"
// (#1, RESOLVED) for the suite/function research this is built from.
//
// Must be called once during PF_Cmd_GLOBAL_SETUP before any working-space query works.
void GradientRemap_RegisterWithAEGP(PF_InData* in_data);

// Returns AE's queried working-space approximate gamma for the comp this effect is
// applied in, or 0.0f (meaning "use the precise sRGB curve") if:
//  - the host doesn't support AEGP suites (e.g. Premiere Pro),
//  - AEGPD_IsColorSpaceAwareEffectsEnabled reports the project isn't colour-managed,
//  - the project uses OCIO instead of a classic ICC profile (OCIO transform math is a
//    separate, unimplemented feature -- see docs/DESIGN.md open question 1), or
//  - any AEGP call along the way fails.
// Safe to call once per render (not once per pixel) and thread-safe: it returns a
// plain value, touching no shared mutable state, so concurrent renders of different
// comps under PF_OutFlag2_SUPPORTS_THREADED_RENDERING each get their own correct answer.
float GradientRemap_QueryWorkingSpaceGamma(PF_InData* in_data);
