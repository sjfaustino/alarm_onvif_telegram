#pragma once

// Periodic checks run from loop(). Each check alerts once when its threshold
// is crossed and re-arms once it recovers (heap: once per boot).
void sendHeartbeat();
void checkNvsUsage();
void checkSdUsage();
void checkWifiSignal();
void checkHeapHealth(); // every tick, not interval-gated
