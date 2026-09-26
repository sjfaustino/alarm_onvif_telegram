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

// "" if epoch is 0 (unknown - main.cpp's checks run after WiFi connects,
// so the clock is normally already synced by then, but a very old record
// from before this feature existed would have no timestamp at all).
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
  // Persisted (ota_history.h) by main.cpp's boot-time OTA checks - "" (no
  // row) until the first update this board ever confirms healthy or rolls
  // back, so a board that's never had an OTA update just doesn't mention
  // it, rather than showing a misleading "None" row.
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

  // Same internal-RAM pool WiFiClientSecure/mbedTLS allocate from - see
  // main.cpp's checkHeapHealth()/HEAP_LOW_WARN_BYTES (config.h) for the
  // proactive Telegram alert and timestamped Activity-log trail this
  // mirrors; shown here too so it's visible without Telegram/Serial
  // access, same "don't only rely on someone noticing a push notification"
  // reasoning as the NVS usage row below. Largest allocatable block, not
  // just the free-byte total, distinguishes a fragmented heap (plenty of
  // free bytes, none of them contiguous enough for whatever allocation
  // actually needs one) from genuinely low total memory - same stat
  // sendTelegramPhotoBuffered (telegram_transport.cpp) already logs before every
  // photo send.
  html += "<tr><th>Free heap (internal)</th><td>" + String(ESP.getFreeHeap()) + " bytes</td></tr>";
  html += "<tr><th>Free heap - lifetime min</th><td>" + String(ESP.getMinFreeHeap()) + " bytes</td></tr>";
  html += "<tr><th>Largest allocatable block</th><td>" + String(ESP.getMaxAllocHeap()) + " bytes</td></tr>";
  html += "<tr><th>Free PSRAM</th><td>" +
          String((unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) + " bytes</td></tr>";

  // Previously invisible entirely - see TelegramRetryQueueStatus's own
  // comment (telegram_retry_queue.h) for what an empty vs. nonzero count
  // actually means. formatUptime()'s "Xh Ym ago" shape reused directly
  // for "queued for" since oldestPendingMs is already an elapsed
  // duration, not a point in time (formatElapsedSince, its usual partner,
  // would need a "now" to subtract from, which isn't the case here).
  TelegramRetryQueueStatus retryStatus = getTelegramRetryQueueStatus();
  html += "<tr><th>Telegram retry queue</th><td>" +
          (retryStatus.count > 0
               ? String((unsigned)retryStatus.count) + " pending (oldest queued for " +
                     formatUptime(retryStatus.oldestPendingMs) + ")"
               : String("empty")) +
          "</td></tr>";

  // NVS is entry-based (fixed ~32-byte slots), not a raw byte pool, so
  // "% used" here means % of entries, not bytes - still the right signal:
  // this project has hit a real incident before where camera records
  // silently failed to persist once NVS filled up (see camera_store.cpp's
  // NVS_KEY_LIST_LEGACY comment) - a visible warning here before that
  // happens again beats discovering it via a dropped write. main.cpp's
  // checkNvsUsage() also proactively alerts via Telegram at the same
  // NVS_USAGE_WARN_PERCENT threshold, so this doesn't only get noticed by
  // someone happening to load this page.
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
