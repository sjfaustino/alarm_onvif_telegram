#include "webserver_storage.h"
#include "sd_store.h"
#include "camera.h" // SNAPSHOT_HISTORY_SIZE
#include "format_utils.h"

String renderStoragePanel() {
  SdStatus status = getSdStatus();
  String html = "<h1>Storage</h1>";

  if (!status.settingEnabled) {
    html += "<p class=\"hint\">SD card storage is off - snapshot history uses the small in-memory "
            "cache only (last " + String((unsigned)SNAPSHOT_HISTORY_SIZE) + " per camera, lost on "
            "reboot). Enable it below to use a microSD card instead, for far more history that "
            "survives a reboot.</p>";
  } else if (!status.available) {
    html += "<p class=\"hint\">SD card storage is enabled, but no module or card was detected at "
            "boot - snapshot history is using the in-memory fallback instead, and monitoring itself "
            "is unaffected. Check the wiring and the SD_CS_PIN/SD_SCK_PIN/SD_MISO_PIN/SD_MOSI_PIN "
            "values in <code>config.h</code>, then reboot.</p>";
  } else {
    double totalMB = (double)status.totalBytes / (1024.0 * 1024.0);
    double usedMB = (double)status.usedBytes / (1024.0 * 1024.0);
    html += "<table>";
    html += "<tr><th>Card type</th><td>" + htmlEscape(status.cardTypeName) + "</td></tr>";
    html += "<tr><th>Used</th><td>" + String(usedMB, 1) + " MB / " + String(totalMB, 1) + " MB</td></tr>";
    html += "</table>";
  }

  html += "<fieldset><legend>Enable</legend>";
  html += "<form method=\"POST\" action=\"/storage/save\">";
  html += "<label class=\"checkbox\"><input type=\"checkbox\" name=\"enabled\"" +
          String(status.settingEnabled ? " checked" : "") +
          "> Use SD card storage for snapshot history</label>";
  html += "<p><button type=\"submit\">Save</button></p></form>";
  html += "<p class=\"hint\">Takes effect after a reboot - toggling this doesn't touch the SPI bus "
          "live while a camera task might be mid-write, and these modules aren't reliably hot-"
          "swappable anyway.</p>";
  html += "</fieldset>";

  if (status.available) {
    html += "<fieldset><legend>Maintenance</legend>";

    html += "<form method=\"POST\" action=\"/storage/check\">";
    html += "<p><button type=\"submit\">Check storage</button></p></form>";
    html += "<p class=\"hint\">Confirms every stored snapshot file is still readable - not a full "
            "filesystem consistency check (this project's SD support has no fsck/chkdsk equivalent), "
            "just a walk verifying nothing this project wrote is corrupted or missing.</p>";

    html += "<form method=\"POST\" action=\"/storage/erase\" "
            "onsubmit=\"return confirm('Erase ALL stored snapshot history for every camera? "
            "This cannot be undone.');\">";
    html += "<p><button type=\"submit\">Erase all snapshot history</button></p></form>";
    html += "<p class=\"hint\">Deletes every snapshot this project has written, for every camera - "
            "not a low-level format of the card itself (this project's SD support can't do that "
            "through the library it uses), just this project's own files. Anything else on the "
            "card, if you're reusing one that already had other data, is left untouched.</p>";

    html += "</fieldset>";
  }

  return html;
}
