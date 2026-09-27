#pragma once
#ifndef FASTPLAY_ACCESSIBILITY_H
#define FASTPLAY_ACCESSIBILITY_H

#include <string>

// Screen reader speech. Safe to call from any thread: the text is spoken on the UI
// thread, and a message still waiting to be spoken is replaced by a newer one.

bool InitSpeech();
void FreeSpeech();

// ANSI / UTF-8 text
void Speak(const char* text, bool interrupt = true);
void Speak(const std::string& text, bool interrupt = true);

// Unicode text (ID3 tags, international text)
void SpeakW(const wchar_t* text, bool interrupt = true);
void SpeakW(const std::wstring& text, bool interrupt = true);

#endif // FASTPLAY_ACCESSIBILITY_H
