#pragma once
#ifndef FASTPLAY_COMMANDS_H
#define FASTPLAY_COMMANDS_H

// Main window command ids: the menu, accelerators, tray menu and global hotkey
// actions all run these through the main window's command handler.
// wxWidgets reserves 4999-5999 for its own ids, so nothing here may use that range.

#define IDM_FILE_OPEN       101
#define IDM_FILE_EXIT       102
#define IDM_TOOLS_OPTIONS   103
#define IDM_FILE_OPEN_URL   104
#define IDM_FILE_HIDE_TRAY  106
#define IDM_FILE_ADD_FOLDER 110
#define IDM_FILE_PLAYLIST   111
#define IDM_FILE_RECENT_BASE 6000  // Recent files use IDs 6000-6009
#define IDM_PLAY_PLAYPAUSE  201
#define IDM_PLAY_STOP       202
#define IDM_PLAY_PREV       203
#define IDM_PLAY_NEXT       204
#define IDM_PLAY_PLAY       212
#define IDM_PLAY_PAUSE      213
#define IDM_PLAY_SEEKBACK   205
#define IDM_PLAY_SEEKFWD    206
#define IDM_PLAY_VOLUP      207
#define IDM_PLAY_VOLDOWN    208
#define IDM_PLAY_ELAPSED    209
#define IDM_PLAY_REMAINING  210
#define IDM_PLAY_TOTAL      211
#define IDM_PLAY_NOWPLAYING 214
#define IDM_PLAY_SHUFFLE    215
#define IDM_PLAY_BEGINNING  216
#define IDM_PLAY_JUMPTOTIME 217
#define IDM_PLAY_MUTE       218
#define IDM_PLAY_REPEAT_TOGGLE 219
#define IDM_EFFECT_PRESETS  238
#define IDM_SEEK_DECREASE   220
#define IDM_SEEK_INCREASE   221
#define IDM_SEEK_MODE       222
#define IDM_PLAY_GOLIVE     223
#define IDM_FILE_LIBRARY    224
#define IDM_EFFECT_PREV     230
#define IDM_EFFECT_NEXT     231
#define IDM_EFFECT_UP       232
#define IDM_EFFECT_DOWN     233
#define IDM_EFFECT_RESET    234
#define IDM_EFFECT_MIN      235
#define IDM_EFFECT_MAX      236
#define IDM_SHOW_AUDIO_DEVICES  237
#define IDM_AUDIO_DEVICE_BASE   8000  // Actual device IDs are IDM_AUDIO_DEVICE_BASE + device index
#define IDM_TOGGLE_VOLUME       240
#define IDM_TOGGLE_PITCH        241
#define IDM_TOGGLE_TEMPO        242
#define IDM_TOGGLE_RATE         243
#define IDM_TOGGLE_REVERB       244
#define IDM_TOGGLE_ECHO         245
#define IDM_TOGGLE_EQ           246
#define IDM_TOGGLE_COMPRESSOR   247
#define IDM_TOGGLE_STEREOWIDTH  248
#define IDM_TOGGLE_CENTERCANCEL 249
#define IDM_TOGGLE_SPATIAL      251
#define IDM_TOGGLE_CONVOLUTION  252
#define IDM_SPEAK_SEEK          250
#define IDM_READ_TAG_TITLE      261
#define IDM_READ_TAG_ARTIST     262
#define IDM_READ_TAG_ALBUM      263
#define IDM_READ_TAG_YEAR       264
#define IDM_READ_TAG_TRACK      265
#define IDM_READ_TAG_GENRE      266
#define IDM_READ_TAG_COMMENT    267
#define IDM_READ_TAG_BITRATE    268
#define IDM_READ_TAG_DURATION   269
#define IDM_READ_TAG_FILENAME   260
#define IDM_TRAY_RESTORE    700
#define IDM_TRAY_EXIT       701
#define IDM_TOGGLE_WINDOW   702
#define IDM_FILE_YOUTUBE    105
#define IDM_BOOKMARK_ADD    800
#define IDM_BOOKMARK_LIST   801
#define IDM_RECORD_TOGGLE   820
#define IDM_FILE_RADIO      107
#define IDM_FILE_ADD_TO_FAVORITES 113
#define IDM_HELP_PLUGINS    109
#define IDM_HELP_UPDATES    112
#define IDM_HELP_README     114
#define IDM_FILE_SCHEDULE   108
#define IDM_PRESET_BASE     7000  // Apply preset: IDM_PRESET_BASE + index (up to 100)
#define IDM_PRESET_DELETE_BASE 7100  // Delete preset: IDM_PRESET_DELETE_BASE + index (up to 100)
#define IDM_PRESET_SAVE_NEW 7200
#define IDM_VIEW_TAG_TITLE      270
#define IDM_VIEW_TAG_ARTIST     271
#define IDM_VIEW_TAG_ALBUM      272
#define IDM_VIEW_TAG_YEAR       273
#define IDM_VIEW_TAG_TRACK      274
#define IDM_VIEW_TAG_GENRE      275
#define IDM_VIEW_TAG_COMMENT    276
#define IDM_VIEW_TAG_BITRATE    277
#define IDM_VIEW_TAG_DURATION   278
#define IDM_VIEW_TAG_FILENAME   279
#define IDM_FILE_PODCAST        940
#define IDM_VIEW_SONG_HISTORY   990

#endif // FASTPLAY_COMMANDS_H
