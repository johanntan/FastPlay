// macOS speech. While VoiceOver is running and FastPlay is the active app, text is
// given to VoiceOver as an announcement, so it is spoken in the user's voice and
// settings. Otherwise (VoiceOver off, or FastPlay in the background, where
// VoiceOver ignores its announcements) the system voice speaks it, as SAPI does on
// Windows when no screen reader is running.

#include "accessibility.h"
#include "app_ui.h"
#include "utils.h"

#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>

#include <mutex>
#include <string>

static std::mutex g_speechMutex;
static std::wstring g_pendingSpeechW;
static bool g_speechInterrupt = true;
static bool g_speechInitialized = false;
static AVSpeechSynthesizer* g_synthesizer = nil;

static void StopSynthesizer() {
    if (g_synthesizer && g_synthesizer.speaking) {
        [g_synthesizer stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
    }
}

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

    NSString* message = [NSString stringWithUTF8String:WideToUtf8(text).c_str()];
    if (message.length == 0) return;

    if ([NSWorkspace sharedWorkspace].voiceOverEnabled && NSApp.active) {
        StopSynthesizer();
        NSDictionary* info = @{
            NSAccessibilityAnnouncementKey: message,
            NSAccessibilityPriorityKey: @(interrupt ? NSAccessibilityPriorityHigh : NSAccessibilityPriorityMedium),
        };
        id element = NSApp.keyWindow ?: NSApp.mainWindow;
        if (!element) element = NSApp;
        NSAccessibilityPostNotificationWithUserInfo(element, NSAccessibilityAnnouncementRequestedNotification, info);
        return;
    }

    if (!g_synthesizer) g_synthesizer = [[AVSpeechSynthesizer alloc] init];
    if (interrupt) StopSynthesizer();
    [g_synthesizer speakUtterance:[AVSpeechUtterance speechUtteranceWithString:message]];
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
        g_speechInitialized = false;
        // Called on the UI thread as FastPlay closes.
        StopSynthesizer();
        g_synthesizer = nil;
    }
}
