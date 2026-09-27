#pragma once
#ifndef FASTPLAY_INI_H
#define FASTPLAY_INI_H

// Settings files (FastPlay.ini) and other INI-format files such as .pls playlists.
// The calls mirror the Windows profile functions they replace, and on Windows they
// are those functions; elsewhere they read and write the same format as UTF-8.
// Section and key names match without regard to case.

// The integer value of `key`, or `defaultValue` if it is missing or not a number.
int IniGetInt(const wchar_t* section, const wchar_t* key, int defaultValue, const wchar_t* path);

// Copy the value of `key` (or `defaultValue` if missing) into `out`, truncated to
// `size` characters including the terminator. Returns the number of characters copied.
unsigned IniGetString(const wchar_t* section, const wchar_t* key, const wchar_t* defaultValue,
                      wchar_t* out, unsigned size, const wchar_t* path);

// Set `key` to `value`. A null `value` removes the key; a null `key` removes the
// whole section. Creates the file if needed.
bool IniWriteString(const wchar_t* section, const wchar_t* key, const wchar_t* value, const wchar_t* path);

// Remove every key in `section`, keeping the section itself.
bool IniClearSection(const wchar_t* section, const wchar_t* path);

#endif // FASTPLAY_INI_H
