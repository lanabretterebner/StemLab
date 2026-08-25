cmake_minimum_required(VERSION 3.22)

project(StemLabPlugin VERSION 0.9.9 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE 9.0.0 is pinned for reproducible builds.
#
# Normal Windows builds download/extract JUCE in scripts/build_plugin.ps1 and pass
# STEMLAB_JUCE_SOURCE_DIR here. This avoids CMake's Git/FetchContent clone
# sub-build, which is fragile on fresh Windows machines.
set(STEMLAB_JUCE_SOURCE_DIR "" CACHE PATH "Path to an extracted JUCE 9.0.0 source tree")

if (STEMLAB_JUCE_SOURCE_DIR AND EXISTS "${STEMLAB_JUCE_SOURCE_DIR}/CMakeLists.txt")
    message(STATUS "Using local JUCE source: ${STEMLAB_JUCE_SOURCE_DIR}")
    add_subdirectory(
        "${STEMLAB_JUCE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/juce"
    )
    set(STEMLAB_JUCE_ROOT "${STEMLAB_JUCE_SOURCE_DIR}")
else()
    include(FetchContent)

    message(STATUS "No local JUCE source supplied; falling back to JUCE archive download.")

    FetchContent_Declare(
        JUCE
        URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    FetchContent_MakeAvailable(JUCE)
    set(STEMLAB_JUCE_ROOT "${juce_SOURCE_DIR}")
endif()

# The REAPER host integration queries IReaperHostApplication, which needs the
# Steinberg base headers JUCE already ships. Only ReaperHost.cpp includes
# them. JUCE 9 keeps the SDK under juce_audio_processors_headless; older
# layouts under juce_audio_processors.
set(STEMLAB_VST3_SDK_DIR "")

foreach(_stemlab_sdk_candidate
    "${STEMLAB_JUCE_ROOT}/modules/juce_audio_processors_headless/format_types/VST3_SDK"
    "${STEMLAB_JUCE_ROOT}/modules/juce_audio_processors/format_types/VST3_SDK")
    if (NOT STEMLAB_VST3_SDK_DIR
        AND EXISTS "${_stemlab_sdk_candidate}/pluginterfaces/base/funknown.h")
        set(STEMLAB_VST3_SDK_DIR "${_stemlab_sdk_candidate}")
    endif()
endforeach()

if (NOT STEMLAB_VST3_SDK_DIR)
    message(FATAL_ERROR
        "Could not find the VST3 base headers inside the JUCE source tree at "
        "${STEMLAB_JUCE_ROOT}")
endif()

juce_add_plugin(StemLabPlugin
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/StemLabIcon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/StemLabIcon.png"
    COMPANY_NAME "StemLab"
    BUNDLE_ID "org.stemlab.audio"
    PLUGIN_MANUFACTURER_CODE StLb
    PLUGIN_CODE SlA1
    FORMATS VST3 Standalone
    PRODUCT_NAME "StemLab"
    DESCRIPTION "Capture, separate, and export stems to Ableton Live"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_add_binary_data(StemLabAssets
    SOURCES
        Resources/StemLabIcon.png
)

target_sources(StemLabPlugin
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/StemLabTheme.h
        Source/StemLabLookAndFeel.h
        Source/StemLabPaths.cpp
        Source/StemLabPaths.h
        Source/ReaperHost.cpp
        Source/ReaperHost.h
        Source/ReaperBridge.cpp
        Source/ReaperBridge.h
        Source/LinuxSystemCapture.cpp
        Source/LinuxSystemCapture.h
)

target_include_directories(StemLabPlugin
    PRIVATE
        "${STEMLAB_VST3_SDK_DIR}"
)

# ReaperHost.cpp calls through interfaces that only REAPER implements, outside
# this binary. Under LTO, GCC's whole-program devirtualisation proves those
# interfaces have no implementation anywhere in the plugin and rewrites the
# calls into __cxa_pure_virtual traps - a guaranteed crash the first time the
# bridge runs inside REAPER. Keeping this one file out of LTO leaves its
# virtual calls opaque and therefore intact.
if (NOT MSVC)
    set_source_files_properties(Source/ReaperHost.cpp
        PROPERTIES
            COMPILE_OPTIONS "-fno-lto;-fno-devirtualize"
    )
endif()

target_compile_definitions(StemLabPlugin
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
    PRIVATE
        STEMLAB_DEV_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_link_libraries(StemLabPlugin
    PRIVATE
        StemLabAssets
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if (WIN32)
    target_link_libraries(StemLabPlugin
        PRIVATE
            ole32
            uuid
    )
elseif (UNIX AND NOT APPLE)
    # JUCE already propagates X11/freetype/ALSA through its own targets. What
    # it does not add is the dynamic loader, which juce_audio_devices needs to
    # pick up JACK at runtime, or the thread library on some toolchains.
    find_package(Threads REQUIRED)

    target_link_libraries(StemLabPlugin
        PRIVATE
            Threads::Threads
            ${CMAKE_DL_LIBS}
    )

    # JACK is loaded with dlopen, so enabling it costs nothing at link time and
    # gives Standalone users on PipeWire/JACK a working device.
    target_compile_definitions(StemLabPlugin
        PUBLIC
            JUCE_JACK=1
            JUCE_ALSA=1
    )
endif()

juce_generate_juce_header(StemLabPlugin)
