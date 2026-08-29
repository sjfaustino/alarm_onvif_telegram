#pragma once

// Pure predicate for a per-camera recurring daily "quiet hours" window
// (CameraConfig::quietHoursEnabled/quietStartMinute/quietEndMinute,
// telegram.cpp's triggerMotionAlert) - whether motion alerts should be
// suppressed right now. Minutes are minutes-since-local-midnight (0-1439).
//
// start == end (the pre-filled 00:00/00:00 default) returns false, not
// true - a zero-width window meaning "always quiet" would make checking
// the enable box alone, without touching the time fields, silently kill
// every motion alert for that camera. Wrong default for a security feature.
//
// start < end is a same-day window (09:00-17:00); start > end wraps past
// midnight (22:00-06:00).
bool isWithinQuietHours(int nowMinuteOfDay, int startMinute, int endMinute);
