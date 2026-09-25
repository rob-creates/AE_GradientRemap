#include "GradientRemap_ColorSpace.h"

namespace {
AEGP_PluginID g_gradient_remap_plugin_id = 0;
}

void GradientRemap_RegisterWithAEGP(PF_InData* in_data) {
    if (in_data->appl_id == kAppID_Premiere) {
        return; // AEGP suites aren't supported when hosted in Premiere Pro.
    }
    AEFX_SuiteScoper<AEGP_UtilitySuite3> utility_suite(in_data, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion3);
    if (utility_suite.get()) {
        utility_suite->AEGP_RegisterWithAEGP(NULL, "GradientRemap", &g_gradient_remap_plugin_id);
    }
}

float GradientRemap_QueryWorkingSpaceGamma(PF_InData* in_data) {
    if (in_data->appl_id == kAppID_Premiere || g_gradient_remap_plugin_id == 0) {
        return 0.0f;
    }

    AEFX_SuiteScoper<AEGP_ColorSettingsSuite6> color_suite(in_data, kAEGPColorSettingsSuite, kAEGPColorSettingsSuiteVersion6);
    if (!color_suite.get()) {
        return 0.0f;
    }

    A_Boolean color_space_aware = FALSE;
    if (color_suite->AEGPD_IsColorSpaceAwareEffectsEnabled(g_gradient_remap_plugin_id, &color_space_aware) !=
            A_Err_NONE ||
        !color_space_aware) {
        return 0.0f;
    }

    A_Boolean is_ocio = FALSE;
    if (color_suite->AEGP_IsOCIOColorManagementUsed(g_gradient_remap_plugin_id, &is_ocio) != A_Err_NONE || is_ocio) {
        return 0.0f; // OCIO working spaces aren't handled yet -- see docs/DESIGN.md.
    }

    AEFX_SuiteScoper<AEGP_PFInterfaceSuite1> pf_interface_suite(in_data, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1);
    AEGP_LayerH layerH = NULL;
    if (!pf_interface_suite.get() ||
        pf_interface_suite->AEGP_GetEffectLayer(in_data->effect_ref, &layerH) != A_Err_NONE || !layerH) {
        return 0.0f;
    }

    AEFX_SuiteScoper<AEGP_LayerSuite8> layer_suite(in_data, kAEGPLayerSuite, kAEGPLayerSuiteVersion8);
    AEGP_CompH compH = NULL;
    if (!layer_suite.get() || layer_suite->AEGP_GetLayerParentComp(layerH, &compH) != A_Err_NONE || !compH) {
        return 0.0f;
    }

    AEGP_ColorProfileP profileP = NULL;
    if (color_suite->AEGP_GetNewWorkingSpaceColorProfile(g_gradient_remap_plugin_id, compH, &profileP) !=
            A_Err_NONE ||
        !profileP) {
        return 0.0f;
    }

    A_FpShort gamma = 0.0f;
    A_Err err = color_suite->AEGP_GetColorProfileApproximateGamma(profileP, &gamma);
    color_suite->AEGP_DisposeColorProfile(profileP);

    if (err != A_Err_NONE || gamma <= 0.0f) {
        return 0.0f;
    }
    return static_cast<float>(gamma);
}
