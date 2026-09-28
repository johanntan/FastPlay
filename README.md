# FastPlay

A fast, accessible audio player for Windows and macOS with support for tempo/pitch shifting, effects, and screen reader accessibility.

## Features

- Tempo, pitch, and rate adjustment (Speedy for speech, Signalsmith Stretch for music)
- Audio effects (reverb, echo, EQ, compressor, stereo width, center/vocal cancel, convolution, 3D audio)
- Recording to WAV, MP3, OGG and FLAC
- MIDI with SoundFonts, and tracker modules
- Internet radio streaming with favorites
- YouTube audio playback
- Screen reader support via Universal Speech
- Hotkey support
- Chapter navigation for audiobooks

## Prerequisites

- **Visual Studio 2022** (or its Build Tools) with the "Desktop development with C++" workload
  - Download from [Visual Studio Downloads](https://visualstudio.microsoft.com/downloads/)
- **CMake** 3.21 or newer
- **Git** (CMake fetches some dependencies with it)

## Dependencies

Run `download-deps.bat` once. It downloads SQLite, Speedy, Sonic, KissFFT,
Signalsmith Stretch and FDK AAC into `deps/` and `src/`, and FastPlay's FFmpeg
into `ffmpeg/`: the audio-only build CI makes with `ci/ffmpeg/build.sh`, taken
from CI's latest run with the GitHub CLI (`gh auth login` first).

wxWidgets (the user interface toolkit), UniversalSpeech (screen reader speech),
miniaudio, SpessaSynth, libopenmpt, LAME, libogg and libvorbis are fetched and
built by CMake itself on the first build.

## Building

Open a command prompt and run:

```batch
build_new.bat
```

This configures CMake in `build/`, builds `FastPlay.exe`, and packages
`FastPlay.zip` (and `FastPlayInstaller.exe` when Inno Setup is installed). The
first build takes a while because it compiles wxWidgets; later builds reuse it.

### Build Options

Disable screen reader support:
```batch
build_new.bat no-speech
```

## Running

After building, run `FastPlay.exe`. The screen reader client DLLs are loaded from the `lib/` subfolder.

## Project Structure

```
FastPlay/
├── src/              # Player, effects, database and other core code
│   ├── audio/        # The audio engine: decoders, tempo, output, recording
│   ├── ui/           # The user interface (wxWidgets)
│   ├── platform/     # Operating system specific code
│   ├── reverb/       # The reverb engines
│   └── spatial/      # The HRTF behind 3D Audio (SADIE II data, pffft)
├── include/          # Header files
├── lib/              # Screen reader client DLLs
├── ffmpeg/           # FastPlay's FFmpeg build (ci/ffmpeg)
├── deps/             # Third-party dependencies (Speedy, Sonic, Signalsmith Stretch, etc.)
├── res/              # Windows resources
├── CMakeLists.txt    # Build definition
├── build_new.bat     # Build script
└── FastPlay.exe      # Output executable
```

## License

This project uses the following third-party libraries:
- FFmpeg (LGPL 2.1 or later), built from source by ci/ffmpeg/build.sh
- miniaudio (public domain / MIT No Attribution)
- SpessaSynth, C port by kode54 (Apache 2.0)
- libopenmpt (BSD)
- LAME (LGPL)
- libogg and libvorbis (BSD)
- Fraunhofer FDK AAC (FDK AAC licence)
- Speedy (Apache 2.0), Sonic (Apache 2.0), Signalsmith Stretch (MIT)
- SQLite (public domain)
- wxWidgets (wxWindows Library Licence)
- Universal Speech (MIT)
- SADIE II HRTF database, University of York (Apache 2.0)
- pffft (FFTPACK licence)
