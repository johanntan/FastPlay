cmake_minimum_required(VERSION 3.21)

project(FastPlay LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /MP /W3)
    # Global on purpose: every dependency must agree. WIN32_LEAN_AND_MEAN is
    # deliberately not set - it hides shellapi.h, which wxWidgets needs.
    add_compile_definitions(
        NOMINMAX
        UNICODE
        _UNICODE
        _CRT_SECURE_NO_WARNINGS
    )
    # Statically link the CRT so FastPlay.exe needs no redistributable.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Floor for old dependencies")
# FastPlay is packaged by dist.bat and the installer, never "installed" by CMake.
set(CMAKE_SKIP_INSTALL_RULES ON)

# ---------------------------------------------------------------------------
# Dependencies fetched by CMake
# ---------------------------------------------------------------------------

# --- wxWidgets ---
# 3.2.x is the last branch that still supports Windows 7.
FetchContent_Declare(wxWidgets
    GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
    GIT_TAG v3.2.11
    GIT_SHALLOW TRUE
)
set(wxBUILD_SHARED OFF CACHE INTERNAL "")
# The same C runtime as FastPlay; mixing the static and DLL runtimes fails at link time.
set(wxBUILD_USE_STATIC_RUNTIME ON CACHE INTERNAL "")
set(wxBUILD_TESTS OFF CACHE INTERNAL "")
set(wxBUILD_SAMPLES OFF CACHE INTERNAL "")
set(wxBUILD_DEMOS OFF CACHE INTERNAL "")
set(wxBUILD_PRECOMP ON CACHE INTERNAL "")
set(wxUSE_STL ON CACHE INTERNAL "")
# FastPlay links core, base, adv and net; the rest of wxWidgets is most of the
# build for code that is never called.
set(wxUSE_STC OFF CACHE INTERNAL "")
set(wxUSE_WEBVIEW OFF CACHE INTERNAL "")
set(wxUSE_MEDIACTRL OFF CACHE INTERNAL "")
set(wxUSE_RICHTEXT OFF CACHE INTERNAL "")
set(wxUSE_PROPGRID OFF CACHE INTERNAL "")
set(wxUSE_RIBBON OFF CACHE INTERNAL "")
set(wxUSE_AUI OFF CACHE INTERNAL "")
set(wxUSE_XRC OFF CACHE INTERNAL "")
set(wxUSE_LIBJPEG OFF CACHE INTERNAL "")
set(wxUSE_LIBTIFF OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(wxWidgets)

# --- UniversalSpeech (Windows only) ---
# Speaks through whatever screen reader is running - NVDA, JAWS, Narrator, or
# SAPI as a fallback. Distributed as source and compiled straight in.
if(WIN32)
    FetchContent_Declare(universalspeech
        GIT_REPOSITORY https://github.com/samtupy/UniversalSpeechMSVCStatic.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(universalspeech)
    set(UNIVERSALSPEECH_DIR ${universalspeech_SOURCE_DIR})
endif()

option(FASTPLAY_SPEECH "Screen reader speech through UniversalSpeech" ON)
option(FASTPLAY_STEAM_AUDIO "3D audio through Steam Audio" ON)

# Configure only the dependencies (used to start the long wxWidgets build early).
option(FASTPLAY_DEPS_ONLY "Configure the fetched dependencies without FastPlay itself" OFF)
if(FASTPLAY_DEPS_ONLY)
    return()
endif()

# ---------------------------------------------------------------------------
# FastPlay
# ---------------------------------------------------------------------------
# BASS, Steam Audio and the tempo libraries come from download-deps.bat
# (include/, lib/ and deps/); SQLite's amalgamation is src/sqlite3.c.

set(FASTPLAY_SOURCES
    src/center_cancel.cpp
    src/convolution.cpp
    src/database.cpp
    src/download_manager.cpp
    src/effects.cpp
    src/globals.cpp
    src/hotkeys.cpp
    src/player.cpp
    src/playlist_io.cpp
    src/podcast.cpp
    src/radio.cpp
    src/scheduler.cpp
    src/settings.cpp
    src/spatial_audio.cpp
    src/tempo_processor.cpp
    src/updater.cpp
    src/utils.cpp
    src/youtube.cpp
    src/sqlite3.c
    src/reverb/reverb.cpp
    src/reverb/efx_reverb.cpp
    src/ui/app.cpp
    src/ui/bookmarks_dialog.cpp
    src/ui/file_dialogs.cpp
    src/ui/history_dialog.cpp
    src/ui/hotkey_dialog.cpp
    src/ui/main_frame.cpp
    src/ui/options_dialog.cpp
    src/ui/playlist_dialog.cpp
    src/ui/podcast_dialog.cpp
    src/ui/radio_dialog.cpp
    src/ui/scheduler_dialog.cpp
    src/ui/small_dialogs.cpp
    src/ui/tray_icon.cpp
    src/ui/ui_common.cpp
    src/ui/update_dialog.cpp
    src/ui/youtube_dialog.cpp
    # Speedy (Google's nonlinear speech speedup) and what it builds on
    deps/speedy/speedy.c
    deps/speedy/soniclib.c
    deps/sonic/sonic.c
    deps/kissfft/kiss_fft.c
)

if(WIN32)
    list(APPEND FASTPLAY_SOURCES
        src/platform/file_assoc_windows.cpp
        src/platform/speech_windows.cpp
        src/platform/startup_windows.cpp
        res/fastplay.rc
    )
endif()

add_executable(FastPlay WIN32 ${FASTPLAY_SOURCES})

# The upstream C libraries' warnings are not ours to fix.
set_source_files_properties(src/sqlite3.c deps/speedy/speedy.c deps/speedy/soniclib.c
    deps/sonic/sonic.c deps/kissfft/kiss_fft.c PROPERTIES COMPILE_OPTIONS "/w")

target_include_directories(FastPlay PRIVATE
    src
    include
    include/fastplay
    deps/speedy
    deps/sonic
    deps/kissfft
    deps/signalsmith-stretch
    deps/steamaudio/include
)

target_compile_definitions(FastPlay PRIVATE
    USE_SPEEDY KISS_FFT SONIC_INTERNAL
    USE_SIGNALSMITH
    $<$<BOOL:${FASTPLAY_STEAM_AUDIO}>:USE_STEAM_AUDIO>
    $<$<BOOL:${FASTPLAY_SPEECH}>:USE_UNIVERSAL_SPEECH>
)

# The commit this was built from, for the update check.
execute_process(COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE FASTPLAY_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(FASTPLAY_BUILD_COMMIT)
    target_compile_definitions(FastPlay PRIVATE BUILD_COMMIT="${FASTPLAY_BUILD_COMMIT}")
endif()

target_link_libraries(FastPlay PRIVATE wx::core wx::base wx::adv wx::net)

if(WIN32 AND FASTPLAY_SPEECH)
    # UniversalSpeech is distributed as source, so it is compiled straight in.
    target_include_directories(FastPlay PRIVATE ${UNIVERSALSPEECH_DIR}/include)
    set(US_SOURCES
        ${UNIVERSALSPEECH_DIR}/src/UniversalSpeech.c
        ${UNIVERSALSPEECH_DIR}/src/windows/cobra.c
        ${UNIVERSALSPEECH_DIR}/src/windows/disphelper.c
        ${UNIVERSALSPEECH_DIR}/src/windows/dolphin.c
        ${UNIVERSALSPEECH_DIR}/src/windows/encoding-conversion.c
        ${UNIVERSALSPEECH_DIR}/src/windows/engines.c
        ${UNIVERSALSPEECH_DIR}/src/windows/jaws.c
        ${UNIVERSALSPEECH_DIR}/src/windows/misc.c
        ${UNIVERSALSPEECH_DIR}/src/windows/narrator.c
        ${UNIVERSALSPEECH_DIR}/src/windows/nvda.c
        ${UNIVERSALSPEECH_DIR}/src/windows/processlist.c
        ${UNIVERSALSPEECH_DIR}/src/windows/sapi.c
        ${UNIVERSALSPEECH_DIR}/src/windows/systemaccess.c
        ${UNIVERSALSPEECH_DIR}/src/windows/windows-eyes.c
        ${UNIVERSALSPEECH_DIR}/src/windows/zdsr.c
        ${UNIVERSALSPEECH_DIR}/src/windows/zoomtext.c
        ${UNIVERSALSPEECH_DIR}/src/windows/zoomtext-guid.c
    )
    target_sources(FastPlay PRIVATE ${US_SOURCES})
    # Old C code whose warnings are upstream's, not ours.
    set_source_files_properties(${US_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "UNIVERSAL_SPEECH_STATIC;UNIVERSAL_SPEECH_BUILDING"
        COMPILE_OPTIONS "/w")
    target_compile_definitions(FastPlay PRIVATE UNIVERSAL_SPEECH_STATIC)

    # The screen reader client DLLs are loaded at run time by UniversalSpeech.
    add_custom_command(TARGET FastPlay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${UNIVERSALSPEECH_DIR}/bin-x64/nvdaControllerClient64.dll
            ${UNIVERSALSPEECH_DIR}/bin-x64/SAAPI64.dll
            ${CMAKE_CURRENT_SOURCE_DIR}/lib)
endif()

if(WIN32)
    # BASS and friends load from lib\ beside the executable (see PlatformStartup),
    # delay-loaded so the search path can be set first.
    target_link_directories(FastPlay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    target_link_libraries(FastPlay PRIVATE
        bass bass_fx bass_aac bassmidi bassenc bassenc_mp3 bassenc_ogg bassenc_flac
        delayimp user32 comctl32 comdlg32 shell32 shlwapi advapi32 ole32 oleaut32
        uuid version psapi wininet winhttp)
    target_link_options(FastPlay PRIVATE
        /DELAYLOAD:bass.dll /DELAYLOAD:bass_fx.dll /DELAYLOAD:bass_aac.dll /DELAYLOAD:bassmidi.dll
        /DELAYLOAD:bassenc.dll /DELAYLOAD:bassenc_mp3.dll /DELAYLOAD:bassenc_ogg.dll /DELAYLOAD:bassenc_flac.dll)

    # FastPlay.exe goes to the repository root, where dist.bat and the installer expect it.
    set_target_properties(FastPlay PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR})
endif()
