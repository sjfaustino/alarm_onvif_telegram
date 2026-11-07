#include "webserver_firmware.h"
#include "build_version.h" // FIRMWARE_VERSION
#include "config.h" // NVS_USAGE_WARN_PERCENT, HEAP_LOW_WARN_BYTES
#include "telegram_retry_queue.h" // getTelegramRetryQueueStatus
#include "format_utils.h" // formatUptime
#include "ota_history.h" // loadOtaHistory
#include <esp_ota_ops.h>
#include <esp_heap_caps.h> // heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
#include <nvs_flash.h>
#include <nvs.h>
#include <time.h>
#include "webserver_internal.h"
#include <Update.h>

// "" for epoch 0 (no timestamp recorded).
static String formatEpochLocal(time_t epoch) {
  if (epoch == 0) return "";
  struct tm tmStruct;
  localtime_r(&epoch, &tmStruct);
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmStruct);
  return String(buf);
}

String renderFirmwarePanel() {
  const esp_partition_t* running = esp_ota_get_running_partition();

  String html = "<h1>Firmware</h1>";
  html += "<table>";
  html += "<tr><th>Version</th><td>" + String(FIRMWARE_VERSION) + "</td></tr>";
  html += "<tr><th>Build</th><td>" + String(__DATE__) + " " + String(__TIME__) + "</td></tr>";
  html += "<tr><th>Running partition</th><td>" + String(running ? running->label : "?") + "</td></tr>";
  // No row until an OTA outcome has been recorded.
  OtaHistoryEntry otaHistory = loadOtaHistory();
  if (otaHistory.outcome != OtaOutcome::None) {
    String when = formatEpochLocal(otaHistory.epoch);
    String outcomeText = otaHistory.outcome == OtaOutcome::Healthy
        ? ("<span class=\"badge badge-on\">confirmed healthy</span>")
        : ("<span class=\"badge badge-offline\">rolled back</span>");
    html += "<tr><th>Last OTA update</th><td>" + outcomeText + " (partition " +
            htmlEscape(otaHistory.partitionLabel) + ")" +
            (when.length() > 0 ? " on " + when : "") + "</td></tr>";
  }
  html += "<tr><th>Sketch size</th><td>" + String(ESP.getSketchSize() / 1024) + " KB</td></tr>";
  html += "<tr><th>Free space for update</th><td>" + String(ESP.getFreeSketchSpace() / 1024) + " KB</td></tr>";

  // Internal RAM, where mbedTLS allocates. Largest block vs. total shows
  // fragmentation.
  html += "<tr><th>Free heap (internal)</th><td>" + String(ESP.getFreeHeap()) + " bytes</td></tr>";
  html += "<tr><th>Free heap - lifetime min</th><td>" + String(ESP.getMinFreeHeap()) + " bytes</td></tr>";
  html += "<tr><th>Largest allocatable block</th><td>" + String(ESP.getMaxAllocHeap()) + " bytes</td></tr>";
  html += "<tr><th>Free PSRAM</th><td>" +
          String((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) + " bytes</td></tr>";

  // oldestPendingMs is already a duration, so formatUptime fits.
  TelegramRetryQueueStatus retryStatus = getTelegramRetryQueueStatus();
  html += "<tr><th>Telegram retry queue</th><td>" +
          (retryStatus.count > 0
               ? String((unsigned)retryStatus.count) + " pending (oldest queued for " +
                     formatUptime(retryStatus.oldestPendingMs) + ")"
               : String("empty")) +
          "</td></tr>";

  // Percent of NVS entries (slots), not bytes. Records have silently failed to
  // save once NVS filled; checkNvsUsage also alerts at this threshold.
  nvs_stats_t nvsStats;
  if (nvs_get_stats(NULL, &nvsStats) == ESP_OK && nvsStats.total_entries > 0) {
    unsigned pct = (unsigned)((uint64_t)nvsStats.used_entries * 100 / nvsStats.total_entries);
    html += "<tr><th>NVS usage</th><td>" + String(pct) + "% (" + String((unsigned)nvsStats.used_entries) +
            " / " + String((unsigned)nvsStats.total_entries) + " entries, " +
            String((unsigned)nvsStats.free_entries) + " free)</td></tr>";
    html += "</table>";
    if (pct >= NVS_USAGE_WARN_PERCENT) {
      html += "<p class=\"hint\">NVS is getting full - this project has silently dropped writes here "
              "before once it filled up. Consider trimming unused cameras/Telegram users, or investigate "
              "what's using space before it happens again.</p>";
    }
  } else {
    html += "</table>";
  }

  if (ESP.getMinFreeHeap() < HEAP_LOW_WARN_BYTES) {
    html += "<p class=\"hint\">Free internal heap has dropped close to allocation-failure territory "
            "at least once since boot - checkHeapHealth() (main.cpp) logs a timestamped Activity page "
            "entry every time this happens, including the largest-allocatable-block reading at that "
            "moment, so check there for what else was going on around the same time.</p>";
  }

  html += "<fieldset><legend>Upload new firmware</legend>";
  html += "<form method=\"POST\" action=\"/firmware/update\" enctype=\"multipart/form-data\" "
          "onsubmit=\"return confirm('Flash this firmware and reboot the board? "
          "Do not power it off while this runs.');\">";
  html += "<label>Firmware .bin (e.g. .pio/build/esp32s3/firmware.bin from `pio run -e esp32s3`)"
          "<input type=\"file\" name=\"firmware\" accept=\".bin\" required></label>";
  html += "<p><button type=\"submit\" class=\"danger\">Upload &amp; Flash</button></p>";
  html += "</form></fieldset>";

  html += "<p class=\"hint\">The board reboots automatically once the upload finishes and the new "
          "image's checksum verifies. If verification fails, nothing is applied and the board keeps "
          "running the current firmware.</p>";
  return html;
}

// ============================================================
// Firmware upload (OTA). Update writes to the inactive app partition and only
// marks it bootable once verified, so a failed upload changes nothing.
// ============================================================

static bool   g_otaError = false;
static String g_otaErrorMsg;

void registerFirmwareRoutes(PsychicHttpServer& server) {
  server.on("/firmware", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Firmware, "", renderFirmwarePanel()).c_str());
  });

  static PsychicUploadHandler* otaHandler = new PsychicUploadHandler();
  otaHandler->onUpload([](PsychicRequest* request, const String& filename, uint64_t index, uint8_t* data,
                           size_t len, bool last) -> esp_err_t {
    if (index == 0) {
      g_otaError = false;
      g_otaErrorMsg = "";
      Serial.printf("[Firmware] Upload started: %s\n", filename.c_str());
      // An upload that dropped mid-way never gets its final chunk, leaving
      // Update "running" and refusing every later attempt until reboot.
      // index==0 is the start of a new upload, so clear any leftover state.
      if (Update.isRunning()) {
        Serial.println("[Firmware] A previous upload never finished (dropped connection?) - "
                        "aborting it before starting this one.");
        Update.abort();
      }
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        g_otaError = true;
        g_otaErrorMsg = Update.errorString();
        Serial.printf("[Firmware] Update.begin failed: %s\n", g_otaErrorMsg.c_str());
      }
    }
    if (!g_otaError && len > 0 && Update.write(data, len) != len) {
      g_otaError = true;
      g_otaErrorMsg = Update.errorString();
      Serial.printf("[Firmware] Update.write failed: %s\n", g_otaErrorMsg.c_str());
    }
    if (last) {
      if (!g_otaError && !Update.end(true)) {
        g_otaError = true;
        g_otaErrorMsg = Update.errorString();
      }
      Serial.printf("[Firmware] Upload finished (%s).\n", g_otaError ? "FAILED" : "OK - rebooting");
    }
    return ESP_OK; // keep accepting bytes even after a failure, so the upload doesn't just hang client-side
  });
  otaHandler->onRequest([](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    if (g_otaError) {
      String banner = "Firmware update FAILED: " + g_otaErrorMsg + " - current firmware keeps running.";
      return response->send(200, "text/html", renderShell(Tab::Firmware, banner, renderFirmwarePanel()).c_str());
    }
    esp_err_t result = response->send(
        200, "text/html",
        renderShell(Tab::Firmware, "Firmware accepted - rebooting now, this page will stop responding.",
                    "<p class=\"hint\">Reconnect in about 15 seconds.</p>")
            .c_str());
    // The image is committed and the response already promised a reboot; if
    // the task can't be created, the update won't apply until some later
    // reboot. All we can do is log it.
    if (xTaskCreate(delayedRebootTask, "otaReboot", 2048, nullptr, 1, nullptr) != pdPASS) {
      Serial.println("[Firmware] ERROR: failed to start the post-update reboot task (out of memory?) - "
                      "the new firmware is flashed but the board will NOT reboot on its own. Reboot "
                      "manually from the Maintenance page once memory frees up.");
    }
    return result;
  });
  server.on("/firmware/update", HTTP_POST, otaHandler);
}
