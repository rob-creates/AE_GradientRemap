cmake_minimum_required(VERSION 3.20)
project(GradientRemap CXX C)

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

    set_target_properties(GradientRemapPlugin PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        OUTPUT_NAME "GradientRemap"
        XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
    )

    # Xcode's native Resources build phase compiles a PiPL .r file with Rez into a
    # flat Contents/Resources/<Name>.rsrc file (verified empirically by building the
    # SDK's own Skeleton sample -- see docs/DESIGN.md Phase 0 notes). Replicated here
    # as an explicit POST_BUILD step so a plain `cmake --build` produces a loadable
    # plugin without requiring the Xcode generator.
    find_program(REZ_EXECUTABLE Rez PATHS /usr/bin)
    if(REZ_EXECUTABLE)
        add_custom_command(TARGET GradientRemapPlugin POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:GradientRemapPlugin>/Resources"
            COMMAND ${REZ_EXECUTABLE}
                    -useDF -script Roman -define __MACH__ -d SystemSevenOrLater=1
                    -i "${AE_SDK_ROOT}/Headers"
                    -i "${AE_SDK_ROOT}/Headers/SP"
                    -i "${AE_SDK_ROOT}/Resources"
                    -o "$<TARGET_BUNDLE_CONTENT_DIR:GradientRemapPlugin>/Resources/GradientRemap.rsrc"
                    "${CMAKE_SOURCE_DIR}/GradientRemap/Resources/GradientRemapPiPL.r"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${CMAKE_SOURCE_DIR}/GradientRemap/Resources/GradientRemap-Info.plist"
                    "$<TARGET_BUNDLE_CONTENT_DIR:GradientRemapPlugin>/Info.plist"
            COMMAND /bin/sh -c "printf 'eFKTFXTC' > \"$<TARGET_BUNDLE_CONTENT_DIR:GradientRemapPlugin>/PkgInfo\""
            COMMENT "Compiling PiPL resource with Rez, writing Info.plist/PkgInfo"
            VERBATIM
        )
    else()
        message(WARNING "Rez not found -- GradientRemap.plugin will be missing its PiPL resource and won't load in AE.")
    endif()
else()
    message(STATUS "AfterEffectsSDK/ not found -- skipping plugin target (core library + tests still build/run).")
endif()
