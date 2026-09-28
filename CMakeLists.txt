cmake_minimum_required(VERSION 3.21)

# macOS: one universal app for Apple silicon and Intel Macs. Set before project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS FastPlay runs on")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architectures to build for")

project(FastPlay LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)  # the macOS platform files are Objective-C++
endif()

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
    set(FASTPLAY_NO_WARNINGS /w)
else()
    set(FASTPLAY_NO_WARNINGS -w)
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
# The 3.3 line (Windows 7 and later), at a master commit: master carries the
# screen reader fixes made since 3.3.3, most of them for VoiceOver on macOS
# (controls titled by their labels, context menus, custom list cells) and some
# for Windows. Move to the next 3.3 release tag once it has them.
FetchContent_Declare(wxWidgets
    GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
    GIT_TAG 776a9a278791fe0d1c79be337e55ab8e53f89587
)
set(wxBUILD_SHARED OFF CACHE INTERNAL "")
# The same C runtime as FastPlay; mixing the static and DLL runtimes fails at link time.
set(wxBUILD_USE_STATIC_RUNTIME ON CACHE INTERNAL "")
set(wxBUILD_TESTS OFF CACHE INTERNAL "")
set(wxBUILD_SAMPLES OFF CACHE INTERNAL "")
set(wxBUILD_DEMOS OFF CACHE INTERNAL "")
if(APPLE)
    # A precompiled header is built for one architecture; the app is universal.
    set(wxBUILD_PRECOMP OFF CACHE INTERNAL "")
else()
    set(wxBUILD_PRECOMP ON CACHE INTERNAL "")
endif()
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
set(wxUSE_LIBWEBP OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(wxWidgets)

# --- UniversalSpeech (Windows only) ---
# Speaks through whatever screen reader is running - NVDA, JAWS, Narrator and
# others. Distributed as source and compiled straight in.
if(WIN32)
    FetchContent_Declare(universalspeech
        GIT_REPOSITORY https://github.com/samtupy/UniversalSpeechMSVCStatic.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(universalspeech)
    set(UNIVERSALSPEECH_DIR ${universalspeech_SOURCE_DIR})
endif()

# --- miniaudio ---
# Audio output: the playback devices on every system (WASAPI, Core Audio...).
# One header, compiled once by src/audio/miniaudio.c.
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(miniaudio)
if(NOT miniaudio_POPULATED)
    FetchContent_Populate(miniaudio)  # the header only; not its own build
endif()

# --- SpessaSynth ---
# MIDI playback with SoundFonts (SF2) and DLS: kode54's C port of SpessaSynth.
FetchContent_Declare(spessasynth
    GIT_REPOSITORY https://github.com/kode54/spessasynth_core_c.git
    GIT_TAG 99fc352d8bb38851cb9b13c36eb8eb7a98903c2b
    SOURCE_SUBDIR spessasynth_core
)
set(SS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SS_ENABLE_SF3_VORBIS OFF CACHE BOOL "" FORCE)
set(SS_ENABLE_SF3_FLAC OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spessasynth)
# Its headers include spessasynth_exports.h under MSVC, which only its shared
# build generates; a static library exports nothing.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/spessasynth_exports/spessasynth_exports.h
    "#pragma once\n#define SPESSASYNTH_EXPORTS\n")
target_include_directories(spessasynth PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/spessasynth_exports)
target_compile_options(spessasynth PRIVATE ${FASTPLAY_NO_WARNINGS})

# --- libopenmpt ---
# Tracker modules (MOD, XM, IT, S3M, MO3...). The release source has no CMake build,
# so it is built here, the way its own makefile builds it: with its bundled miniz,
# minimp3 and stb_vorbis for compressed modules, and no other library.
FetchContent_Declare(libopenmpt
    URL https://lib.openmpt.org/files/libopenmpt/src/libopenmpt-0.8.7+release.makefile.tar.gz
    URL_HASH SHA256=8a6c0a70b034740f6cdd7b53de65181c3bca82183d084a53ed7fd678ba1a38ab
)
FetchContent_GetProperties(libopenmpt)
if(NOT libopenmpt_POPULATED)
    FetchContent_Populate(libopenmpt)
endif()
set(OPENMPT_DIR ${libopenmpt_SOURCE_DIR})
file(GLOB_RECURSE OPENMPT_SOURCES
    ${OPENMPT_DIR}/common/*.cpp
    ${OPENMPT_DIR}/sounddsp/*.cpp
    ${OPENMPT_DIR}/soundlib/*.cpp)
list(APPEND OPENMPT_SOURCES
    ${OPENMPT_DIR}/libopenmpt/libopenmpt_c.cpp
    ${OPENMPT_DIR}/libopenmpt/libopenmpt_cxx.cpp
    ${OPENMPT_DIR}/libopenmpt/libopenmpt_impl.cpp
    ${OPENMPT_DIR}/libopenmpt/libopenmpt_ext_impl.cpp
    ${OPENMPT_DIR}/include/miniz/miniz.c
    ${OPENMPT_DIR}/include/minimp3/minimp3.c
    ${OPENMPT_DIR}/include/stb_vorbis/stb_vorbis.c)
add_library(openmpt STATIC ${OPENMPT_SOURCES})
target_include_directories(openmpt
    PUBLIC ${OPENMPT_DIR}
    PRIVATE ${OPENMPT_DIR}/src ${OPENMPT_DIR}/common ${OPENMPT_DIR}/include ${OPENMPT_DIR}/build/svn_version)
target_compile_definitions(openmpt PRIVATE
    LIBOPENMPT_BUILD
    MPT_WITH_MINIZ MPT_WITH_MINIMP3
    MPT_WITH_STBVORBIS STB_VORBIS_NO_PULLDATA_API STB_VORBIS_NO_STDIO)
target_compile_options(openmpt PRIVATE ${FASTPLAY_NO_WARNINGS} $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)

option(FASTPLAY_SPEECH "Screen reader speech through UniversalSpeech" ON)

# Configure only the dependencies (used to start the long wxWidgets build early).
option(FASTPLAY_DEPS_ONLY "Configure the fetched dependencies without FastPlay itself" OFF)
if(FASTPLAY_DEPS_ONLY)
    return()
endif()

# ---------------------------------------------------------------------------
# FastPlay
# ---------------------------------------------------------------------------
# FFmpeg is FastPlay's own audio-only build (ci/ffmpeg), in ffmpeg/: built by CI,
# fetched for local builds by download-deps. The tempo libraries come from
# download-deps (deps/); SQLite's amalgamation is src/sqlite3.c.
set(FASTPLAY_FFMPEG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg)
if(NOT EXISTS ${FASTPLAY_FFMPEG_DIR}/include/libavformat/avformat.h)
    message(FATAL_ERROR "FFmpeg is missing from ffmpeg/. Run download-deps, or build it with ci/ffmpeg/build.sh.")
endif()

set(FASTPLAY_SOURCES
    src/audio/decoder_ffmpeg.cpp
    src/audio/decoder_midi.cpp
    src/audio/decoder_tracker.cpp
    src/audio/decoders.cpp
    src/audio/engine.cpp
    src/audio/miniaudio.c
    src/audio/tempo_processor.cpp
    src/center_cancel.cpp
    src/convolution.cpp
    src/database.cpp
    src/download_manager.cpp
    src/effects.cpp
    src/globals.cpp
    src/hotkeys.cpp
    src/http_common.cpp
    src/mp4_chapters.cpp
    src/mp4_index.cpp
    src/mp4_remux.cpp
    src/player.cpp
    src/playlist_io.cpp
    src/podcast.cpp
    src/radio.cpp
    src/scheduler.cpp
    src/settings.cpp
    src/spatial_audio.cpp
    src/updater.cpp
    src/utils.cpp
    src/xheaac.cpp
    src/youtube.cpp
    src/sqlite3.c
    src/reverb/reverb.cpp
    src/reverb/efx_reverb.cpp
    src/spatial/hrtf.cpp
    src/spatial/hrtf_data.cpp
    src/spatial/pffft/pffft.c
    # Simulated speakers in rooms: the 3D Audio room presets
    src/speakers/presets.cpp
    src/speakers/core/dsp.cpp
    src/speakers/model/room.cpp
    src/speakers/model/speaker.cpp
    src/speakers/model/system.cpp
    src/speakers/engine/engine.cpp
    src/speakers/engine/modes.cpp
    src/speakers/engine/reverb.cpp
    src/speakers/engine/spatial.cpp
    src/speakers/engine/voice.cpp
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
        src/platform/http_windows.cpp
        src/platform/ini_windows.cpp
        src/platform/paths_windows.cpp
        src/platform/subprocess_windows.cpp
        src/platform/win7_compat.cpp
        src/platform/speech_windows.cpp
        src/platform/startup_windows.cpp
        src/platform/updater_windows.cpp
        res/fastplay.rc
    )
else()
    list(APPEND FASTPLAY_SOURCES
        src/platform/http_curl.cpp
        src/platform/ini_portable.cpp
        src/platform/subprocess_posix.cpp
    )
endif()
if(APPLE)
    list(APPEND FASTPLAY_SOURCES
        src/platform/file_assoc_mac.cpp
        src/platform/menu_keys_mac.mm
        src/platform/paths_mac.cpp
        src/platform/speech_mac.mm
        src/platform/startup_mac.cpp
        src/platform/system_keys_mac.mm
        src/platform/updater_mac.cpp
    )
elseif(NOT WIN32)
    list(APPEND FASTPLAY_SOURCES src/platform/updater_unsupported.cpp)
endif()

add_executable(FastPlay WIN32 MACOSX_BUNDLE ${FASTPLAY_SOURCES})

# Fraunhofer FDK AAC, for xHE-AAC (src/xheaac.cpp). Its decoder only;
# nothing of it is installed.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(FDK_AAC_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(FDK_AAC_INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/fdk-aac EXCLUDE_FROM_ALL)
target_compile_options(fdk-aac PRIVATE ${FASTPLAY_NO_WARNINGS})
target_link_libraries(FastPlay PRIVATE fdk-aac spessasynth openmpt)

# The upstream C libraries' warnings are not ours to fix.
set_source_files_properties(src/sqlite3.c deps/speedy/speedy.c deps/speedy/soniclib.c
    deps/sonic/sonic.c deps/kissfft/kiss_fft.c src/spatial/pffft/pffft.c PROPERTIES COMPILE_OPTIONS ${FASTPLAY_NO_WARNINGS})

target_include_directories(FastPlay PRIVATE
    src
    src/audio
    include
    include/fastplay
    ${FASTPLAY_FFMPEG_DIR}/include
    ${miniaudio_SOURCE_DIR}
    deps/speedy
    deps/sonic
    deps/kissfft
    deps/signalsmith-stretch
)

target_compile_definitions(FastPlay PRIVATE
    USE_SPEEDY KISS_FFT SONIC_INTERNAL
    USE_SIGNALSMITH
    # miniaudio: only its devices and ring buffer
    MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_ENGINE MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH
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
if(NOT WIN32)
    find_package(CURL REQUIRED)
    target_link_libraries(FastPlay PRIVATE CURL::libcurl)
endif()

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
        ${UNIVERSALSPEECH_DIR}/src/windows/systemaccess.c
        ${UNIVERSALSPEECH_DIR}/src/windows/windows-eyes.c
        ${UNIVERSALSPEECH_DIR}/src/windows/zdsr.c
        ${UNIVERSALSPEECH_DIR}/src/windows/zoomtext.c
        ${UNIVERSALSPEECH_DIR}/src/windows/zoomtext-guid.c
    )
    target_sources(FastPlay PRIVATE ${US_SOURCES})
    # Built the way its own build builds it:
    # - Without UNICODE. It is ANSI code: FindWindow("JFWUI2", ...) is how it
    #   notices JAWS is running, and compiled as FindWindowW that narrow string
    #   names no window, so JAWS was never found and said nothing.
    # - Without SAPI: with no screen reader running, nothing speaks, as before.
    # - Old C code whose warnings are upstream's, not ours.
    set_source_files_properties(${US_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS "UNIVERSAL_SPEECH_STATIC;UNIVERSAL_SPEECH_BUILDING;UNIVERSAL_SPEECH_NO_SAPI"
        COMPILE_OPTIONS "${FASTPLAY_NO_WARNINGS};/UUNICODE;/U_UNICODE")
    target_compile_definitions(FastPlay PRIVATE UNIVERSAL_SPEECH_STATIC)

    # The screen reader client DLLs are loaded at run time by UniversalSpeech.
    add_custom_command(TARGET FastPlay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${UNIVERSALSPEECH_DIR}/bin-x64/nvdaControllerClient64.dll
            ${UNIVERSALSPEECH_DIR}/bin-x64/SAAPI64.dll
            ${CMAKE_CURRENT_SOURCE_DIR}/lib)
endif()

if(WIN32)
    # FFmpeg's static libraries, and what they use of Windows (sockets, TLS through
    # SChannel, random numbers)
    target_link_libraries(FastPlay PRIVATE
        ${FASTPLAY_FFMPEG_DIR}/lib/avformat.lib ${FASTPLAY_FFMPEG_DIR}/lib/avcodec.lib
        ${FASTPLAY_FFMPEG_DIR}/lib/swresample.lib ${FASTPLAY_FFMPEG_DIR}/lib/avutil.lib
        ws2_32 secur32 ncrypt crypt32 bcrypt
        user32 comctl32 comdlg32 shell32 shlwapi advapi32 ole32 oleaut32
        uuid version psapi wininet winhttp)

    # FastPlay.exe goes to the repository root, where dist.bat and the installer expect it.
    set_target_properties(FastPlay PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(APPLE)
    # FFmpeg's static libraries (universal), with what they use of macOS (TLS through
    # Secure Transport, zlib); miniaudio plays through Core Audio.
    target_link_libraries(FastPlay PRIVATE
        ${FASTPLAY_FFMPEG_DIR}/lib/libavformat.a ${FASTPLAY_FFMPEG_DIR}/lib/libavcodec.a
        ${FASTPLAY_FFMPEG_DIR}/lib/libswresample.a ${FASTPLAY_FFMPEG_DIR}/lib/libavutil.a
        z "-framework Security" "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox"
        "-framework AppKit" "-framework AVFoundation" "-framework Carbon"
        "-framework CoreServices" "-framework MediaPlayer")
    set_source_files_properties(src/platform/speech_mac.mm src/platform/system_keys_mac.mm
        src/platform/menu_keys_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)

    # The app bundle: Info.plist with the version from version.h and the
    # documentation in Contents/Resources/docs.
    file(STRINGS include/fastplay/version.h FASTPLAY_VERSION_LINE REGEX "#define APP_VERSION ")
    string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" FASTPLAY_VERSION "${FASTPLAY_VERSION_LINE}")
    configure_file(res/mac/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)
    set_target_properties(FastPlay PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks")

    add_custom_command(TARGET FastPlay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/docs
            $<TARGET_BUNDLE_CONTENT_DIR:FastPlay>/Resources/docs)
endif()
