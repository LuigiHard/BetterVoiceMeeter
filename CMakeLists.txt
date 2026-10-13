cmake_minimum_required(VERSION 3.22)
project(BetterVoiceMeeter VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE 9.0.3 (bundles the ASIO headers). Cloned into external/JUCE by the build
# script; fall back to FetchContent if it isn't there.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE/CMakeLists.txt")
    add_subdirectory(external/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(BetterVoiceMeeter
    PRODUCT_NAME "BetterVoiceMeeter"
    COMPANY_NAME "BetterVoiceMeeter"
    VERSION 0.3.0
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon-512.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon-64.png")

# Logo for the in-app header.
juce_add_binary_data(BetterVoiceMeeterData SOURCES Resources/icon-64.png)

juce_generate_juce_header(BetterVoiceMeeter)

target_sources(BetterVoiceMeeter PRIVATE
    Source/Main.cpp
    Source/Engine.cpp
    Source/ExternalBridge.cpp
    Source/InsertChain.cpp
    Source/SongPlayer.cpp
    Source/UI/SingStudio.cpp
    Source/UI/MainComponent.cpp)

target_include_directories(BetterVoiceMeeter PRIVATE Source)

# Sing Studio's Python stem worker lives next to the exe.
add_custom_command(TARGET BetterVoiceMeeter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Tools/stemworker" "$<TARGET_FILE_DIR:BetterVoiceMeeter>/stemworker")

target_compile_definitions(BetterVoiceMeeter PRIVATE
    JUCE_ASIO=1
    JUCE_WASAPI=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_DIRECTSOUND=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:BetterVoiceMeeter,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:BetterVoiceMeeter,JUCE_VERSION>")

target_link_libraries(BetterVoiceMeeter
    PRIVATE
        BetterVoiceMeeterData
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(BetterVoiceMeeter PRIVATE /fp:fast)
endif()

# Offline DSP checks: cmake --build build --config Release --target RepairTest
juce_add_console_app(RepairTest PRODUCT_NAME "RepairTest")
juce_generate_juce_header(RepairTest)
target_sources(RepairTest PRIVATE Tests/RepairTest.cpp)
target_compile_definitions(RepairTest PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(RepairTest PRIVATE juce::juce_dsp juce::juce_audio_basics
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Loads real VST3s through InsertChain: PluginSmokeTest <path.vst3> ...
juce_add_console_app(PluginSmokeTest PRODUCT_NAME "PluginSmokeTest")
juce_generate_juce_header(PluginSmokeTest)
target_sources(PluginSmokeTest PRIVATE Tests/PluginSmokeTest.cpp Source/InsertChain.cpp)
target_compile_definitions(PluginSmokeTest PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
target_link_libraries(PluginSmokeTest PRIVATE juce::juce_audio_processors juce::juce_gui_basics
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
