cmake_minimum_required(VERSION 3.22)

project(FIStemPlugin VERSION 0.9.9 LANGUAGES C CXX)

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
else()
    include(FetchContent)

    message(STATUS "No local JUCE source supplied; falling back to JUCE archive download.")

    FetchContent_Declare(
        JUCE
        URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(FIStemPlugin
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/FIStemIcon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/FIStemIcon.png"
    COMPANY_NAME "FI-STEM"
    BUNDLE_ID "org.fistem.audio"
    PLUGIN_MANUFACTURER_CODE StLb
    PLUGIN_CODE SlA1
    FORMATS VST3 Standalone
    PRODUCT_NAME "FI-STEM"
    DESCRIPTION "Capture, separate, and export stems to Ableton Live"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_add_binary_data(FIStemAssets
    SOURCES
        Resources/FIStemIcon.png
        # Inter, under the SIL Open Font License (Resources/fonts/OFL.txt).
        # Registered by StemLabLookAndFeel; the interface falls back to the
        # platform face if these are ever absent.
        Resources/fonts/Inter-Regular.ttf
        Resources/fonts/Inter-Medium.ttf
)

target_sources(FIStemPlugin
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginProcessor.h
        Source/PluginEditor.cpp
        Source/PluginEditor.h
        Source/HostIntegrationPolicy.h
        Source/SelfFileDragGuard.h
        Source/StemLabLookAndFeel.cpp
        Source/StemLabLookAndFeel.h
        Source/StemLabTheme.h
        Source/WaveformGrid.h
)

include(CTest)
if (BUILD_TESTING)
    add_executable(FIStemWaveformGridTests Tests/WaveformGridTests.cpp
                   Source/HostIntegrationPolicy.h Source/WaveformGrid.h)
    target_include_directories(FIStemWaveformGridTests PRIVATE Source)
    target_compile_features(FIStemWaveformGridTests PRIVATE cxx_std_20)
    add_test(NAME FIStemWaveformGridTests COMMAND FIStemWaveformGridTests)

    add_executable(FIStemHostCaptureTests Tests/HostCaptureTests.cpp Source/SelfFileDragGuard.h)
    target_include_directories(FIStemHostCaptureTests PRIVATE Source
                               "${CMAKE_CURRENT_BINARY_DIR}/FIStemPlugin_artefacts/JuceLibraryCode"
                               "${CMAKE_CURRENT_BINARY_DIR}/juce_binarydata_FIStemAssets/JuceLibraryCode")
    target_compile_features(FIStemHostCaptureTests PRIVATE cxx_std_20)
    target_link_libraries(FIStemHostCaptureTests PRIVATE FIStemPlugin juce::juce_audio_utils
                                                        juce::juce_audio_formats
                                                        juce::juce_gui_extra)
    add_test(NAME FIStemHostCaptureTests COMMAND FIStemHostCaptureTests)

    # Pins the theme's contrast guarantees, and by compiling the
    # look-and-feel proves the drawing code builds independently of the
    # processor.
    add_executable(FIStemThemeTests Tests/ThemeTests.cpp Source/StemLabLookAndFeel.cpp
                   Source/StemLabTheme.h Source/StemLabLookAndFeel.h)
    target_include_directories(FIStemThemeTests PRIVATE Source
                               "${CMAKE_CURRENT_BINARY_DIR}/FIStemPlugin_artefacts/JuceLibraryCode"
                               "${CMAKE_CURRENT_BINARY_DIR}/juce_binarydata_FIStemAssets/JuceLibraryCode")
    target_compile_features(FIStemThemeTests PRIVATE cxx_std_20)
    # gui_basics, not gui_extra: the look-and-feel needs LookAndFeel_V4 and
    # nothing from the browser module, which would drag in GTK on Linux.
    target_compile_definitions(FIStemThemeTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
                                                        JUCE_STANDALONE_APPLICATION=1)
    target_link_libraries(FIStemThemeTests PRIVATE FIStemAssets juce::juce_gui_basics
                                                   juce::juce_graphics)
    add_test(NAME FIStemThemeTests COMMAND FIStemThemeTests)
endif()

target_compile_definitions(FIStemPlugin
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(FIStemPlugin
    PRIVATE
        FIStemAssets
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if (WIN32)
    target_link_libraries(FIStemPlugin
        PRIVATE
            ole32
            uuid
    )
endif()

juce_generate_juce_header(FIStemPlugin)
