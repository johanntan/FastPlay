#pragma once
#ifndef FASTPLAY_SPEAKERS_PRESETS_H
#define FASTPLAY_SPEAKERS_PRESETS_H

// The room presets 3D Audio offers: a room from the room catalog with a speaker
// system installed in it, chosen to sound like something people actually own.

#include <string>

namespace speakers {

class SpeakerSystem;

constexpr int kRoomPresetCount = 12;

// The preset's spoken name ("Car with a subwoofer"), or "" for a bad index.
const char* RoomPresetName(int preset);

// Whether the preset has subwoofers, and so a sub level and crossover to adjust.
bool RoomPresetHasSub(int preset);

// Replace `system` with the preset: its room, its speakers, and the listener in
// the room's listening seat. False for a bad index.
bool BuildRoomPreset(int preset, SpeakerSystem& system);

}  // namespace speakers

#endif  // FASTPLAY_SPEAKERS_PRESETS_H
