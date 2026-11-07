#include "webserver_internal.h"
#include "boot_checks.h" // describeResetReason
#include "build_version.h"
#include "camera_tasks.h"
#include "debug_token.h"
#include "diagnostics.h"
#include "secrets.h"
#include <ArduinoJson.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <algorithm>
#include <map>
#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// /debug/*: remote diagnostics. Logs and crash dumps can contain passwords, and
// the dashboard login may be unset, so these need their own token: DEBUG_TOKEN in
// secrets.h, sent as an X-Debug-Token header. Without one, the routes are off.
#ifndef DEBUG_TOKEN
#define DEBUG_TOKEN ""
#endif

static const size_t LOG_RESPONSE_MAX_BYTES = 16UL * 1024UL;

static bool authorized(PsychicRequest* request) {
  return debugTokenValid(DEBUG_TOKEN, request->header("X-Debug-Token").c_str());
}

static const char* taskStateName(eTaskState state) {
  switch (state) {
    case eRunning:   return "running";
    case eReady:     return "ready";
    case eBlocked:   return "blocked";
    case eSuspended: return "suspended";
    case eDeleted:   return "deleted";
    default:         return "invalid";
  }
}

static void addTasks(JsonDocument& doc) {
  UBaseType_t capacity = uxTaskGetNumberOfTasks() + 4; // tasks may start while this runs
  TaskStatus_t* tasks = (TaskStatus_t*)heap_caps_malloc(capacity * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
  if (!tasks) return;
  UBaseType_t count = uxTaskGetSystemState(tasks, capacity, nullptr);
  for (UBaseType_t i = 0; i < count; i++) {
    JsonObject t = doc["tasks"].add<JsonObject>();
    t["name"] = String(tasks[i].pcTaskName); // copied: a task could exit before this is sent
    t["state"] = taskStateName(tasks[i].eCurrentState);
    t["priority"] = tasks[i].uxCurrentPriority;
    t["stackFreeMin"] = tasks[i].usStackHighWaterMark; // bytes on this port
  }
  heap_caps_free(tasks);
}

static void addCameras(JsonDocument& doc) {
  for (size_t i = 0; i < g_cameras.size(); i++) {
    JsonObject c = doc["cameras"].add<JsonObject>();
    c["name"] = g_cameras[i].name;
    c["enabled"] = g_cameras[i].enabled;
    if (!g_cameras[i].enabled) continue; // disabled slots may have no mutex yet
    CameraState& st = g_cameraStates[i];
    CameraStateLock lock(st);
    unsigned long now = millis();
    c["taskRunning"] = st.taskRunning;
    c["msSinceBeat"] = now - st.lastTaskBeatMs;
    c["subscribed"] = st.subscriptionActive;
    c["offline"] = st.isOffline;
    c["msSinceContact"] = now - st.lastContactMs;
    c["totalReconnects"] = st.totalReconnects;
    c["alertsEnabled"] = st.alertsEnabled;
  }
}

static void addCoreDump(JsonDocument& doc) {
  JsonObject out = doc["coreDump"].to<JsonObject>();
  size_t addr = 0, size = 0;
  bool present = esp_core_dump_image_check() == ESP_OK && esp_core_dump_image_get(&addr, &size) == ESP_OK;
  out["present"] = present;
  if (!present) return;
  out["size"] = size;
  esp_core_dump_summary_t* s = (esp_core_dump_summary_t*)heap_caps_malloc(sizeof(*s), MALLOC_CAP_SPIRAM);
  if (s && esp_core_dump_get_summary(s) == ESP_OK) {
    out["task"] = String(s->exc_task);
    char pc[11];
    snprintf(pc, sizeof(pc), "0x%08lx", (unsigned long)s->exc_pc);
    out["pc"] = pc;
    out["elfSha256"] = String((const char*)s->app_elf_sha256);
    for (uint32_t i = 0; i < s->exc_bt_info.depth; i++) {
      snprintf(pc, sizeof(pc), "0x%08lx", (unsigned long)s->exc_bt_info.bt[i]);
      out["backtrace"].add(pc);
    }
  }
  if (s) heap_caps_free(s);
}

// NVS usage per namespace, and the largest values: NVS is small (20 KB), and a
// full one fails saves. Entries are 32 bytes; a string or blob takes one per 32
// bytes plus one, roughly.
static void addNvs(JsonDocument& doc) {
  JsonObject out = doc["nvs"].to<JsonObject>();
  nvs_stats_t stats;
  if (nvs_get_stats(NULL, &stats) == ESP_OK) {
    out["totalEntries"] = stats.total_entries;
    out["usedEntries"] = stats.used_entries;
    out["freeEntries"] = stats.free_entries;
    out["availableEntries"] = stats.available_entries; // free, minus the page kept for compaction
  }
  struct Usage { size_t keys = 0, bytes = 0, entries = 0; };
  std::map<String, Usage> byNamespace;
  struct Value { String ns, key; size_t bytes; };
  std::vector<Value> values;
  nvs_iterator_t it = nullptr;
  esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
  while (res == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    size_t len = 0;
    if (info.type == NVS_TYPE_STR || info.type == NVS_TYPE_BLOB) {
      nvs_handle_t h;
      if (nvs_open(info.namespace_name, NVS_READONLY, &h) == ESP_OK) {
        if (info.type == NVS_TYPE_STR) nvs_get_str(h, info.key, NULL, &len);
        else nvs_get_blob(h, info.key, NULL, &len);
        nvs_close(h);
      }
      values.push_back({info.namespace_name, info.key, len});
    }
    Usage& u = byNamespace[info.namespace_name];
    u.keys++;
    u.bytes += len;
    u.entries += 1 + (len + 31) / 32;
    res = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);
  for (auto& kv : byNamespace) {
    JsonObject n = out["namespaces"][kv.first].to<JsonObject>();
    n["keys"] = kv.second.keys;
    n["bytes"] = kv.second.bytes;
    n["approxEntries"] = kv.second.entries;
  }
  std::sort(values.begin(), values.end(), [](const Value& a, const Value& b) { return a.bytes > b.bytes; });
  for (size_t i = 0; i < values.size() && i < 10; i++) {
    JsonObject v = out["largest"].add<JsonObject>();
    v["namespace"] = values[i].ns;
    v["key"] = values[i].key;
    v["bytes"] = values[i].bytes;
  }
}

void registerDebugRoutes(PsychicHttpServer& server) {
  server.on("/debug/status", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    if (!authorized(request)) return response->send(403, "text/plain", "Forbidden");
    JsonDocument doc;
    doc["version"] = FIRMWARE_VERSION;
    doc["uptimeMs"] = millis();
    doc["resetReason"] = describeResetReason();
    JsonObject heap = doc["heap"].to<JsonObject>();
    heap["free"] = ESP.getFreeHeap();
    heap["minFree"] = ESP.getMinFreeHeap();
    heap["largestBlock"] = ESP.getMaxAllocHeap();
    heap["psramFree"] = ESP.getFreePsram();
    heap["psramLargestBlock"] = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    static AllocFailure failures[8]; // static: the web server's stack is small, and it handles one request at a time
    uint32_t total = 0;
    size_t n = recentAllocFailures(failures, 8, &total);
    doc["allocFailuresSinceBoot"] = total;
    for (size_t i = 0; i < n; i++) {
      JsonObject f = doc["recentAllocFailures"].add<JsonObject>();
      f["size"] = failures[i].size;
      f["caps"] = failures[i].caps;
      f["atMs"] = failures[i].atMs;
      f["task"] = failures[i].task;
      f["function"] = failures[i].function ? failures[i].function : "";
    }
    String heapLow = loadHeapLowRecord();
    if (heapLow.length() > 0) doc["heapLowRecord"] = serialized(heapLow);

    addTasks(doc);
    addCameras(doc);
    addCoreDump(doc);
    addNvs(doc);

    String json;
    serializeJson(doc, json);
    return response->send(200, "application/json", json.c_str());
  });

  // ?since=<position>; X-Log-Next is the position to ask for next time, and
  // X-Log-Oldest shows whether anything was missed.
  server.on("/debug/log", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    if (!authorized(request)) return response->send(403, "text/plain", "Forbidden");
    uint32_t since = strtoul(request->getParam("since", "0").c_str(), nullptr, 10);
    char* buffer = (char*)heap_caps_malloc(LOG_RESPONSE_MAX_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (!buffer) return response->send(503, "text/plain", "Out of PSRAM.");
    uint32_t next = since, oldest = 0;
    size_t n = logTeeRead(since, buffer, LOG_RESPONSE_MAX_BYTES, &next, &oldest);
    buffer[n] = 0;
    response->addHeader("X-Log-Next", String(next).c_str());
    response->addHeader("X-Log-Oldest", String(oldest).c_str());
    esp_err_t result = response->send(200, "text/plain", buffer);
    heap_caps_free(buffer);
    return result;
  });

  // The raw ELF dump; decode it with the firmware.elf of the build that crashed.
  server.on("/debug/coredump", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    if (!authorized(request)) return response->send(403, "text/plain", "Forbidden");
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK) {
      return response->send(404, "text/plain", "No core dump stored.");
    }
    uint8_t* buffer = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buffer) return response->send(503, "text/plain", "Out of PSRAM.");
    const esp_partition_t* part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (!part || esp_partition_read(part, addr - part->address, buffer, size) != ESP_OK) {
      heap_caps_free(buffer);
      return response->send(500, "text/plain", "Couldn't read the core dump.");
    }
    response->addHeader("Content-Disposition", "attachment; filename=\"coredump.elf\"");
    esp_err_t result = response->send(200, "application/octet-stream", buffer, size);
    heap_caps_free(buffer);
    return result;
  });

  // Clears the stored dump so the next crash is unambiguous.
  server.on("/debug/coredump/erase", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    if (!authorized(request)) return response->send(403, "text/plain", "Forbidden");
    esp_err_t err = esp_core_dump_image_erase();
    return response->send(err == ESP_OK ? 200 : 500, "text/plain", err == ESP_OK ? "Erased." : "Erase failed.");
  });
}
