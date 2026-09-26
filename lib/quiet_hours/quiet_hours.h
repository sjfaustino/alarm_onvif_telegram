#pragma once

// Whether a camera's daily quiet window is active (minutes since local
// midnight). start == end (the 00:00/00:00 default) is never quiet, so ticking
// the box alone can't silence a camera. start > end wraps midnight.
bool isWithinQuietHours(int nowMinuteOfDay, int startMinute, int endMinute);
