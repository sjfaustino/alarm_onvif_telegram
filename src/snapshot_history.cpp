#include "snapshot_history.h"
#include "sd_store.h"
#include "config.h"
#include <esp_heap_caps.h>
#include <cstring>

// ============================================================
// PSRAM ring fallback
// ============================================================

static void pushRamSnapshot(const CameraConfig& cfg, CameraState& st, uint8_t* jpg, size_t jpgLen,
                             SnapshotSource source) {
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
    st.snapshotHistory[idx].source = source;
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

static SnapshotSource ramSnapshotSourceAt(CameraState& st, size_t age) {
  CameraStateLock lock(st);
  if (age >= st.snapshotHistoryCount) return SnapshotSource::Motion;
  size_t ringIdx = (st.snapshotHistoryNext + SNAPSHOT_HISTORY_SIZE - 1 - age) % SNAPSHOT_HISTORY_SIZE;
  return st.snapshotHistory[ringIdx].source;
}

static std::vector<SnapshotSource> ramSnapshotSourcesAll(CameraState& st) {
  std::vector<SnapshotSource> sources;
  CameraStateLock lock(st);
  sources.reserve(st.snapshotHistoryCount);
  for (size_t age = 0; age < st.snapshotHistoryCount; age++) {
    size_t ringIdx = (st.snapshotHistoryNext + SNAPSHOT_HISTORY_SIZE - 1 - age) % SNAPSHOT_HISTORY_SIZE;
    sources.push_back(st.snapshotHistory[ringIdx].source);
  }
  return sources;
}

// No dates: the ring only has boot-relative millis().
static std::vector<SnapshotEntryInfo> ramSnapshotEntriesAll(CameraState& st) {
  std::vector<SnapshotEntryInfo> entries;
  CameraStateLock lock(st);
  entries.reserve(st.snapshotHistoryCount);
  for (size_t age = 0; age < st.snapshotHistoryCount; age++) {
    size_t ringIdx = (st.snapshotHistoryNext + SNAPSHOT_HISTORY_SIZE - 1 - age) % SNAPSHOT_HISTORY_SIZE;
    SnapshotEntryInfo info;
    info.source = st.snapshotHistory[ringIdx].source;
    entries.push_back(info);
  }
  return entries;
}

// ============================================================
// Dispatch
// ============================================================

void pushCameraSnapshot(const CameraConfig& cfg, CameraState& st, uint8_t* jpg, size_t jpgLen,
                         SnapshotSource source) {
  if (sdActive()) {
    // The SD write can block behind a long storage check. Advance
    // lastContactMs by the time blocked, so SD delays don't count as camera
    // silence (a false OFFLINE).
    unsigned long before = millis();
    writeSdSnapshot(cfg, jpg, jpgLen, source); // takes ownership regardless of outcome - see its own comment
    unsigned long blockedMs = millis() - before;
    // Locked: also reached from loop()'s task via /snap.
    { CameraStateLock lock(st); st.lastContactMs += blockedMs; }
    return;
  }
  pushRamSnapshot(cfg, st, jpg, jpgLen, source);
}

size_t cameraSnapshotCount(const CameraConfig& cfg, CameraState& st) {
  if (sdActive()) return sdSnapshotCount(cfg);
  return ramSnapshotCount(st);
}

bool readCameraSnapshot(const CameraConfig& cfg, CameraState& st, size_t age, uint8_t** outBuf, size_t* outLen) {
  if (sdActive()) return readSdSnapshot(cfg, age, outBuf, outLen);
  return readRamSnapshot(st, age, outBuf, outLen);
}

SnapshotSource cameraSnapshotSourceAt(const CameraConfig& cfg, CameraState& st, size_t age) {
  if (sdActive()) return sdSnapshotSourceAt(cfg, age);
  return ramSnapshotSourceAt(st, age);
}

std::vector<SnapshotSource> cameraSnapshotSourcesAll(const CameraConfig& cfg, CameraState& st) {
  if (sdActive()) return sdSnapshotSourcesAll(cfg);
  return ramSnapshotSourcesAll(st);
}

std::vector<SnapshotEntryInfo> cameraSnapshotEntriesAll(const CameraConfig& cfg, CameraState& st) {
  if (sdActive()) return sdSnapshotEntriesAll(cfg);
  return ramSnapshotEntriesAll(st);
}
