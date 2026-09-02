#include "webserver_storage.h"
#include "sd_store.h"
#include "camera.h" // SNAPSHOT_HISTORY_SIZE
#include "format_utils.h"
#include "background_job.h" // BackgroundJob<T> - startStorageCheckAsync
#include <freertos/FreeRTOS.h>

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
  html += "<p><label>Automatic full storage check every "
          "<input type=\"number\" name=\"checkIntervalHours\" min=\"0\" max=\"720\" style=\"width:5em\" "
          "value=\"" + String((unsigned)status.checkIntervalHours) + "\"> hour(s) (0 = off)</label></p>";
  html += "<p><button type=\"submit\">Save</button></p></form>";
  html += "<p class=\"hint\">The enable checkbox takes effect after a reboot - toggling it doesn't "
          "touch the SPI bus live while a camera task might be mid-write, and these modules aren't "
          "reliably hot-swappable anyway. The check interval takes effect immediately. A full check "
          "walks every stored file (same as the manual \"check storage\" button below) and can briefly "
          "delay a camera's own SD write on a card with a lot of history - keep the interval long "
          "(e.g. daily) unless you have a specific reason not to.</p>";
  html += "</fieldset>";

  if (status.available) {
    html += "<fieldset><legend>Maintenance</legend>";

    html += "<form method=\"POST\" action=\"/storage/check\">";
    html += "<p><button type=\"submit\">Check storage</button></p></form>";
    html += "<p class=\"hint\">Confirms every stored snapshot file is still readable - not a full "
            "filesystem consistency check (this project's SD support has no fsck/chkdsk equivalent), "
            "just a walk verifying nothing this project wrote is corrupted or missing. Runs in the "
            "background, so the dashboard stays responsive to everyone else while it works - reload "
            "this page after clicking to see the result once ready.</p>";
    html += renderStorageCheckStatus();

    html += "<form method=\"POST\" action=\"/storage/erase\" "
            "onsubmit=\"return confirm('Erase ALL stored snapshot history for every camera? "
            "This cannot be undone.');\">";
    html += "<p><button type=\"submit\" class=\"danger\">Erase all snapshot history</button></p></form>";
    html += "<p class=\"hint\">Deletes every snapshot this project has written, for every camera - "
            "not a low-level format of the card itself (this project's SD support can't do that "
            "through the library it uses), just this project's own files. Anything else on the "
            "card, if you're reusing one that already had other data, is left untouched. Runs in "
            "the background, so the dashboard stays responsive to everyone else while it works - "
            "reload this page after clicking to see the result once ready.</p>";
    html += renderEraseAllStatus();

    html += "</fieldset>";
  }

  return html;
}

// ============================================================
// Background wrapper - see webserver_storage.h's own comment for why this
// can't run synchronously on the calling (PsychicHttp) task. BackgroundJob<T>
// (background_job.h) owns the mutex/state-machine part, same as
// webserver_cameras.cpp's identically-shaped background jobs.
// ============================================================

static BackgroundJob<SnapshotStorageCheckResult> g_storageCheckJob;

static void storageCheckTask(void*) {
  g_storageCheckJob.finish(checkSnapshotStorage()); // the actual (slow, unbounded) work
  vTaskDelete(nullptr);
}

BackgroundJobStartOutcome startStorageCheckAsync() {
  if (!g_storageCheckJob.tryStart()) return BackgroundJobStartOutcome::AlreadyRunning; // one check at a time - a second click while one's in flight is a no-op

  // No TLS/HTTPClient work here, just SD file I/O and light path-string
  // building - a smaller stack than the TLS-heavy tasks (camera_tasks.h's
  // 10240) is enough, same reasoning as wifiScanTask/cameraDiscoveryTask
  // (webserver_network.cpp/webserver_cameras.cpp).
  BaseType_t created = xTaskCreate(storageCheckTask, "sdCheck", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr);
  if (created != pdPASS) {
    // Without this, a task creation failure (out of memory) would leave
    // g_storageCheckJob permanently stuck "in progress" - see
    // BackgroundJob<T>::cancelStart's own comment (background_job.h).
    g_storageCheckJob.cancelStart();
    Serial.println("[webserver_storage] ERROR: failed to start the storage check task (out of memory?) "
                    "- try again once memory frees up.");
    return BackgroundJobStartOutcome::FailedToStart;
  }
  return BackgroundJobStartOutcome::Started;
}

String renderStorageCheckStatus() {
  auto st = g_storageCheckJob.status();
  if (st.inProgress) {
    return "<p class=\"hint\">Checking storage in the background - reload this page in a moment to "
           "see the result. The rest of the dashboard stays responsive to everyone else in the "
           "meantime.</p>";
  }
  if (!st.hasResult) return "";
  const SnapshotStorageCheckResult& result = st.result;
  if (!result.ranAtAll) {
    return "<p class=\"hint\">SD storage isn't active - nothing to check.</p>";
  }
  if (result.ok) {
    return "<p class=\"hint\">Checked " + String((unsigned)result.filesChecked) + " file(s) across " +
           String((unsigned)result.directoriesChecked) + " camera(s) - all readable.</p>";
  }
  return "<p class=\"hint\">Checked " + String((unsigned)result.filesChecked) + " file(s) - " +
         String((unsigned)result.unreadableFiles) + " unreadable. See Serial log for which.</p>";
}

static BackgroundJob<bool> g_eraseAllJob;

static void eraseAllTask(void*) {
  g_eraseAllJob.finish(eraseAllSnapshots()); // the actual (slow, unbounded) work
  vTaskDelete(nullptr);
}

BackgroundJobStartOutcome startEraseAllAsync() {
  if (!g_eraseAllJob.tryStart()) return BackgroundJobStartOutcome::AlreadyRunning; // one erase at a time - a second click while one's in flight is a no-op

  // Same sizing reasoning as startStorageCheckAsync above - SD file I/O
  // only, no TLS/HTTPClient work.
  BaseType_t created = xTaskCreate(eraseAllTask, "sdErase", 4096, nullptr, tskIDLE_PRIORITY + 1, nullptr);
  if (created != pdPASS) {
    // Without this, a task creation failure (out of memory) would leave
    // g_eraseAllJob permanently stuck "in progress" - see
    // BackgroundJob<T>::cancelStart's own comment (background_job.h).
    g_eraseAllJob.cancelStart();
    Serial.println("[webserver_storage] ERROR: failed to start the erase-all task (out of memory?) "
                    "- try again once memory frees up.");
    return BackgroundJobStartOutcome::FailedToStart;
  }
  return BackgroundJobStartOutcome::Started;
}

String renderEraseAllStatus() {
  auto st = g_eraseAllJob.status();
  if (st.inProgress) {
    return "<p class=\"hint\">Erasing all snapshot history in the background - reload this page in a "
           "moment to see the result. The rest of the dashboard stays responsive to everyone else in "
           "the meantime.</p>";
  }
  if (!st.hasResult) return "";
  return st.result ? "<p class=\"hint\">All snapshot history erased.</p>"
                    : "<p class=\"hint\">Erase completed with errors - see Serial log.</p>";
}
