#pragma once

// Pure predicate for a per-camera recurring daily "quiet hours" window
// (CameraConfig::quietHoursEnabled/quietStartMinute/quietEndMinute,
// telegram.cpp's triggerMotionAlert) - whether motion alerts should be
// suppressed right now. Minutes are minutes-since-local-midnight (0-1439).
//
// start == end (the natural pre-filled 00:00/00:00 state before anyone
// touches the time fields on the dashboard) deliberately returns false -
// "no active window", not "always quiet". Treating a zero-width window as
// "always quiet" would make checking the enable box alone, without ever
// touching the time fields, silently and permanently kill every motion
// alert for that camera - the wrong default for a security feature.
//
// start < end is a same-day window (e.g. 09:00-17:00). start > end wraps
// past midnight (e.g. 22:00-06:00).
bool isWithinQuietHours(int nowMinuteOfDay, int startMinute, int endMinute);
