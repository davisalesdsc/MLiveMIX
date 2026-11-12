cmake_minimum_required(VERSION 3.22)

# macOS settings MUST come before project(): set afterwards, CMake ignores them and the plugin
# ends up requiring the macOS version of the build machine (it required macOS 15 in 0.1.0).
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary (Apple Silicon + Intel)")

project(MLiveMIX VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# JUCE: downloaded automatically, or a local copy with -DMLIVEMIX_JUCE_DIR=/path/to/JUCE
# ---------------------------------------------------------------------------
set(MLIVEMIX_JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (optional)")

if(MLIVEMIX_JUCE_DIR)
    add_subdirectory(${MLIVEMIX_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

# The web page served to phones / browsers is embedded in the plugin
juce_add_binary_data(MLiveMixWeb SOURCES Web/index.html)

set(MLIVEMIX_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND MLIVEMIX_FORMATS AU)
endif()

juce_add_plugin(MLiveMIX
    COMPANY_NAME "MLiveMIX"
    BUNDLE_ID "com.mlivemix.mlivemix"
    PLUGIN_MANUFACTURER_CODE Mlvm
    PLUGIN_CODE Mlv1
    PRODUCT_NAME "MLiveMIX"
    FORMATS ${MLIVEMIX_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Tools
    AU_MAIN_TYPE kAudioUnitType_Effect)

set(MLIVEMIX_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/net/WebSocket.cpp
    Source/net/StreamServer.cpp
    Source/net/NetUtil.cpp
    Source/ui/QrCodeComponent.cpp
    third_party/qrcodegen/qrcodegen.cpp)

target_sources(MLiveMIX PRIVATE ${MLIVEMIX_SOURCES})
target_include_directories(MLiveMIX PRIVATE Source third_party)

target_compile_definitions(MLiveMIX PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

if(MSVC)
    target_compile_options(MLiveMIX PRIVATE /utf-8)
endif()

target_link_libraries(MLiveMIX
    PRIVATE
        MLiveMixWeb
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Tests:  cmake --build build --target MLiveMixTests
#   MLiveMixTests                 -> unit + integration tests
#   MLiveMixTests --serve 20 8800 -> serves a test tone for 20 s on port 8800 (browser test)
# ---------------------------------------------------------------------------
option(MLIVEMIX_BUILD_TESTS "Build the test runner" ON)

if(MLIVEMIX_BUILD_TESTS)
    juce_add_console_app(MLiveMixTests PRODUCT_NAME "MLiveMixTests")
    target_sources(MLiveMixTests PRIVATE Tests/MLiveMixTests.cpp ${MLIVEMIX_SOURCES})
    target_include_directories(MLiveMixTests PRIVATE Source third_party)
    target_compile_definitions(MLiveMixTests PRIVATE
        JucePlugin_Name="MLiveMIX"
        JucePlugin_VersionString="${PROJECT_VERSION}"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)
    if(MSVC)
        target_compile_options(MLiveMixTests PRIVATE /utf-8)
    endif()
    target_link_libraries(MLiveMixTests PRIVATE
        MLiveMixWeb
        juce::juce_audio_utils
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()
