#include "diagnostics.h"
#include "build_version.h"
#include "config.h"
#include "heap_health.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

static const char* NVS_NAMESPACE = "diag";
static const char* NVS_KEY_HEAP_LOW = "heapLow";

// ---- Failed allocations ----

static const size_t ALLOC_FAILURE_RING = 8;
static AllocFailure g_allocFailures[ALLOC_FAILURE_RING];
static uint32_t g_allocFailureCount = 0;
static portMUX_TYPE g_allocFailureMux = portMUX_INITIALIZER_UNLOCKED;

// Runs in whichever task's allocation failed, possibly with little heap left, so
// it only copies into static storage.
static void onAllocFailed(size_t size, uint32_t caps, const char* function) {
  bool inIsr = xPortInIsrContext();
  const char* task = (!inIsr && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) ? pcTaskGetName(nullptr) : "-";
  if (inIsr) portENTER_CRITICAL_ISR(&g_allocFailureMux);
  else portENTER_CRITICAL(&g_allocFailureMux);
  AllocFailure& f = g_allocFailures[g_allocFailureCount % ALLOC_FAILURE_RING];
  f.size = (uint32_t)size;
  f.caps = caps;
  f.atMs = (uint32_t)(esp_timer_get_time() / 1000);
  strlcpy(f.task, task, sizeof(f.task));
  f.function = function;
  g_allocFailureCount++;
  if (inIsr) portEXIT_CRITICAL_ISR(&g_allocFailureMux);
  else portEXIT_CRITICAL(&g_allocFailureMux);
}

size_t recentAllocFailures(AllocFailure* out, size_t maxCount, uint32_t* total) {
  portENTER_CRITICAL(&g_allocFailureMux);
  uint32_t count = g_allocFailureCount;
  size_t n = count < ALLOC_FAILURE_RING ? count : ALLOC_FAILURE_RING;
  if (n > maxCount) n = maxCount;
  for (size_t i = 0; i < n; i++) out[i] = g_allocFailures[(count - 1 - i) % ALLOC_FAILURE_RING];
  portEXIT_CRITICAL(&g_allocFailureMux);
  if (total) *total = count;
  return n;
}

// ---- Heap-low record ----

// Captured when the low is seen and saved later, so a burst of new lows costs one
// flash write. Buffers are in PSRAM so capturing needs no internal heap.
static const size_t HEAP_LOW_LOG_TAIL_BYTES = 1000;
static char* g_heapLowTail = nullptr;
static size_t g_heapLowTailLen = 0;
static bool g_heapLowPending = false;
static uint32_t g_heapLowMin = 0;
static uint32_t g_heapLowLargest = 0;
static uint32_t g_heapLowAtMs = 0;
static time_t g_heapLowEpoch = 0;
static unsigned long g_lastHeapLowSaveMs = 0;
static bool g_heapLowSaved = false;

static void saveHeapLowRecord() {
  JsonDocument doc;
  doc["minFreeHeap"] = g_heapLowMin;
  doc["largestBlock"] = g_heapLowLargest;
  doc["atUptimeMs"] = g_heapLowAtMs;
  doc["epoch"] = (long)g_heapLowEpoch;
  doc["firmware"] = FIRMWARE_VERSION;
  uint32_t total = 0;
  AllocFailure failures[3];
  size_t n = recentAllocFailures(failures, 3, &total);
  doc["allocFailuresSinceBoot"] = total;
  for (size_t i = 0; i < n; i++) {
    JsonObject f = doc["recentAllocFailures"].add<JsonObject>();
    f["size"] = failures[i].size;
    f["task"] = failures[i].task;
    f["atMs"] = failures[i].atMs;
  }
  if (g_heapLowTail) doc["logTail"] = String(g_heapLowTail);

  String json;
  serializeJson(doc, json);
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  if (prefs.putString(NVS_KEY_HEAP_LOW, json) == 0) {
    Serial.println("[diagnostics] Couldn't save the heap-low record to NVS.");
  }
  prefs.end();
}

void initDiagnostics() {
  heap_caps_register_failed_alloc_callback(onAllocFailed);
  g_heapLowTail = (char*)heap_caps_malloc(HEAP_LOW_LOG_TAIL_BYTES + 1, MALLOC_CAP_SPIRAM);
}

void diagnosticsOnHeapTick(uint32_t minFreeHeap, bool isNewLow) {
  if (isNewLow && minFreeHeap < HEAP_LOW_WARN_BYTES) {
    g_heapLowMin = minFreeHeap;
    g_heapLowLargest = ESP.getMaxAllocHeap();
    g_heapLowAtMs = millis();
    time_t now = time(nullptr);
    g_heapLowEpoch = now > 1500000000 ? now : 0; // 0 = clock not synced
    if (g_heapLowTail) {
      uint32_t end = logTeeEnd();
      uint32_t from = end > HEAP_LOW_LOG_TAIL_BYTES ? end - HEAP_LOW_LOG_TAIL_BYTES : 0;
      g_heapLowTailLen = logTeeRead(from, g_heapLowTail, HEAP_LOW_LOG_TAIL_BYTES, nullptr, nullptr);
      g_heapLowTail[g_heapLowTailLen] = 0;
    }
    g_heapLowPending = true;
  }
  unsigned long sinceSave = g_heapLowSaved ? millis() - g_lastHeapLowSaveMs : HEAP_LOW_SAVE_GAP_MS;
  if (shouldPersistHeapLow(g_heapLowPending, sinceSave, HEAP_LOW_SAVE_GAP_MS)) {
    saveHeapLowRecord();
    g_heapLowPending = false;
    g_heapLowSaved = true;
    g_lastHeapLowSaveMs = millis();
  }
}

String loadHeapLowRecord() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, false);
  String json = prefs.isKey(NVS_KEY_HEAP_LOW) ? prefs.getString(NVS_KEY_HEAP_LOW, "") : "";
  prefs.end();
  return json;
}
