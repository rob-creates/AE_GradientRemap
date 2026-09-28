#include "AEConfig.h"
#include "AE_EffectVers.h"

#ifndef AE_OS_WIN
    #include <AE_General.r>
#endif

resource 'PiPL' (16000) {
    {
        Kind {
            AEEffect
        },
        Name {
            "Gradient Remap"
        },
        Category {
            // Joins AE's built-in "Color Correction" group (US spelling must match exactly).
            // Must also match the category passed to PF_REGISTER_EFFECT_EXT2 in
            // GradientRemap_Main.cpp.
            "Color Correction"
        },
#ifdef AE_OS_WIN
    #if defined(AE_PROC_INTELx64)
        CodeWin64X86 {"EffectMain"},
    #elif defined(AE_PROC_ARM64)
        CodeWinARM64 {"EffectMain"},
    #endif
#elif defined(AE_OS_MAC)
        CodeMacIntel64 {"EffectMain"},
        CodeMacARM64 {"EffectMain"},
#endif
        AE_PiPL_Version {
            2,
            0
        },
        AE_Effect_Spec_Version {
            PF_PLUG_IN_VERSION,
            PF_PLUG_IN_SUBVERS
        },
        AE_Effect_Version {
            // Must equal PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION,
            // STAGE_VERSION, BUILD_VERSION) from GradientRemap.h bit-for-bit (AE
            // cross-checks this against out_data->my_version at GlobalSetup and
            // errors on any mismatch). Currently PF_VERSION(1, 0, 0, PF_Stage_DEVELOP=0, 1).
            524289 /* 1.0 DEVELOP build 1 */
        },
        AE_Effect_Info_Flags {
            0
        },
        AE_Effect_Global_OutFlags {
            // Must equal the OR of every out_flags bit set in GlobalSetup()
            // (AE_Effect.h): PF_OutFlag_DEEP_COLOR_AWARE (1<<25) |
            // PF_OutFlag_PIX_INDEPENDENT (1<<10) | PF_OutFlag_USE_OUTPUT_EXTENT (1<<6) |
            // PF_OutFlag_CUSTOM_UI (1<<15). Phase 3 (2026-09-25): forgetting to mirror
            // GlobalSetup()'s added PF_OutFlag_CUSTOM_UI bit here made the gradient-bar
            // arbitrary-data param's row disappear entirely from the Effect Controls
            // Window (not just fail to draw) -- AE reads the PiPL's declared out_flags,
            // not GlobalSetup()'s actual runtime value, to decide whether to reserve any
            // custom-UI space at all. See docs/DESIGN.md.
            0x02008440
        },
        AE_Effect_Global_OutFlags_2 {
            // Must equal the OR of every out_flags2 bit set in GlobalSetup():
            // PF_OutFlag2_SUPPORTS_THREADED_RENDERING (1<<27) |
            // PF_OutFlag2_FLOAT_COLOR_AWARE (1<<12) | PF_OutFlag2_SUPPORTS_SMART_RENDER (1<<10).
            0x08001400
        },
        AE_Effect_Match_Name {
            // Must match MATCH_NAME in GradientRemap.h. This is how AE identifies the effect
            // in saved projects -- changing it after release orphans existing instances.
            // ("ADBE" prefixes are reserved for Adobe's own effects.)
            "RE Gradient Remap"
        },
        AE_Reserved_Info {
            0
        },
        AE_Effect_Support_URL {
            // Must match SUPPORT_URL in GradientRemap.h. Empty: unsupported freeware.
            ""
        }
    }
};
