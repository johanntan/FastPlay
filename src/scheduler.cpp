// Scheduled events: running them when their time comes, stopping them when their
// duration runs out, and working out when a repeating event runs next.

#include "scheduler.h"
#include "database.h"
#include "globals.h"
#include "player.h"
#include "accessibility.h"
#include "utils.h"
#include "app_ui.h"

#include <ctime>
#include <string>
#include <vector>

// Scheduled event duration tracking
static ScheduleStopAction g_pendingStopAction = ScheduleStopAction::StopBoth;
static bool g_schedulerMuted = false;  // Track if we muted for scheduled recording

// Calculate next schedule time for repeating events
void CalculateNextScheduleTime(int id, int64_t lastRun, ScheduleRepeat repeat) {
    if (repeat == ScheduleRepeat::None) {
        // One-time event, just disable it
        UpdateScheduledEventEnabled(id, false);
        return;
    }

    time_t now = time(nullptr);
    struct tm tm;
    localtime_s(&tm, &now);

    // Start from the current scheduled time and add appropriate interval
    int64_t nextTime = lastRun;

    switch (repeat) {
        case ScheduleRepeat::Daily:
            nextTime += 24 * 60 * 60;  // Add 1 day
            break;

        case ScheduleRepeat::Weekly:
            nextTime += 7 * 24 * 60 * 60;  // Add 1 week
            break;

        case ScheduleRepeat::Weekdays: {
            // Find next weekday
            time_t t = static_cast<time_t>(nextTime);
            do {
                t += 24 * 60 * 60;
                localtime_s(&tm, &t);
            } while (tm.tm_wday == 0 || tm.tm_wday == 6);  // Skip Sun=0, Sat=6
            nextTime = static_cast<int64_t>(t);
            break;
        }

        case ScheduleRepeat::Weekends: {
            // Find next weekend day
            time_t t = static_cast<time_t>(nextTime);
            do {
                t += 24 * 60 * 60;
                localtime_s(&tm, &t);
            } while (tm.tm_wday != 0 && tm.tm_wday != 6);  // Find Sun=0 or Sat=6
            nextTime = static_cast<int64_t>(t);
            break;
        }

        case ScheduleRepeat::Monthly: {
            // Same day next month
            time_t t = static_cast<time_t>(nextTime);
            localtime_s(&tm, &t);
            tm.tm_mon += 1;
            if (tm.tm_mon > 11) {
                tm.tm_mon = 0;
                tm.tm_year += 1;
            }
            nextTime = static_cast<int64_t>(mktime(&tm));
            break;
        }

        default:
            break;
    }

    UpdateScheduledEventTime(id, nextTime);
}

// Handle scheduled duration timer expiry
void HandleScheduledDurationEnd() {
    // Restore mute state if we muted for scheduled recording
    if (g_schedulerMuted) {
        g_muted = false;
        g_schedulerMuted = false;
    }

    switch (g_pendingStopAction) {
        case ScheduleStopAction::StopBoth:
            Stop();
            if (g_isRecording) {
                StopRecording();
            }
            Speak("Scheduled event ended");
            break;
        case ScheduleStopAction::StopPlayback:
            Stop();
            Speak("Scheduled playback ended");
            break;
        case ScheduleStopAction::StopRecording:
            if (g_isRecording) {
                StopRecording();
                Speak("Scheduled recording ended");
            }
            break;
    }
}

// Check for and execute scheduled events
void CheckScheduledEvents() {
    std::vector<ScheduledEvent> pending = GetPendingScheduledEvents();

    for (const auto& ev : pending) {
        // Mark as run immediately to prevent re-triggering
        int64_t now = static_cast<int64_t>(time(nullptr));
        UpdateScheduledEventLastRun(ev.id, now);

        // Execute the action
        bool shouldPlay = (ev.action == ScheduleAction::Playback || ev.action == ScheduleAction::Both);
        bool shouldRecord = (ev.action == ScheduleAction::Recording || ev.action == ScheduleAction::Both);

        // Load the source
        if (shouldPlay || shouldRecord) {
            g_playlist.clear();
            g_playlist.push_back(ev.sourcePath);

            // For Recording-only mode, mute playback so user doesn't hear it
            // but the stream still plays (required for encoder to capture audio)
            if (shouldRecord && !shouldPlay) {
                g_muted = true;
                g_schedulerMuted = true;
            }

            // Always play the track first - recording requires audio to flow through the stream
            PlayTrack(0);

            // Start recording AFTER playback begins (requires active stream)
            if (shouldRecord && !g_isRecording) {
                ToggleRecording();
            }

            // Set up duration timer if specified
            if (ev.duration > 0) {
                g_pendingStopAction = ev.stopAction;
                // Set timer for duration (minutes to milliseconds)
                StartScheduleDurationTimer(ev.duration * 60 * 1000);
            }
        }

        // Speak announcement
        std::wstring msg = L"Scheduled event: " + ev.name;
        Speak(WideToUtf8(msg).c_str());

        // Calculate next run time for repeating events
        CalculateNextScheduleTime(ev.id, ev.scheduledTime, ev.repeat);
    }
}
