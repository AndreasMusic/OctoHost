cmake_minimum_required(VERSION 3.22)
project(OctoHost VERSION 3.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment version")
endif()

if(MSVC)
    # Statically link MSVC runtime so Windows users don't need VC++ Redistributable installed
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.6
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(OctoHost
    PRODUCT_NAME "OctoHost"
    COMPANY_NAME "OctoHost"
    BUNDLE_ID "com.octohost.daw"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "OctoHost needs audio input access to record guitar, bass, and vocals."
    HARDENED_RUNTIME_ENABLED FALSE
)

target_sources(OctoHost PRIVATE
    Source/Main.cpp
    Source/LookAndFeel.h
    Source/PluginWindow.h
    Source/DSPUtils.h
    Source/DrumEngine.h
    Source/MidiTools.h
    Source/InsertChain.h
    Source/ChannelStrip.h
    Source/AudioEngine.h
    Source/DrumSequencerUI.h
    Source/PianoRollUI.h
    Source/AudioEditorUI.h
    Source/MelodyGeneratorUI.h
    Source/MixerStripUI.h
    Source/MainComponent.h
)

target_compile_definitions(OctoHost PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=$<BOOL:${APPLE}>
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OctoHost,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OctoHost,JUCE_VERSION>"
)

if(WIN32)
    target_compile_definitions(OctoHost PRIVATE NOMINMAX=1)
endif()

target_link_libraries(OctoHost
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
