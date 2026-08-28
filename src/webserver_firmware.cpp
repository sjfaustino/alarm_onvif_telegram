#include "webserver_firmware.h"
#include "build_version.h" // FIRMWARE_VERSION
#include <esp_ota_ops.h>
#include <nvs_flash.h>
#include <nvs.h>

String renderFirmwarePanel() {
  const esp_partition_t* running = esp_ota_get_running_partition();

  String html = "<h1>Firmware</h1>";
  html += "<table>";
  html += "<tr><th>Version</th><td>" + String(FIRMWARE_VERSION) + "</td></tr>";
  html += "<tr><th>Build</th><td>" + String(__DATE__) + " " + String(__TIME__) + "</td></tr>";
  html += "<tr><th>Running partition</th><td>" + String(running ? running->label : "?") + "</td></tr>";
  html += "<tr><th>Sketch size</th><td>" + String(ESP.getSketchSize() / 1024) + " KB</td></tr>";
  html += "<tr><th>Free space for update</th><td>" + String(ESP.getFreeSketchSpace() / 1024) + " KB</td></tr>";

  // NVS is entry-based (fixed ~32-byte slots), not a raw byte pool, so
  // "% used" here means % of entries, not bytes - still the right signal:
  // this project has hit a real incident before where camera records
  // silently failed to persist once NVS filled up (see camera_store.cpp's
  // NVS_KEY_LIST_LEGACY comment) - a visible warning here before that
  // happens again beats discovering it via a dropped write.
  nvs_stats_t nvsStats;
  if (nvs_get_stats(NULL, &nvsStats) == ESP_OK && nvsStats.total_entries > 0) {
    unsigned pct = (unsigned)((uint64_t)nvsStats.used_entries * 100 / nvsStats.total_entries);
    html += "<tr><th>NVS usage</th><td>" + String(pct) + "% (" + String((unsigned)nvsStats.used_entries) +
            " / " + String((unsigned)nvsStats.total_entries) + " entries, " +
            String((unsigned)nvsStats.free_entries) + " free)</td></tr>";
    html += "</table>";
    if (pct >= 80) {
      html += "<p class=\"hint\">NVS is getting full - this project has silently dropped writes here "
              "before once it filled up. Consider trimming unused cameras/Telegram users, or investigate "
              "what's using space before it happens again.</p>";
    }
  } else {
    html += "</table>";
  }

  html += "<fieldset><legend>Upload new firmware</legend>";
  html += "<form method=\"POST\" action=\"/firmware/update\" enctype=\"multipart/form-data\" "
          "onsubmit=\"return confirm('Flash this firmware and reboot the board? "
          "Do not power it off while this runs.');\">";
  html += "<label>Firmware .bin (e.g. .pio/build/esp32s3/firmware.bin from `pio run -e esp32s3`)"
          "<input type=\"file\" name=\"firmware\" accept=\".bin\" required></label>";
  html += "<p><button type=\"submit\">Upload &amp; Flash</button></p>";
  html += "</form></fieldset>";

  html += "<p class=\"hint\">The board reboots automatically once the upload finishes and the new "
          "image's checksum verifies. If verification fails, nothing is applied and the board keeps "
          "running the current firmware.</p>";
  return html;
}
