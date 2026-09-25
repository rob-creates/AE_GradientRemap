#pragma once

#ifndef GradientRemap_H
    #define GradientRemap_H

    #define PF_DEEP_COLOR_AWARE 1 // request 16bpc pixels

    #include "AEConfig.h"
    #include "entry.h"
    #include "AE_Effect.h"
    #include "AE_EffectCB.h"
    #include "AE_Macros.h"
    #include "Param_Utils.h"
    #include "AE_EffectCBSuites.h"
    #include "String_Utils.h"
    #include "AE_GeneralPlug.h"
    #include "Smart_Utils.h"
    #include "AEGP_SuiteHandler.h"
    #include "AEFX_SuiteHelper.h"

    #ifdef AE_OS_WIN
        #include <Windows.h>
    #endif

#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define BUG_VERSION 0
#define STAGE_VERSION PF_Stage_DEVELOP
#define BUILD_VERSION 1

#define NAME "Gradient Remap"
#define DESCRIPTION "Luminance-to-gradient colour remap (Phase 2 validation build)."

// Fixed 4-knot Phase-2 validation UI (see docs/DESIGN.md): a throwaway stock-param
// front end used to exercise GradientRemapCore through real SmartFX render calls
// before the Phase 3 custom multi-knot gradient-bar UI replaces it.
//
// Luma formula is intentionally NOT a user-facing control: per user feedback
// (2026-09-25), exposing 709/601/average as a choice added no real value and worked
// against "colour space handled without intervention" -- Rec.709 relative luminance is
// hardcoded (see ComputeLuma in GradientRemap_Main.cpp).
//
// Alpha remapping is NOT a simple on/off toggle here for the same reason: the intended
// design is a second, independent alpha knot list remapping the same source luminance,
// multiplied into the existing source alpha -- that needs the Phase 3 custom UI to be
// meaningful (conventionally drawn on the opposite side of the gradient bar from the
// colour knots, colour below/alpha above, matching Photoshop's gradient editor). Source
// alpha is passed through unchanged in this Phase 2 UI until then.
enum {
    GRADREMAP_INPUT = 0,
    GRADREMAP_INTERP_MODE,
    GRADREMAP_PATH,
    GRADREMAP_KNOT0_COLOR,
    GRADREMAP_KNOT0_POS,
    GRADREMAP_KNOT1_COLOR,
    GRADREMAP_KNOT1_POS,
    GRADREMAP_KNOT2_COLOR,
    GRADREMAP_KNOT2_POS,
    GRADREMAP_KNOT3_COLOR,
    GRADREMAP_KNOT3_POS,
    GRADREMAP_CLAMP_INPUT,
    GRADREMAP_DITHER,
    GRADREMAP_DEBAND_THRESHOLD,
    GRADREMAP_NUM_PARAMS
};

enum {
    KNOT_DISK_ID = 1,
    INTERP_MODE_DISK_ID,
    PATH_DISK_ID,
    KNOT0_COLOR_DISK_ID,
    KNOT0_POS_DISK_ID,
    KNOT1_COLOR_DISK_ID,
    KNOT1_POS_DISK_ID,
    KNOT2_COLOR_DISK_ID,
    KNOT2_POS_DISK_ID,
    KNOT3_COLOR_DISK_ID,
    KNOT3_POS_DISK_ID,
    CLAMP_INPUT_DISK_ID,
    DITHER_DISK_ID,
    DEBAND_THRESHOLD_DISK_ID
};

enum { InterpModePopup_NAIVE = 1, InterpModePopup_LINEAR_LIGHT, InterpModePopup_OKLCH };

// Subset of Cinema 4D's gradient path options (Blend and Cubic Bias not implemented).
// Cubic is first/default per user preference (2026-09-25): smoothest path through
// multiple knots, least plateau/ridging at each knot. "Ease" (renamed from "Smooth",
// 2026-09-25 -- it isn't smoother than Cubic overall and shouldn't read as if it were)
// is second, ahead of Linear/Step.
enum { InterpPathPopup_CUBIC = 1, InterpPathPopup_EASE, InterpPathPopup_LINEAR, InterpPathPopup_STEP };

extern "C" {
DllExport PF_Err EffectMain(
    PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra);
}

#endif // GradientRemap_H
