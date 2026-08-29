cmake_minimum_required(VERSION 3.22)

# pyproject.toml is the single source of the StemLab version (the release
# workflow refuses a tag that disagrees with it). Read it here so the plugin
# binary - JucePlugin_VersionString, the settings-menu version tag, what the
# host displays - can never drift from a release.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../pyproject.toml" _stemlab_pyproject)
if (NOT _stemlab_pyproject MATCHES "\nversion = \"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
    message(FATAL_ERROR "Could not read a plain X.Y.Z version from pyproject.toml")
endif()
set(STEMLAB_VERSION "${CMAKE_MATCH_1}")
unset(_stemlab_pyproject)

project(StemLabPlugin VERSION ${STEMLAB_VERSION} LANGUAGES C CXX)

# Re-run the configure step when the version bumps.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/../../pyproject.toml")

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
        Resources/fonts/Inter-Regular.ttf
        Resources/fonts/Inter-Medium.ttf
)

target_sources(StemLabPlugin
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/StemLabTheme.h
        Source/StemLabLookAndFeel.h
        Source/StemLabLookAndFeel.cpp
        Source/StemLabWidgets.h
        Source/StemLabWidgets.cpp
        Source/ModelManagerPanel.h
        Source/ModelManagerPanel.cpp
        Source/StemLabPaths.cpp
        Source/StemLabPaths.h
        Source/HostIntegrationPolicy.h
        Source/SelfFileDragGuard.h
        Source/ReaperHost.cpp
        Source/ReaperHost.h
        Source/ReaperBridge.cpp
        Source/ReaperBridge.h
        Source/LinuxSystemCapture.cpp
        Source/LinuxSystemCapture.h
        Source/WaveformGrid.h
        Source/WaveformAnalysis.h
        Source/WaveformCache.cpp
        Source/WaveformCache.h
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

include(CTest)
if (BUILD_TESTING)
    # These suites state their expectations with assert(), which a Release
    # build defines away - leaving tests that prove only that the code runs.
    # Undefining NDEBUG for the test targets alone keeps the library itself
    # optimised exactly as it ships.
    function(stemlab_enable_test_assertions target)
        if (MSVC)
            target_compile_options(${target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${target} PRIVATE -UNDEBUG)
        endif()
    endfunction()

    add_executable(StemLabWaveformGridTests Tests/WaveformGridTests.cpp
                   Source/HostIntegrationPolicy.h Source/WaveformGrid.h)
    target_include_directories(StemLabWaveformGridTests PRIVATE Source)
    target_compile_features(StemLabWaveformGridTests PRIVATE cxx_std_20)
    stemlab_enable_test_assertions(StemLabWaveformGridTests)
    add_test(NAME StemLabWaveformGridTests COMMAND StemLabWaveformGridTests)

    # The spectral analysis is deliberately JUCE-free - FFT included - so the
    # whole of it is exercised here without standing up a plugin.
    add_executable(StemLabWaveformAnalysisTests
        Tests/WaveformAnalysisTests.cpp Source/WaveformAnalysis.h)
    target_include_directories(StemLabWaveformAnalysisTests PRIVATE Source)
    target_compile_features(StemLabWaveformAnalysisTests PRIVATE cxx_std_20)
    stemlab_enable_test_assertions(StemLabWaveformAnalysisTests)
    add_test(NAME StemLabWaveformAnalysisTests COMMAND StemLabWaveformAnalysisTests)

    # The multi-lane loop policy: which ranges merge, where playback jumps.
    add_executable(StemLabLoopRegionsTests
        Tests/LoopRegionsTests.cpp Source/LoopRegions.h)
    target_include_directories(StemLabLoopRegionsTests PRIVATE Source)
    target_compile_features(StemLabLoopRegionsTests PRIVATE cxx_std_20)
    stemlab_enable_test_assertions(StemLabLoopRegionsTests)
    add_test(NAME StemLabLoopRegionsTests COMMAND StemLabLoopRegionsTests)

    # How a track name and a take/clip name become one source label, which
    # names the window's source strip and every exported stem.
    add_executable(StemLabSourceLabelTests
        Tests/SourceLabelTests.cpp Source/SourceLabel.h)
    target_include_directories(StemLabSourceLabelTests PRIVATE Source)
    target_compile_features(StemLabSourceLabelTests PRIVATE cxx_std_20)
    stemlab_enable_test_assertions(StemLabSourceLabelTests)
    add_test(NAME StemLabSourceLabelTests COMMAND StemLabSourceLabelTests)

    # Upstream's host-capture pass: the self-drag guard plus a real
    # processor capturing processBlock audio, so this one links the plugin.
    add_executable(StemLabHostCaptureTests Tests/HostCaptureTests.cpp
                   Source/SelfFileDragGuard.h)
    target_include_directories(StemLabHostCaptureTests PRIVATE Source
                               "${CMAKE_CURRENT_BINARY_DIR}/StemLabPlugin_artefacts/JuceLibraryCode"
                               "${CMAKE_CURRENT_BINARY_DIR}/juce_binarydata_StemLabAssets/JuceLibraryCode")
    target_compile_features(StemLabHostCaptureTests PRIVATE cxx_std_20)
    target_link_libraries(StemLabHostCaptureTests PRIVATE StemLabPlugin juce::juce_audio_utils
                                                          juce::juce_audio_formats
                                                          juce::juce_gui_extra)
    stemlab_enable_test_assertions(StemLabHostCaptureTests)
    add_test(NAME StemLabHostCaptureTests COMMAND StemLabHostCaptureTests)
endif()

target_compile_definitions(StemLabPlugin
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
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
