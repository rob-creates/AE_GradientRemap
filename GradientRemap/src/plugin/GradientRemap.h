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
    #include "AE_EffectUI.h"
    #include "Smart_Utils.h"
    #include "AEGP_SuiteHandler.h"
    #include "AEFX_SuiteHelper.h"

    #ifdef AE_OS_WIN
        #include <Windows.h>
    #endif

#include "../core/GradientData.h"

#define MAJOR_VERSION 1
#define MINOR_VERSION 0
#define BUG_VERSION 0
#define STAGE_VERSION PF_Stage_DEVELOP
#define BUILD_VERSION 1

#define NAME "Gradient Remap"
#define DESCRIPTION "Luminance-to-gradient colour remap."

// Phase 3 (see docs/DESIGN.md): a real custom multi-knot gradient-bar UI, replacing
// Phase 2's throwaway fixed-4-knot stock-param UI. The knot list lives in a single
// PF_ADD_ARBITRARY2 param (GRADREMAP_GRADIENT); Interpolation/Path stay as separate
// stock popups (simpler, discoverable, and orthogonal to the knot list itself).
//
// Luma formula is intentionally NOT a user-facing control: per user feedback
// (2026-09-25), exposing 709/601/average as a choice added no real value and worked
// against "colour space handled without intervention" -- Rec.709 relative luminance is
// hardcoded (see ComputeLuma in GradientRemap_Main.cpp).
//
// Alpha remapping is still NOT implemented: the intended design is a second,
// independent alpha knot list remapping the same source luminance, multiplied into the
// existing source alpha, drawn on the opposite side of the gradient bar from the colour
// knots (colour below/alpha above, matching Photoshop's gradient editor) -- deferred to
// a future pass. Source alpha passes through unchanged for now.
// Order here MUST match the PF_ADD_* call order in ParamsSetup (GradientRemap_Main.cpp)
// -- params[] is positional, assigned strictly by registration order, not by these
// names. GRADREMAP_GRADIENT listed right after GRADREMAP_INPUT (2026-09-25, user
// request) so the gradient bar is the first visible parameter. Grouped into "Colour" and
// "Motion" twirl-down topics (2026-09-28, user request) -- each topic's start/end marker
// is itself a param and occupies a slot here.
enum {
    GRADREMAP_INPUT = 0,
    GRADREMAP_COLOUR_TOPIC_START,
    GRADREMAP_GRADIENT,
    GRADREMAP_INTERP_MODE,
    GRADREMAP_PATH,
    GRADREMAP_DITHER,
    GRADREMAP_CLAMP_INPUT,
    GRADREMAP_SAVE_BUTTON,
    GRADREMAP_LOAD_BUTTON,
    GRADREMAP_COLOUR_TOPIC_END,
    GRADREMAP_MOTION_TOPIC_START,
    GRADREMAP_OFFSET,
    GRADREMAP_LOOP,
    GRADREMAP_MOTION_TOPIC_END,
    GRADREMAP_NUM_PARAMS
};

enum {
    INTERP_MODE_DISK_ID = 1,
    PATH_DISK_ID,
    GRADIENT_DISK_ID,
    CLAMP_INPUT_DISK_ID,
    DITHER_DISK_ID,
    DEBAND_THRESHOLD_DISK_ID, // retired (2026-09-28): threshold now hardcoded, see kDebandThreshold
    SAVE_BUTTON_DISK_ID,
    LOAD_BUTTON_DISK_ID,
    OFFSET_DISK_ID,
    LOOP_DISK_ID,
    COLOUR_TOPIC_START_DISK_ID,
    COLOUR_TOPIC_END_DISK_ID,
    MOTION_TOPIC_START_DISK_ID,
    MOTION_TOPIC_END_DISK_ID
};

// NOTE (2026-09-25): AE displays this popup as "Colour Space", not "Interpolation" --
// GRADREMAP_INTERP_MODE/INTERP_MODE_DISK_ID/InterpMode (GradientData.h) keep their
// original internal names (still accurate: this picks which colour space the blend math
// happens in) to avoid a large, purely-cosmetic rename through the tested core/
// serialization code. Only the AE-facing label text changed -- see ParamsSetup.
//
// Order matches the "Colour Space" popup string in ParamsSetup, not declaration order:
// OKLCH listed first (2026-09-25, user preference -- handles chroma/hue traversal best)
// with Linear Light pushed last (least commonly needed). Not yet made the *default*
// selection -- OKLCH's perceptually-uniform lightness axis renders a black/white ramp's
// shadow end noticeably darker than Native/Linear Light (confirmed with real numbers,
// see docs/DESIGN.md); still deciding whether that's the right default.
// "Naive" renamed to "Native" (2026-09-25, user feedback -- clearer name for "blend
// directly in the project's own working colour space, no conversion").
enum { InterpModePopup_OKLCH = 1, InterpModePopup_NATIVE, InterpModePopup_LINEAR_LIGHT };

// NOTE (2026-09-25): AE displays this popup as "Interpolation", not "Path" --
// GRADREMAP_PATH/PATH_DISK_ID/InterpPath (GradientData.h) keep their original internal
// names (still accurate: this picks the interpolation path/curve shape between knots)
// to avoid a large, purely-cosmetic rename through the tested core/serialization code.
// Only the AE-facing label text changed -- see ParamsSetup.
//
// Subset of Cinema 4D's gradient path options (Blend and Cubic Bias not implemented).
// Cubic is first/default per user preference (2026-09-25): smoothest path through
// multiple knots, least plateau/ridging at each knot. "Ease" (renamed from "Smooth",
// 2026-09-25 -- it isn't smoother than Cubic overall and shouldn't read as if it were)
// is second, ahead of Linear/Step.
enum { InterpPathPopup_CUBIC = 1, InterpPathPopup_EASE, InterpPathPopup_LINEAR, InterpPathPopup_STEP };

// "Loop" popup (paired with the "Offset" angle dial -- see ApplyOffsetLoop in
// ../core/Interpolation.h). Cycle first/default: at zero offset it's an exact identity.
// Displayed as "Wave" (2026-09-28, user feedback -- AE's own convention); core keeps
// LoopMode::Sine, which still accurately describes the maths.
enum { LoopPopup_CYCLE = 1, LoopPopup_WAVE, LoopPopup_BOUNCE };

// Maps the two popup values above onto GradientData's own interpolation_mode/path
// fields. Shared by GradientRemap_Main.cpp's render-time BuildGradientFromParams and
// GradientRemap_UI.cpp's UI-time BuildGradientForUI, which both need the identical
// mapping applied -- defined in GradientRemap_Main.cpp.
void GradientRemap_ApplyInterpPopups(GradientRemap::GradientData& g, A_long interp_mode_popup_value,
                                      A_long path_popup_value);

// A sentinel checked in every arbitrary-data callback against the refconPV AE hands
// back, matching the SDK's own ColorGrid sample -- cheap sanity check, not meaningful
// data. Any fixed, distinctive bit pattern works; this one is just this project's own.
#define GRADIENT_ARB_REFCON ((void*)0x67524164) // 'gRAd'

// Gradient-bar custom UI geometry (Effect Controls Window), in local pixels.
// kGradientBarWidth is only the *nominal* width hint passed to PF_ADD_ARBITRARY2 --
// actual drawing/hit-testing always uses the real panel width from
// event_extra->effect_win.current_frame, which can differ (panel is resizable).
constexpr A_long kGradientBarWidth = 200;
constexpr A_long kGradientBarHeight = 24;    // the coloured gradient strip itself
constexpr A_long kKnotMarkerHeight = 10;     // downward-pointing triangle, below the bar
constexpr A_long kGradientUIMargin = 4;      // breathing room above the bar
constexpr A_long kGradientUITotalHeight = kGradientUIMargin + kGradientBarHeight + kKnotMarkerHeight + 2;
constexpr float kKnotHitHalfWidth = 6.0f;    // px either side of a knot's x, for hit-testing
constexpr float kKnotMarkerHalfWidth = 5.0f; // px, drawn triangle half-width at its base
constexpr float kKnotDeleteDragDistance = 40.0f; // px away from the bar to mark a dragged knot for deletion

// Double-click detection: AE hands effects PF_DoClickEventInfo::num_clicks, which
// *should* be the SDK-intended signal, but no vendored SDK example exercises it and its
// cross-host reliability is unconfirmed (see docs/DESIGN.md). As a hedge, we also track
// same-knot-clicked-again-quickly manually via sequence data. `when`'s units aren't
// documented -- this threshold is an untested starting guess, flagged for the user to
// verify/tune against real double-click behaviour.
constexpr A_u_long kDoubleClickMaxWhenDelta = 60;

// Transient, UI-thread-only selection/interaction state -- never persisted, never read
// during rendering (UI-thread and render-thread sequence data are genuinely separate
// copies of memory as of AE 13.5; see docs/DESIGN.md). Lifecycle mirrors the SDK's own
// HistoGrid sample (PF_Cmd_SEQUENCE_SETUP/SETDOWN/RESETUP/FLATTEN).
#define GRADIENT_UI_SEQ_MAGIC 0x47524144 // 'GRAD'
struct GradientUISeqData {
    A_long magic;
    A_long selected_knot_index;   // -1 = none selected
    A_long last_click_knot_index; // for manual double-click fallback; -1 = none
    A_u_long last_click_when;
    PF_Boolean delete_pending; // dragged knot currently past the delete threshold
};

// GradientRemap_Arb.cpp: PF_ADD_ARBITRARY2 callback set, and small helpers shared with
// GradientRemap_UI.cpp / GradientRemap_Main.cpp for reading/writing the arb param's
// live PF_Handle, whose bytes ARE GradientRemap::GradientData::Flatten()'s output
// directly (no separate POD mirror struct -- see GradientRemap_Arb.cpp for why).
PF_Err GradientRemap_HandleArbitrary(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                      PF_LayerDef* output, PF_ArbParamsExtra* extra);
GradientRemap::GradientData GradientRemap_UnflattenArbHandle(PF_InData* in_data, PF_Handle arbH);
PF_Err GradientRemap_ReflattenIntoHandle(PF_InData* in_data, const GradientRemap::GradientData& g, PF_Handle* arbHP);
PF_Err GradientRemap_CreateDefaultArbHandle(PF_InData* in_data, PF_Handle* arbHP);

// Reflattens `g` into the GRADREMAP_GRADIENT param's live handle and marks it changed --
// shared by GradientRemap_UI.cpp's click/drag handlers and GradientRemap_SaveLoad.cpp's
// Load button, both of which need to write a mutated gradient back and have AE pick it
// up. Defined in GradientRemap_Arb.cpp, alongside GradientRemap_ReflattenIntoHandle.
PF_Err GradientRemap_WriteGradientAndMarkChanged(PF_InData* in_data, PF_ParamDef* params[],
                                                  const GradientRemap::GradientData& g);

// GradientRemap_UI.cpp: custom UI event handling (PF_Cmd_EVENT) and the transient
// selection-state sequence data lifecycle (PF_Cmd_SEQUENCE_*).
PF_Err GradientRemap_HandleEvent(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output,
                                  PF_EventExtra* extra);
PF_Err GradientRemap_SequenceSetup(PF_InData* in_data, PF_OutData* out_data);
PF_Err GradientRemap_SequenceSetdown(PF_InData* in_data, PF_OutData* out_data);
PF_Err GradientRemap_SequenceResetup(PF_InData* in_data, PF_OutData* out_data);
PF_Err GradientRemap_SequenceFlatten(PF_InData* in_data, PF_OutData* out_data);

// GradientRemap_SaveLoad.cpp: the "Save Gradient..."/"Load Gradient..." buttons
// (GRADREMAP_SAVE_BUTTON/GRADREMAP_LOAD_BUTTON, PF_Param_BUTTON + PF_ParamFlag_SUPERVISE
// -> PF_Cmd_USER_CHANGED_PARAM), exporting/importing the knot list as a plain CSV file
// via native macOS save/open panels (GradientRemap_FileDialog.mm).
PF_Err GradientRemap_HandleUserChangedParam(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                             const PF_UserChangedParamExtra* which_hit);

extern "C" {
DllExport PF_Err EffectMain(
    PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra);
}

#endif // GradientRemap_H
