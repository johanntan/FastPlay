// Windows speech via UniversalSpeech, which speaks through whichever screen reader
// is running (NVDA, JAWS, and others).

#include "accessibility.h"
#include "app_ui.h"
#include "utils.h"

#include <mutex>
#include <string>

#ifdef USE_UNIVERSAL_SPEECH
#include "UniversalSpeech.h"
#else
// Built without screen reader support: speech does nothing.
static void speechStop() {}
static void speechSay(const wchar_t*, int) {}
#endif

static std::mutex g_speechMutex;
static std::wstring g_pendingSpeechW;
static bool g_speechInterrupt = true;
static bool g_speechInitialized = false;

// Speak whatever is pending. Runs on the UI thread.
static void DoSpeak() {
    std::wstring text;
    bool interrupt;
    {
        std::lock_guard<std::mutex> lock(g_speechMutex);
        if (!g_speechInitialized || g_pendingSpeechW.empty()) return;
        text.swap(g_pendingSpeechW);
        interrupt = g_speechInterrupt;
    }
    if (interrupt) {
        speechStop();
    }
    speechSay(text.c_str(), interrupt ? 1 : 0);
}

void SpeakW(const wchar_t* text, bool interrupt) {
    {
        std::lock_guard<std::mutex> lock(g_speechMutex);
        if (!g_speechInitialized) return;
        g_pendingSpeechW = text;
        g_speechInterrupt = interrupt;
    }
    RunOnUiThread(DoSpeak);
}

void SpeakW(const std::wstring& text, bool interrupt) {
    SpeakW(text.c_str(), interrupt);
}

void Speak(const char* text, bool interrupt) {
    SpeakW(Utf8ToWide(text), interrupt);
}

void Speak(const std::string& text, bool interrupt) {
    Speak(text.c_str(), interrupt);
}

bool InitSpeech() {
    std::lock_guard<std::mutex> lock(g_speechMutex);
    g_speechInitialized = true;
    return true;
}

void FreeSpeech() {
    std::lock_guard<std::mutex> lock(g_speechMutex);
    if (g_speechInitialized) {
        speechStop();
        g_speechInitialized = false;
    }
}
