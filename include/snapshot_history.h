#pragma once
#include <Arduino.h>
#include <vector>
#include "camera.h" // CameraConfig, CameraState

// The one place that picks SD or the PSRAM ring for a camera's snapshot
// history; neither backend knows about the other.

// Takes ownership of jpg. Writes to SD if active, else the PSRAM ring. source
// has no default so a new call site can't silently mislabel.
void pushCameraSnapshot(const CameraConfig& cfg, CameraState& st, uint8_t* jpg, size_t jpgLen,
                         SnapshotSource source);

size_t cameraSnapshotCount(const CameraConfig& cfg, CameraState& st);

// age 0 = newest. Caller free()s *outBuf on success.
bool readCameraSnapshot(const CameraConfig& cfg, CameraState& st, size_t age, uint8_t** outBuf, size_t* outLen);

// Source tag only, without reading the JPEG (for labelling thumbnails). Motion
// if out of range.
SnapshotSource cameraSnapshotSourceAt(const CameraConfig& cfg, CameraState& st, size_t age);

// All sources newest-first in one pass (the SD backend would otherwise re-list
// the directory per entry).
std::vector<SnapshotSource> cameraSnapshotSourcesAll(const CameraConfig& cfg, CameraState& st);

// As above plus capture dates, SD only ("" for the ring, which has no wall
// clock).
std::vector<SnapshotEntryInfo> cameraSnapshotEntriesAll(const CameraConfig& cfg, CameraState& st);
