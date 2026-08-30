#pragma once
#include <Arduino.h>

// Storage panel content (SD card status, enable/disable toggle, a
// readability check, and an erase-all-history action). Split out of
// webserver.cpp - see webserver_network.h's comment for why. Backed by
// sd_store.h for the actual SD mechanics; this file only renders/parses
// the form - both maintenance actions go through a background-job wrapper
// below (startStorageCheckAsync/startEraseAllAsync), not sd_store.h
// directly, since both walk every camera's snapshot directory and are, by
// sd_store.cpp's own comment on checkSnapshotStorage, "unbounded by
// design."
String renderStoragePanel();

// ============================================================
// Background wrappers for both Maintenance actions - sd_store.cpp's
// checkSnapshotStorage()/eraseAllSnapshots() each walk every stored
// snapshot file across every camera's directory and are, per
// checkSnapshotStorage's own comment, "unbounded by design" - can run
// long enough that main.cpp's loop() (checkSnapshotStorage's automatic
// periodic caller) resets the task watchdog per file to survive it.
// Called synchronously from a PsychicHttp request handler, that same
// unbounded walk blocks the entire dashboard for everyone, not just
// whoever clicked the button - PsychicHttp here services one request at a
// time (no async worker pool), the exact reasoning webserver_cameras.h's
// startTestAllCamerasAsync documents. Not being subscribed to the task
// watchdog (true - only loop()'s task is) doesn't change that.
// eraseAllSnapshots has no watchdog resets of its own precisely because it
// never had an automatic caller forcing that question to be asked - not
// evidence it's any faster.
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

// Starts eraseAllSnapshots() (sd_store.h) on a background FreeRTOS task
// instead of the calling task. A no-op (doesn't start a second overlapping
// run) if an erase is already in progress. Call this from the
// /storage/erase route handler.
void startEraseAllAsync();

// Renders the current erase status: "erasing in the background" while one
// is in progress, the last completed run's result once one exists, or ""
// if no erase has ever run this boot. Safe to call from any task
// (internally locked) - renderStoragePanel calls this itself.
String renderEraseAllStatus();
