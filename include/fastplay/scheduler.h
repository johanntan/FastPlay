#pragma once
#ifndef FASTPLAY_SCHEDULER_H
#define FASTPLAY_SCHEDULER_H

// Scheduled events: starting playback or recording at a set time, and stopping it
// after a duration. The main window calls CheckScheduledEvents() once a minute.

#include <cstdint>
#include "database.h"

void CheckScheduledEvents();

// The duration of a started event has run out (see StartScheduleDurationTimer()).
void HandleScheduledDurationEnd();

// Work out and store when event `id` should next run after running at `lastRun`.
void CalculateNextScheduleTime(int id, int64_t lastRun, ScheduleRepeat repeat);

#endif // FASTPLAY_SCHEDULER_H
