#pragma once

#include <string>

// Native save/open panels for the gradient CSV Save/Load buttons (see
// GradientRemap_SaveLoad.cpp). Implemented only in GradientRemap_FileDialog.mm
// (Objective-C++, macOS-only, using NSSavePanel/NSOpenPanel) -- matches this project's
// existing macOS-only scope. A Windows build would need a common-dialog equivalent
// here before the Save/Load buttons could work there.
//
// Both block synchronously (NSPanel runModal) and are only ever called from
// PF_Cmd_USER_CHANGED_PARAM, which runs on AE's main/UI thread -- the same assumption
// PF_AppColorPickerDialog's synchronous call already relies on elsewhere in this
// plugin.
namespace GradientRemap {

// Returns true and fills *out_path if the user chose a location; false if cancelled.
// *out_path is guaranteed to end in ".csv" (appended if the user didn't type it).
bool ShowSaveCSVPanel(std::string* out_path);

// Returns true and fills *out_path if the user chose a file; false if cancelled.
bool ShowOpenCSVPanel(std::string* out_path);

} // namespace GradientRemap
