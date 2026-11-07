#pragma once

// Periodic checks run from loop(). Each check alerts once when its threshold
// is crossed and re-arms once it recovers (heap: once per boot).
void sendHeartbeat();
void checkNvsUsage();
void checkSdUsage();
void checkWifiSignal();
void checkHeapHealth(); // every tick, not interval-gated
// Alerts if a camera task stops completing loop passes, e.g. stuck forever on a
// mutex. Alert only, no reboot: this can't tell a wedged task from a very slow
// send, and a false reboot would blind every camera.
void checkCameraTasks();
