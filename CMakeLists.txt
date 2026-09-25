cmake_minimum_required(VERSION 3.20)
project(GradientRemap CXX C)

# OBJCXX is needed for GradientRemap_FileDialog.mm (native macOS NSSavePanel/NSOpenPanel
# behind the Save/Load buttons, see docs/DESIGN.md) -- only enabled on Apple platforms,
# matching this project's existing macOS-only scope (Rez/PiPL bundling assumes macOS
# tooling already).
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- GradientRemapCore -------------------------------------------------------
# AE-independent: no AE SDK headers, no Cocoa/AppKit. Must remain buildable and
# testable standalone so the interpolation math can be validated before any AE
# integration exists (see docs/DESIGN.md, Phase 1).
add_library(GradientRemapCore STATIC
    GradientRemap/src/core/GradientData.h
    GradientRemap/src/core/GradientData.cpp
    GradientRemap/src/core/Interpolation.h
    GradientRemap/src/core/Interpolation.cpp
    GradientRemap/src/core/GradientCSV.h
    GradientRemap/src/core/GradientCSV.cpp
)
target_include_directories(GradientRemapCore PUBLIC GradientRemap/src/core)

# --- GradientRemapCoreTests ---------------------------------------------------
add_executable(GradientRemapCoreTests GradientRemap/src/tests/main.cpp)
target_link_libraries(GradientRemapCoreTests PRIVATE GradientRemapCore)

enable_testing()
add_test(NAME CoreUnitTests COMMAND GradientRemapCoreTests test)
add_test(NAME CoreDumpStrips COMMAND GradientRemapCoreTests dump "${CMAKE_BINARY_DIR}/test_output")

# --- GradientRemapPlugin ------------------------------------------------------
# Phase 2 (docs/DESIGN.md): minimal SmartFX AE plugin wrapping GradientRemapCore
# behind a throwaway fixed 4-knot stock-param UI. Only built once the AE SDK has
# been vendored into AfterEffectsSDK/ alongside this file.
if(EXISTS "${CMAKE_SOURCE_DIR}/AfterEffectsSDK")
    set(AE_SDK_ROOT "${CMAKE_SOURCE_DIR}/AfterEffectsSDK/Examples")

    add_library(GradientRemapPlugin MODULE
        GradientRemap/src/plugin/GradientRemap.h
        GradientRemap/src/plugin/GradientRemap_Main.cpp
        GradientRemap/src/plugin/GradientRemap_Arb.cpp
        GradientRemap/src/plugin/GradientRemap_UI.cpp
        GradientRemap/src/plugin/GradientRemap_SaveLoad.cpp
        GradientRemap/src/plugin/GradientRemap_FileDialog.h
        GradientRemap/src/plugin/GradientRemap_FileDialog.mm
        GradientRemap/src/plugin/GradientRemap_ColorSpace.h
        GradientRemap/src/plugin/GradientRemap_ColorSpace.cpp
        ${AE_SDK_ROOT}/Util/AEGP_SuiteHandler.cpp
        ${AE_SDK_ROOT}/Util/MissingSuiteError.cpp
        ${AE_SDK_ROOT}/Util/Smart_Utils.cpp
        ${AE_SDK_ROOT}/Util/AEFX_SuiteHelper.c
    )
    target_include_directories(GradientRemapPlugin PRIVATE
        ${AE_SDK_ROOT}/Headers
        ${AE_SDK_ROOT}/Headers/SP
        ${AE_SDK_ROOT}/Util
    )
    target_link_libraries(GradientRemapPlugin PRIVATE GradientRemapCore)
    if(APPLE)
        target_link_libraries(GradientRemapPlugin PRIVATE "-framework Cocoa")
    endif()

    set_target_properties(GradientRemapPlugin PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        OUTPUT_NAME "GradientRemap"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
    )

    # Xcode's native Resources build phase compiles a PiPL .r file with Rez into a
    # flat Contents/Resources/<Name>.rsrc file (verified empirically by building the
    # SDK's own Skeleton sample -- see docs/DESIGN.md Phase 0 notes). Replicated here
    # as an explicit step so a plain `cmake --build` produces a loadable plugin without
    # requiring the Xcode generator.
    #
    # This must be an OUTPUT-based custom command with an explicit DEPENDS on the .r/
    # .plist files, not a TARGET POST_BUILD command (which has no DEPENDS and so only
    # ever reruns when the target's own sources force a relink) -- a real bug found
    # 2026-09-25: editing GradientRemapPiPL.r alone (to add a missing out_flags bit)
    # left a stale .rsrc in the built bundle because nothing told CMake the plugin
    # needed rebuilding, and `cmake --build` reported the target as already up to date.
    find_program(REZ_EXECUTABLE Rez PATHS /usr/bin)
    if(REZ_EXECUTABLE)
        set(GRADIENT_REMAP_PIPL_R "${CMAKE_SOURCE_DIR}/GradientRemap/Resources/GradientRemapPiPL.r")
        set(GRADIENT_REMAP_INFO_PLIST "${CMAKE_SOURCE_DIR}/GradientRemap/Resources/GradientRemap-Info.plist")
        set(GRADIENT_REMAP_RSRC_STAMP "${CMAKE_CURRENT_BINARY_DIR}/GradientRemap_resources.stamp")

        add_custom_command(
            OUTPUT "${GRADIENT_REMAP_RSRC_STAMP}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:GradientRemapPlugin>/Resources"
            COMMAND ${REZ_EXECUTABLE}
                    -useDF -script Roman -define __MACH__ -d SystemSevenOrLater=1
                    -i "${AE_SDK_ROOT}/Headers"
                    -i "${AE_SDK_ROOT}/Headers/SP"
                    -i "${AE_SDK_ROOT}/Resources"
                    -o "$<TARGET_BUNDLE_CONTENT_DIR:GradientRemapPlugin>/Resources/GradientRemap.rsrc"
                    "${GRADIENT_REMAP_PIPL_R}"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${GRADIENT_REMAP_INFO_PLIST}"
                    "$<TARGET_BUNDLE_CONTENT_DIR:GradientRemapPlugin>/Info.plist"
            COMMAND /bin/sh -c "printf 'eFKTFXTC' > \"$<TARGET_BUNDLE_CONTENT_DIR:GradientRemapPlugin>/PkgInfo\""
            COMMAND ${CMAKE_COMMAND} -E touch "${GRADIENT_REMAP_RSRC_STAMP}"
            DEPENDS "${GRADIENT_REMAP_PIPL_R}" "${GRADIENT_REMAP_INFO_PLIST}"
            COMMENT "Compiling PiPL resource with Rez, writing Info.plist/PkgInfo"
            VERBATIM
        )
        add_custom_target(GradientRemapResources DEPENDS "${GRADIENT_REMAP_RSRC_STAMP}")
        add_dependencies(GradientRemapPlugin GradientRemapResources)
    else()
        message(WARNING "Rez not found -- GradientRemap.plugin will be missing its PiPL resource and won't load in AE.")
    endif()
else()
    message(STATUS "AfterEffectsSDK/ not found -- skipping plugin target (core library + tests still build/run).")
endif()
