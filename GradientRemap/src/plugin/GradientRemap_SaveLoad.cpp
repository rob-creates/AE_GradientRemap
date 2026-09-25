// GradientRemap_SaveLoad.cpp -- the "Save Gradient..."/"Load Gradient..." buttons:
// export/import the knot list as a plain CSV file (see ../core/GradientCSV.h for the
// format) via native macOS save/open panels (GradientRemap_FileDialog.mm).

#include "GradientRemap.h"
#include "GradientRemap_FileDialog.h"

#include "../core/GradientCSV.h"

#include <fstream>
#include <sstream>

using GradientRemap::GradientData;

namespace {

void ShowAlert(PF_InData* in_data, PF_OutData* out_data, const char* message) {
    PF_SPRINTF(out_data->return_msg, "%s", message);
    if (in_data->appl_id != kAppID_Premiere) {
        out_data->out_flags |= PF_OutFlag_DISPLAY_ERROR_MESSAGE;
    }
}

PF_Err HandleSaveButton(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[]) {
    std::string path;
    if (!GradientRemap::ShowSaveCSVPanel(&path)) return PF_Err_NONE; // cancelled

    GradientData g = GradientRemap_UnflattenArbHandle(in_data, params[GRADREMAP_GRADIENT]->u.arb_d.value);
    std::string csv = GradientRemap::GradientKnotsToCSV(g);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << csv;
    if (!file) {
        ShowAlert(in_data, out_data, "Gradient Remap: couldn't write the CSV file -- check the location is writable.");
    }
    return PF_Err_NONE;
}

PF_Err HandleLoadButton(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[]) {
    std::string path;
    if (!GradientRemap::ShowOpenCSVPanel(&path)) return PF_Err_NONE; // cancelled

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ShowAlert(in_data, out_data, "Gradient Remap: couldn't read that file.");
        return PF_Err_NONE;
    }
    std::ostringstream buf;
    buf << file.rdbuf();

    auto parsed = GradientRemap::ParseGradientKnotsCSV(buf.str());
    if (!parsed) {
        ShowAlert(in_data, out_data,
                  "Gradient Remap: not a valid gradient CSV (expected \"position,r,g,b,a\" rows, 2-256 knots).");
        return PF_Err_NONE;
    }

    // interpolation_mode/path are left untouched -- they're separate stock popups, not
    // part of the CSV round-trip (see GradientCSV.h).
    GradientData g = GradientRemap_UnflattenArbHandle(in_data, params[GRADREMAP_GRADIENT]->u.arb_d.value);
    g.knots = *parsed;
    g.SortKnots();

    PF_Err err = GradientRemap_ReflattenIntoHandle(in_data, g, &params[GRADREMAP_GRADIENT]->u.arb_d.value);
    if (!err) {
        params[GRADREMAP_GRADIENT]->uu.change_flags |= PF_ChangeFlag_CHANGED_VALUE;
    }
    return err;
}

} // namespace

PF_Err GradientRemap_HandleUserChangedParam(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                             const PF_UserChangedParamExtra* which_hit) {
    if (which_hit->param_index == GRADREMAP_SAVE_BUTTON) {
        return HandleSaveButton(in_data, out_data, params);
    }
    if (which_hit->param_index == GRADREMAP_LOAD_BUTTON) {
        return HandleLoadButton(in_data, out_data, params);
    }
    return PF_Err_NONE;
}
