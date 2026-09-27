# FastPlay

A fast, accessible audio player for Windows with support for tempo/pitch shifting, effects, and screen reader accessibility.

## Features

- Tempo, pitch, and rate adjustment with multiple algorithms (SoundTouch, Speedy, Signalsmith Stretch)
- Audio effects (reverb, echo, EQ, compressor, stereo width, center/vocal cancel, convolution, 3D audio)
- Recording/encoding to MP3, OGG, FLAC
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

Run `download-deps.bat` once. It downloads BASS and its add-ons, SQLite, Speedy,
Sonic, KissFFT and Signalsmith Stretch into `lib/`, `include/`, `deps/` and `src/`.

wxWidgets (the user interface toolkit) and UniversalSpeech (screen reader
speech) are fetched and built by CMake itself on the first build.

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

After building, run `FastPlay.exe`. DLLs are loaded from the `lib/` subfolder.

## Project Structure

```
FastPlay/
├── src/              # Player, effects, database and other core code
│   ├── ui/           # The user interface (wxWidgets)
│   ├── platform/     # Operating system specific code
│   ├── reverb/       # The reverb engines
│   └── spatial/      # The HRTF behind 3D Audio (SADIE II data, pffft)
├── include/          # Header files
├── lib/              # BASS libraries and DLLs
├── deps/             # Third-party dependencies (Speedy, Sonic, Signalsmith Stretch, etc.)
├── res/              # Windows resources
├── CMakeLists.txt    # Build definition
├── build_new.bat     # Build script
└── FastPlay.exe      # Output executable
```

## License

This project uses the following third-party libraries:
- BASS and related libraries (commercial/free for non-commercial use)
- SQLite (public domain)
- wxWidgets (wxWindows Library Licence)
- Universal Speech (MIT)
- SADIE II HRTF database, University of York (Apache 2.0)
- pffft (FFTPACK licence)
