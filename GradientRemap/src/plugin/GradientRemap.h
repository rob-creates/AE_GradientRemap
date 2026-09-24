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
enum {
    GRADREMAP_INPUT = 0,
    GRADREMAP_LUMA_FORMULA,
    GRADREMAP_INTERP_MODE,
    GRADREMAP_KNOT0_COLOR,
    GRADREMAP_KNOT0_POS,
    GRADREMAP_KNOT1_COLOR,
    GRADREMAP_KNOT1_POS,
    GRADREMAP_KNOT2_COLOR,
    GRADREMAP_KNOT2_POS,
    GRADREMAP_KNOT3_COLOR,
    GRADREMAP_KNOT3_POS,
    GRADREMAP_CLAMP_INPUT,
    GRADREMAP_REMAP_ALPHA,
    GRADREMAP_NUM_PARAMS
};

enum {
    KNOT_DISK_ID = 1,
    LUMA_FORMULA_DISK_ID,
    INTERP_MODE_DISK_ID,
    KNOT0_COLOR_DISK_ID,
    KNOT0_POS_DISK_ID,
    KNOT1_COLOR_DISK_ID,
    KNOT1_POS_DISK_ID,
    KNOT2_COLOR_DISK_ID,
    KNOT2_POS_DISK_ID,
    KNOT3_COLOR_DISK_ID,
    KNOT3_POS_DISK_ID,
    CLAMP_INPUT_DISK_ID,
    REMAP_ALPHA_DISK_ID
};

enum { LumaFormula_REC709 = 1, LumaFormula_REC601, LumaFormula_AVERAGE };

enum { InterpModePopup_NAIVE = 1, InterpModePopup_LINEAR_LIGHT, InterpModePopup_OKLCH };

extern "C" {
DllExport PF_Err EffectMain(
    PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra);
}

#endif // GradientRemap_H
