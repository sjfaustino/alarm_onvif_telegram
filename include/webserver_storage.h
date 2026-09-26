#pragma once
#include <Arduino.h>
#include "background_job.h" // BackgroundJobStartOutcome

// Storage panel: SD status, enable toggle, readability check, erase-all.
String renderStoragePanel();

// ============================================================
// Both maintenance actions walk every stored file (unbounded), so they run on
// a background task; PsychicHttp serves one request at a time.
// ============================================================

// Starts checkSnapshotStorage on a task; reports if one is already running.
BackgroundJobStartOutcome startStorageCheckAsync();

// Check status HTML: running, last result, or "".
String renderStorageCheckStatus();

// Starts eraseAllSnapshots on a task; reports if one is already running.
BackgroundJobStartOutcome startEraseAllAsync();

String renderEraseAllStatus();

// True while either job runs, so the page auto-refreshes.
bool storageJobsInProgress();
