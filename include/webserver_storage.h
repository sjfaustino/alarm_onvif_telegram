#pragma once
#include <Arduino.h>

// Storage panel content (SD card status, enable/disable toggle, a
// readability check, and an erase-all-history action). Split out of
// webserver.cpp - see webserver_network.h's comment for why. Backed by
// sd_store.h for the actual SD mechanics; this file only renders/parses
// the form and the two maintenance-action buttons - webserver.cpp's
// /storage/erase route calls sd_store.h directly and passes the result
// back through renderShell's banner (fast - a single directory removal),
// but /storage/check goes through startStorageCheckAsync/
// renderStorageCheckStatus below instead, same reasoning as
// webserver_cameras.h's startTestAllCamerasAsync.
String renderStoragePanel();

// ============================================================
// "Check storage" background wrapper - sd_store.cpp's checkSnapshotStorage()
// walks every stored snapshot file across every camera's directory and is,
// by its own comment, "unbounded by design" - can run long enough that
// main.cpp's loop() (the automatic periodic check) resets the task
// watchdog per file to survive it. Called synchronously from a PsychicHttp
// request handler, that same unbounded walk blocks the entire dashboard
// for everyone, not just whoever clicked the button - PsychicHttp here
// services one request at a time (no async worker pool), the exact
// reasoning webserver_cameras.h's startTestAllCamerasAsync documents.
// Not being subscribed to the task watchdog (true - only loop()'s task
// is) doesn't change that.
// ============================================================

// Starts checkSnapshotStorage() (sd_store.h) on a background FreeRTOS task
// instead of the calling task. A no-op (doesn't start a second overlapping
// run) if a check is already in progress. Call this from the
// /storage/check route handler.
void startStorageCheckAsync();

// Renders the current check status: "checking in the background" while
// one is in progress, the last completed run's result once one exists, or
// "" if no check has ever run this boot. Safe to call from any task
// (internally locked) - renderStoragePanel calls this itself, so it shows
// up on a normal page load too, not just right after clicking the button.
String renderStorageCheckStatus();
