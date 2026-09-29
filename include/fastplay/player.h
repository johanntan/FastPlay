#pragma once
#ifndef FASTPLAY_PLAYER_H
#define FASTPLAY_PLAYER_H

// Playback as the rest of FastPlay sees it: the playlist, tracks, seeking, volume,
// devices, tags and recording, on top of the audio engine (audio.h).

#include <string>
#include <vector>

// The audio engine, on the saved device (or the default)
bool InitAudio();
void FreeAudio();
// What the engine is and plays through, for Help
std::wstring GetAudioEngineInfo();

// Playback control
bool LoadFile(const wchar_t* path);
bool LoadURL(const wchar_t* url);
bool IsURL(const wchar_t* path);
bool IsPlaying();
void PlayPause();
void Play();
void Pause();
void Stop();
void FreeCurrentStream();

// Seeking
void Seek(double seconds);
void SeekTracks(int tracks);
void SeekToPosition(double seconds);
double GetCurrentPosition();

// Seek modes (globals.h): slash cycles them; in spring and tape seeking, holding an
// arrow scrubs (Start on the key going down, Stop on it coming up) and comma and
// period change the speed.
void CycleSeekMode();
bool IsScrubSeekMode();
void ChangeScrubSpeed(int direction);
void SpeakSeekMode();
void StartScrubbing(int direction);
void StopScrubbing();
double GetCurrentLength();  // 0 for a live stream

// Chapters
bool SeekToNextChapter();
bool SeekToPrevChapter();
int GetCurrentChapterIndex();

// Volume
void SetVolume(float vol);
void ToggleMute();
void RefreshReplayGain();  // Recompute and re-apply ReplayGain for the current track (after settings change)
void UpdateOutputGain();   // Apply volume, mute and ReplayGain

// Track navigation
void NextTrack(bool autoPlay = true);
void PrevTrack();
void PlayTrack(int index, bool autoPlay = true);
void ToggleRepeatMode();
void ResetShuffleOrder();  // Discard the current shuffle order (fresh shuffle on next advance)

// A stream's title changed (internet radio): record it, and say it if wanted
void AnnounceStreamMetadata();

// Devices. A device is its number in the system's list, from 1; -1 the default.
bool SwitchAudioDevice(int device);
int FindDeviceByName(const std::wstring& name);
std::wstring GetDeviceName(int device);
struct AudioDeviceInfo {
    int index;          // device number; 0 is "Default", the system's default device
    std::wstring name;
    bool current;       // the device FastPlay is using
};
std::vector<AudioDeviceInfo> GetAudioDevices();
void SelectAudioDevice(int deviceIndex);

// MIDI settings (SoundFont, voices)
void ApplyMidiSettings();

// Speak functions
void SpeakElapsed();
void SpeakRemaining();
void SpeakTotal();

// Tag reading functions (speak ID3/metadata tags)
void SpeakTagTitle();
void SpeakTagArtist();
void SpeakTagAlbum();
void SpeakTagYear();
void SpeakTagTrack();
void SpeakTagGenre();
void SpeakTagComment();
void SpeakTagBitrate();
int GetCurrentBitrate();  // Returns current stream bitrate in kbps, or 0 if unavailable
bool IsCurrentVbr();      // A variable bitrate file
void SpeakTagDuration();
void SpeakTagFilename();

// Tag retrieval functions (return tag text for display)
std::wstring GetTagTitle();
std::wstring GetTagArtist();
std::wstring GetTagAlbum();
std::wstring GetTagYear();
std::wstring GetTagTrack();
std::wstring GetTagGenre();
std::wstring GetTagComment();
std::wstring GetTagBitrate();
std::wstring GetTagDuration();
std::wstring GetTagFilename();

// Recording functions
void ToggleRecording();
void StopRecording();

#endif // FASTPLAY_PLAYER_H
