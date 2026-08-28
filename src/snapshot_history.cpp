#include "snapshot_history.h"
#include "sd_store.h"
#include "config.h"
#include <esp_heap_caps.h>
#include <cstring>

// ============================================================
// PSRAM ring fallback - unchanged from before SD support existed
// (including its own free-PSRAM safety check, added when history was
// first introduced), just moved here from telegram.cpp and given
// internal linkage.
// ============================================================

static void pushRamSnapshot(const CameraConfig& cfg, CameraState& st, uint8_t* jpg, size_t jpgLen) {
  size_t freePsram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (freePsram < SNAPSHOT_MAX_BYTES_PSRAM + jpgLen) {
    Serial.printf("[%s] Skipping snapshot history retention - PSRAM getting low (%u bytes free).\n",
                  cfg.name.c_str(), (unsigned)freePsram);
    free(jpg);
    return;
  }

  uint8_t* old = nullptr;
  {
    CameraStateLock lock(st);
    size_t idx = st.snapshotHistoryNext;
    old = st.snapshotHistory[idx].jpg;
    st.snapshotHistory[idx].jpg = jpg;
    st.snapshotHistory[idx].len = jpgLen;
    st.snapshotHistory[idx].ms = millis();
    st.snapshotHistoryNext = (idx + 1) % SNAPSHOT_HISTORY_SIZE;
    if (st.snapshotHistoryCount < SNAPSHOT_HISTORY_SIZE) st.snapshotHistoryCount++;
  }
  free(old);
}

static size_t ramSnapshotCount(CameraState& st) {
  CameraStateLock lock(st);
  return st.snapshotHistoryCount;
}

static bool readRamSnapshot(CameraState& st, size_t age, uint8_t** outBuf, size_t* outLen) {
  size_t len = 0;
  {
    CameraStateLock lock(st);
    if (age >= st.snapshotHistoryCount) return false;
    size_t ringIdx = (st.snapshotHistoryNext + SNAPSHOT_HISTORY_SIZE - 1 - age) % SNAPSHOT_HISTORY_SIZE;
    len = st.snapshotHistory[ringIdx].len;
    if (len == 0) return false;

    uint8_t* copy = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!copy) copy = (uint8_t*)malloc(len);
    if (!copy) return false;
    memcpy(copy, st.snapshotHistory[ringIdx].jpg, len);
    *outBuf = copy;
  }
  *outLen = len;
  return true;
}

// ============================================================
// Dispatch
// ============================================================

void pushCameraSnapshot(const CameraConfig& cfg, CameraState& st, uint8_t* jpg, size_t jpgLen) {
  if (sdActive()) {
    // writeSdSnapshot blocks on sd_store.cpp's internal mutex, which a
    // concurrent full storage check (checkSnapshotStorage, when the
    // automatic periodic check is enabled) can hold for a long walk - this
    // camera task can't reach checkCameraOnlineStatus (camera.cpp) again
    // until this call returns. Without the compensation below, that purely
    // SD-internal delay would silently count against st.lastContactMs and
    // could trip a false OFFLINE alert with nothing actually wrong with
    // the camera - lastContactMs already reflects the camera's real last
    // response (set in cameraSoapCall, before this write ever started), so
    // advancing it by exactly how long this call was blocked just excludes
    // that unrelated delay from "how long has this camera been silent,"
    // rather than fabricating fresh contact that didn't happen.
    unsigned long before = millis();
    writeSdSnapshot(cfg, jpg, jpgLen); // takes ownership regardless of outcome - see its own comment
    unsigned long blockedMs = millis() - before;
    // Lock-guarded - this function is reachable from loop()'s task too
    // (sendOnDemandSnapshot, via /snap or handleAllCamerasCommand), not
    // just the owning camera's own task. See cameraSoapCall's (camera.cpp)
    // comment for why lastContactMs itself is lock-guarded now.
    { CameraStateLock lock(st); st.lastContactMs += blockedMs; }
    return;
  }
  pushRamSnapshot(cfg, st, jpg, jpgLen);
}

size_t cameraSnapshotCount(const CameraConfig& cfg, CameraState& st) {
  if (sdActive()) return sdSnapshotCount(cfg);
  return ramSnapshotCount(st);
}

bool readCameraSnapshot(const CameraConfig& cfg, CameraState& st, size_t age, uint8_t** outBuf, size_t* outLen) {
  if (sdActive()) return readSdSnapshot(cfg, age, outBuf, outLen);
  return readRamSnapshot(st, age, outBuf, outLen);
}
