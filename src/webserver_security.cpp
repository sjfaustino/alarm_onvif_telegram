#include "webserver_security.h"
#include "auth_store.h"
#include "format_utils.h"
#include "webserver_internal.h"
#include "config_backup.h"

String renderSecurityPanel() {
  DashboardAuth auth = loadDashboardAuth();
  bool configured = auth.username.length() > 0 && auth.password.length() > 0;

  String html = "<h1>Security</h1>";
  if (configured) {
    html += "<p class=\"hint\">A dashboard login is set (username: " + htmlEscape(auth.username) +
            "). Every page here, including this one and the Firmware upload, now requires it. "
            "Changing it below takes effect on your very next request.</p>";
  } else {
    html += "<p class=\"hint\">No login is set - this dashboard, including the Firmware upload page, "
            "is reachable by anyone on your LAN with no password. Set one below to require it on "
            "every page from now on.</p>";
  }

  html += "<fieldset><legend>" + String(configured ? "Change" : "Set") + " dashboard login</legend>";
  html += "<form method=\"POST\" action=\"/security/save\">";
  html += "<label>Username<input type=\"text\" name=\"username\" value=\"" + htmlEscape(auth.username) +
          "\" required></label>";
  html += "<label>Password<input type=\"password\" name=\"password\" required></label>";
  html += "<label>Confirm password<input type=\"password\" name=\"confirmPassword\" required></label>";
  html += "<p><button type=\"submit\">Save</button></p></form></fieldset>";

  html += "<p class=\"hint\">There's no recovery flow if this is lost - forgetting it means erasing "
          "the board's NVS entirely (wiping cameras, WiFi, and Telegram users too, not just this) "
          "to get back in. Keep it somewhere safe.</p>";

  html += "<fieldset><legend>Backup</legend>";
  html += "<p class=\"hint\">Downloads every camera, Telegram user, and network setting currently "
          "stored, as plain text. It includes every camera's username and password (so keep the file "
          "private), but never the WiFi password. Useful to have on hand if NVS is ever erased (see above) "
          "or a board gets replaced - reconstructing everything else from memory is the tedious part.</p>";
  html += "<p><a href=\"/export\"><button type=\"button\">Export configuration</button></a></p>";
  html += "</fieldset>";

  html += "<fieldset><legend>Import</legend>";
  html += "<p class=\"hint\">Restores cameras, Telegram users, network settings, and SD settings "
          "from a previously exported configuration file - REPLACES whichever of those sections "
          "the file actually contains (a section missing from the file is left untouched). "
          "Camera passwords are restored from the file; the WiFi password is never exported, so if "
          "Network is included it must be re-entered before rebooting - rebooting first strands the board off the network entirely, reachable "
          "only via physical/serial access. Takes effect after a reboot, same as any other "
          "camera/network change. Only files exported by this Import feature (this build or "
          "later) can be restored - older exports have nothing for it to read. Every import "
          "automatically saves a backup of what was stored just before it - if the wrong file "
          "gets imported, <a href=\"/import/backup\">download that backup</a> and import it "
          "again to undo.</p>";
  // multipart/form-data, not a plain form field - a config file grows
  // roughly linearly with camera/user count, and the URL-encoded plain-
  // form approach (the browser percent-encoding every
  // newline and every \x1F machine-readable-block separator as 3 bytes
  // each) could blow well past the dashboard's ordinary request-body cap
  // on exactly the multi-camera setups this feature exists to back up. A
  // real file upload is bounded by the much larger upload-size limit
  // (registerSecurityRoutes' importHandler) instead, and streams in rather than
  // needing to be buffered whole by the browser into a hidden field first.
  html += "<form method=\"POST\" action=\"/import\" enctype=\"multipart/form-data\" "
          "onsubmit=\"return confirm('Import this "
          "configuration? This REPLACES cameras/Telegram users/network/SD settings currently "
          "stored with whatever the file contains (a section missing from the file is left "
          "alone). The WiFi password is not in the file - if it includes Network, do NOT "
          "reboot until you have re-entered the WiFi password, or the board will be unable to "
          "reconnect.');\">";
  html += "<label>Configuration file (.txt from Export above)"
          "<input type=\"file\" name=\"configFile\" accept=\".txt\" required></label>";
  html += "<p><button type=\"submit\">Import configuration</button></p></form></fieldset>";
  return html;
}

void registerSecurityRoutes(PsychicHttpServer& server) {
  server.on("/security", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Security, "", renderSecurityPanel()).c_str());
  });

  // Downloads as a file. Note: the machine-readable CAMERAS block includes
  // camera passwords (WiFi passwords are never exported).
  server.on("/export", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    response->addHeader("Content-Disposition", "attachment; filename=\"camera-monitor-config.txt\"");
    return response->send(200, "text/plain", buildConfigExport().c_str());
  });

  // A multipart upload, not a form field: an export can exceed PsychicHttp's
  // 16K body limit once percent-encoded. Chunks are accumulated into a String
  // (exports are KB-sized).
  static String g_importText;
  static String g_importBanner;

  static PsychicUploadHandler* importHandler = new PsychicUploadHandler();
  importHandler->onUpload([](PsychicRequest* request, const String& filename, uint64_t index, uint8_t* data,
                              size_t len, bool last) -> esp_err_t {
    if (index == 0) g_importText = ""; // first chunk of THIS upload - drop any leftover from a previous one
    if (len > 0) g_importText.concat((const char*)data, len);
    if (last) {
      ConfigImportApplyResult r = applyConfigImport(g_importText);
      g_importText = ""; // done with it - don't hold the buffer until the next import
      g_importBanner = summarizeImportResult(r, ImportSummaryFormat::Html);
    }
    return ESP_OK;
  });
  importHandler->onRequest([](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    return response->send(200, "text/html", renderShell(Tab::Security, g_importBanner, renderSecurityPanel()).c_str());
  });
  server.on("/import", HTTP_POST, importHandler);

  // The automatic pre-import backup, in export format - re-import it to undo.
  server.on("/import/backup", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    String backup = loadConfigBackup();
    if (backup.length() == 0) {
      return response->send(200, "text/plain",
                             "No pre-import backup available yet - one is saved automatically the next "
                             "time Import is used.");
    }
    response->addHeader("Content-Disposition", "attachment; filename=\"camera-monitor-config-backup.txt\"");
    return response->send(200, "text/plain", backup.c_str());
  });

  server.on("/security/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String username = request->getParam("username", "");
    String password = request->getParam("password", "");
    String confirmPassword = request->getParam("confirmPassword", "");
    username.trim();

    String banner;
    if (username.length() == 0 || password.length() == 0) {
      banner = "Username and password are both required.";
    } else if (password != confirmPassword) {
      banner = "Password and confirmation don't match - not saved.";
    } else {
      DashboardAuth newAuth;
      newAuth.username = username;
      newAuth.password = password;
      if (!saveDashboardAuth(newAuth)) {
        // Leave the live login alone if the save failed; otherwise it would
        // silently revert on the next reboot.
        banner = "Failed to save - NVS write error (see Serial log). Login was NOT changed.";
      } else {
        applyDashboardLogin(newAuth);
        banner = "Saved - a login is now required on every page, starting now.";
      }
    }
    return response->send(200, "text/html", renderShell(Tab::Security, banner, renderSecurityPanel()).c_str());
  });
}
