#pragma once
#include <Arduino.h>
#include <vector>
#include "camera.h" // CameraConfig, CameraState

// The single place that decides "SD or PSRAM ring" for a camera's
// snapshot history, so telegram.cpp/webserver.cpp/webserver_cameras.cpp
// don't each duplicate that branch. Neither backing store knows about the
// other: sd_store.h/.cpp owns the SD-specific mechanics, CameraState's
// own snapshotHistory ring (camera.h) is the PSRAM fallback, exactly as
// it worked before SD support existed. This module is what chooses
// between them, once, in one place.

// Takes ownership of jpg (caller must not free() it after this call).
// Dispatches to sd_store's writeSdSnapshot() if sdActive() (sd_store.h),
// else falls back to the existing PSRAM ring - unchanged, including its
// own free-PSRAM safety check. source (snapshot_source.h) is required,
// not defaulted - every call site (telegram.cpp) has a real answer for
// "why was this captured" (motion/person/vehicle/pet, tamper, timelapse,
// a manual test alert, or an on-demand /snap), and a silent default would
// too easily hide a forgotten update at a new call site.
void pushCameraSnapshot(const CameraConfig& cfg, CameraState& st, uint8_t* jpg, size_t jpgLen,
                         SnapshotSource source);

// How many snapshots are currently available for this camera, wherever
// they live.
size_t cameraSnapshotCount(const CameraConfig& cfg, CameraState& st);

// Reads the age-th most recent snapshot (0 = newest) into a freshly
// allocated buffer (PSRAM-preferred) - caller must free() it on success.
// No per-entry identifier is returned: the dashboard's cache-busting
// query param (webserver_cameras.cpp's Preview column) uses a single
// shared render-time value for every thumbnail on a page load instead,
// which is simpler and achieves the same goal (the browser never shows a
// stale image across page loads) without needing this module to expose a
// value that's otherwise unused.
bool readCameraSnapshot(const CameraConfig& cfg, CameraState& st, size_t age, uint8_t** outBuf, size_t* outLen);

// Returns just the SnapshotSource tag for the age-th most recent snapshot
// (0 = newest), without fetching its JPEG bytes - lets a caller building a
// grid of N thumbnails (webserver_gallery.cpp, webserver_cameras.cpp's
// Preview column) show what triggered each one without paying for N full
// image reads just to label them. Returns SnapshotSource::Motion (the
// safe generic default) if age is out of range.
SnapshotSource cameraSnapshotSourceAt(const CameraConfig& cfg, CameraState& st, size_t age);

// Returns every stored snapshot's SnapshotSource at once, newest-first
// (index i == age i) - one bulk fetch instead of calling
// cameraSnapshotSourceAt once per age, which for the SD backend would
// mean re-listing and re-sorting this camera's whole directory (up to
// SD_MAX_FILES_PER_CAMERA entries) from scratch for every single entry.
// Built for the Gallery page's per-kind filter/counts (webserver_gallery.cpp),
// which needs to inspect every entry anyway.
std::vector<SnapshotSource> cameraSnapshotSourcesAll(const CameraConfig& cfg, CameraState& st);
