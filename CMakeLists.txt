cmake_minimum_required(VERSION 3.24)

project(BandJam VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime: the Release exe runs on any Windows PC without
# installing the Visual C++ Redistributable first.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.14
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(BandJam
    PRODUCT_NAME "VRC Downbeat"
    COMPANY_NAME "VibeCoding"
    ICON_BIG     "${CMAKE_CURRENT_SOURCE_DIR}/Assets/app_icon.png"
    ICON_SMALL   "${CMAKE_CURRENT_SOURCE_DIR}/Assets/app_icon.png"
    VERSION      ${PROJECT_VERSION})

# Images embedded into the executable (startup background, in-app logo).
juce_add_binary_data(BandJamAssets SOURCES
    Assets/startup_background.png
    Assets/app_icon.png)

target_sources(BandJam PRIVATE
    Source/Main.cpp
    third_party/monocypher.c
    Source/common/Crypto.cpp
    Source/common/Protocol.cpp
    Source/common/SongLoader.cpp
    Source/common/StreamOutput.cpp
    Source/common/VbCableInstaller.cpp
    Source/common/AppAudioRouter.cpp
    Source/common/AppCapture.cpp
    Source/common/Mp3Encoder.cpp
    Source/common/VoiceChat.cpp
    Source/ui/RootComponent.cpp
    Source/ui/AudioStreamPanel.cpp
    Source/host/SongLibrary.cpp
    Source/host/JamServer.cpp
    Source/host/RelayLink.cpp
    Source/host/HostMixEngine.cpp
    Source/host/HostView.cpp
    Source/ui/RecordingsPanel.cpp
    Source/musician/DownloadStore.cpp
    Source/musician/HostConnection.cpp
    Source/musician/InstrumentHost.cpp
    Source/musician/PlayCaptureEngine.cpp
    Source/musician/MusicianView.cpp)

target_include_directories(BandJam PRIVATE Source third_party)

target_compile_definitions(BandJam PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    # ASIO: the SDK sources ship with JUCE since 8.0.11 (GPLv3), so this just works.
    JUCE_ASIO=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_USE_MP3AUDIOFORMAT=1
    # VST3 hosting: run instrument plugins (e.g. Superior Drummer 3) in-app.
    JUCE_PLUGINHOST_VST3=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:BandJam,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:BandJam,JUCE_VERSION>")

if(MSVC)
    # Source files are UTF-8 (German UI strings).
    target_compile_options(BandJam PRIVATE /utf-8)
endif()

target_link_libraries(BandJam PRIVATE
    BandJamAssets
    juce::juce_gui_extra
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    # OS random number generator (BCryptGenRandom) for the crypto handshake.
    bcrypt)
