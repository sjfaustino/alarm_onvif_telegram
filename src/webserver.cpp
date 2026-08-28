#include "webserver.h"
#include "webserver_network.h"
#include "webserver_cameras.h"
#include "webserver_users.h"
#include "webserver_firmware.h"
#include "webserver_maintenance.h"
#include "webserver_security.h"
#include "webserver_activity.h"
#include "webserver_storage.h"
#include "event_log_store.h"
#include "snapshot_history.h"
#include "sd_store.h"
#include "telegram_users.h"
#include "auth_store.h"
#include "backoff.h"
#include "format_utils.h"
#include <PsychicHttp.h>
#include <Update.h>

// Routing table, the dashboard shell (sidebar + banner), and OTA
// upload-in-progress state - the parts that are either genuinely about
// wiring routes together or too tightly coupled to the PsychicHttpServer
// instance here to live anywhere else. Each panel's own rendering/form-
// handling lives in its own webserver_<panel>.h/.cpp - see
// webserver_network.h's comment for why this used to be one 946-line file.

static PsychicHttpServer server;
static std::vector<CameraConfig>* g_liveCameras = nullptr;
static std::vector<CameraState>*  g_liveStates  = nullptr;

// Global middleware, applied to every request in startWebServer() below -
// AuthenticationMiddleware::run() only requires a login once both
// setUsername()/setPassword() are non-empty, so leaving it unconfigured is
// what makes the board boot with no login required. The Security page's
// save handler updates it live, taking effect on the very next request.
static AuthenticationMiddleware g_authMiddleware;

// ============================================================
// Login rate-limiting - HTTP Basic Auth over plain HTTP has no throttling
// of its own, so without this a wrong-password guess costs an attacker
// nothing but one more request. Runs as its own middleware, registered
// BEFORE g_authMiddleware in startWebServer() (PsychicMiddlewareChain runs
// middleware in the order added - verified by reading its runChain()
// implementation, not assumed), so a locked-out IP never reaches the real
// credential check at all.
//
// Tracks consecutive failed logins per source IP; RATE_LIMIT_MAX_FAILURES
// in a row locks that IP out for an escalating duration - nextBackoffDelayMs
// (backoff.h), the same doubling-with-cap helper WiFi reconnect and camera
// subscription retry already use - reoffending after a lockout expires
// doubles the next one, up to RATE_LIMIT_LOCKOUT_MAX_MS. A single
// successful login from that IP forgives it completely (fail count and
// lockout duration both reset to 0).
//
// Applies to every route for that IP during a lockout, not just the login
// itself - untangling "let already-known-good credentials bypass a
// lockout" would partially defeat the point, and a legitimate user who
// knows the real password just waits out what should be a rare, short
// window rather than guessing.
//
// Deliberately in-RAM only, not persisted - a reboot clears every lockout,
// same as every other purely in-RAM per-boot state in this project (see
// CameraState::scheduledRevertDueMs for the same reasoning). Tracks at
// most MAX_TRACKED_IPS distinct addresses - a home LAN device doesn't
// need to remember more distinct offending IPs than that; once full, the
// least-recently-seen entry is evicted to make room for a new one.
// ============================================================

static const uint8_t       RATE_LIMIT_MAX_FAILURES     = 5;               // consecutive failures before a lockout
static const unsigned long RATE_LIMIT_LOCKOUT_START_MS = 30UL * 1000UL;   // first lockout: 30s
static const unsigned long RATE_LIMIT_LOCKOUT_MAX_MS   = 30UL * 60UL * 1000UL; // cap: 30 minutes
static const size_t        MAX_TRACKED_IPS             = 8;

struct RateLimitEntry {
  IPAddress ip;
  bool used = false;
  uint8_t failCount = 0;
  unsigned long lockoutUntilMs = 0;        // millis() timestamp; 0 = not currently locked out
  unsigned long lastLockoutDurationMs = 0; // for nextBackoffDelayMs if this IP reoffends later
  unsigned long lastSeenMs = 0;            // for LRU eviction when the table is full
};

class RateLimitMiddleware : public PsychicMiddleware {
 public:
  RateLimitMiddleware() : mutex_(xSemaphoreCreateMutex()) {}
  void setAuth(AuthenticationMiddleware* auth) { auth_ = auth; }

  esp_err_t run(PsychicRequest* request, PsychicResponse* response, PsychicMiddlewareNext next) override {
    unsigned long now = millis();

    xSemaphoreTake(mutex_, portMAX_DELAY);
    RateLimitEntry* entry = findOrCreate(request->client()->remoteIP(), now);
    bool lockedOut = entry->lockoutUntilMs != 0 && (long)(now - entry->lockoutUntilMs) < 0;
    unsigned long remainingMs = lockedOut ? (entry->lockoutUntilMs - now) : 0;
    xSemaphoreGive(mutex_);

    if (lockedOut) {
      String body = "Too many failed login attempts from this address - try again in " +
                    formatUptime(remainingMs) + ".";
      return response->send(429, "text/plain", body.c_str());
    }

    // Pre-check credentials directly (isAllowed() is public and side-
    // effect-free - see AuthenticationMiddleware.cpp) so this middleware
    // knows whether to count a failure. g_authMiddleware itself still runs
    // via next() below and issues the real 401 challenge on failure;
    // duplicating the check here (rather than inspecting its response
    // afterward) avoids reaching into PsychicResponse internals this
    // library doesn't expose to a middleware.
    bool allowed = !auth_ || auth_->isAllowed(request);

    xSemaphoreTake(mutex_, portMAX_DELAY);
    entry = findOrCreate(request->client()->remoteIP(), now); // re-find - table state may have moved under a lock we briefly released
    if (allowed) {
      entry->failCount = 0;
      entry->lockoutUntilMs = 0;
      entry->lastLockoutDurationMs = 0;
    } else {
      entry->failCount++;
      if (entry->failCount >= RATE_LIMIT_MAX_FAILURES) {
        entry->lastLockoutDurationMs = nextBackoffDelayMs(entry->lastLockoutDurationMs,
                                                            RATE_LIMIT_LOCKOUT_START_MS, RATE_LIMIT_LOCKOUT_MAX_MS);
        entry->lockoutUntilMs = now + entry->lastLockoutDurationMs;
        entry->failCount = 0; // counts fresh toward the *next* lockout, after this one expires
        Serial.printf("[WebServer] IP %s locked out of the dashboard for %lus after %u consecutive failed logins.\n",
                      entry->ip.toString().c_str(), entry->lastLockoutDurationMs / 1000UL,
                      (unsigned)RATE_LIMIT_MAX_FAILURES);
      }
    }
    xSemaphoreGive(mutex_);

    return next();
  }

 private:
  // Caller must hold mutex_. Exact IP match if tracked; otherwise an
  // unused slot, or (table full) the least-recently-seen entry, reset and
  // claimed for this IP.
  RateLimitEntry* findOrCreate(const IPAddress& ip, unsigned long now) {
    for (auto& e : table_) {
      if (e.used && e.ip == ip) { e.lastSeenMs = now; return &e; }
    }
    RateLimitEntry* victim = &table_[0];
    for (auto& e : table_) {
      if (!e.used) { victim = &e; break; }
      if (e.lastSeenMs < victim->lastSeenMs) victim = &e;
    }
    *victim = RateLimitEntry{};
    victim->ip = ip;
    victim->used = true;
    victim->lastSeenMs = now;
    return victim;
  }

  RateLimitEntry table_[MAX_TRACKED_IPS];
  AuthenticationMiddleware* auth_ = nullptr;
  SemaphoreHandle_t mutex_;
};

static RateLimitMiddleware g_rateLimitMiddleware;

// ============================================================
// Dashboard shell - sidebar + content panel, plain server-rendered pages
// with no client-side router/JS framework. Everything's embedded in the
// firmware binary rather than served from a filesystem, on purpose.
// ============================================================

enum class Tab { None, Network, Cameras, Users, Activity, Firmware, Maintenance, Storage, Security };

static String renderShell(Tab active, const String& banner, const String& contentHtml) {
  String html;
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<title>Camera Monitor</title><style>";
  html += "*{box-sizing:border-box;}";
  html += "body{font-family:sans-serif;margin:0;display:flex;min-height:100vh;color:#222;}";
  html += ".sidebar{width:200px;flex-shrink:0;background:#1f2937;color:#e5e7eb;padding:20px 0;}";
  html += ".sidebar .brand{font-weight:bold;font-size:16px;padding:0 20px 20px;}";
  html += ".sidebar a{display:block;padding:10px 20px;color:#cbd5e1;text-decoration:none;font-size:14px;}";
  html += ".sidebar a:hover{background:#374151;}";
  html += ".sidebar a.active{background:#2563eb;color:#fff;font-weight:bold;}";
  html += ".content{flex:1;padding:24px 28px;max-width:960px;}";
  html += "h1{font-size:20px;margin-top:0;}";
  html += "table{border-collapse:collapse;width:100%;margin-bottom:24px;}";
  html += "th,td{border:1px solid #ccc;padding:6px 8px;text-align:left;font-size:14px;vertical-align:top;}";
  html += "th{background:#f0f0f0;}";
  html += "form.inline{display:inline;}";
  html += "fieldset{margin-bottom:20px;}";
  html += "label{display:block;margin-top:10px;font-size:14px;}";
  html += "label.checkbox{display:flex;align-items:center;gap:6px;font-weight:normal;}";
  html += "label.checkbox input{width:auto;}";
  html += "input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:5px;margin-top:2px;}";
  html += ".camera-list{border:1px solid #ddd;padding:8px;max-height:180px;overflow-y:auto;margin-top:2px;}";
  html += ".camera-list label{margin-top:2px;}";
  html += ".banner{background:#fffae0;border:1px solid #e0d080;padding:8px 12px;margin-bottom:16px;}";
  html += ".banner-warn{background:#fde2e1;border:1px solid #e08080;padding:8px 12px;margin-bottom:16px;}";
  html += ".hint{color:#666;font-size:13px;}";
  html += ".sidebar-parent{cursor:pointer;}";
  html += ".sidebar-submenu a{padding-left:36px;font-size:13px;}";
  html += "</style></head><body>";

  // System submenu (Firmware/Maintenance) starts expanded whenever either
  // of its own pages is the active one, so navigating straight to
  // /firmware or /maintenance (a bookmark, a link from elsewhere) doesn't
  // land on a page whose own sidebar entry is hidden inside a collapsed
  // menu. The onclick toggle below is a plain inline handler, not a
  // separate <script> block - consistent with this project's "no client-
  // side framework" stance elsewhere, just enough JS to open/close a menu
  // on a full-page-reload site.
  bool systemOpen = (active == Tab::Firmware || active == Tab::Maintenance || active == Tab::Storage);

  html += "<nav class=\"sidebar\"><div class=\"brand\">Camera Monitor</div>";
  html += "<a href=\"/network\" class=\"";
  html += (active == Tab::Network) ? "active" : "";
  html += "\">Network</a>";
  html += "<a href=\"/cameras\" class=\"";
  html += (active == Tab::Cameras) ? "active" : "";
  html += "\">Cameras</a>";
  html += "<a href=\"/users\" class=\"";
  html += (active == Tab::Users) ? "active" : "";
  html += "\">Telegram Users</a>";
  html += "<a href=\"/activity\" class=\"";
  html += (active == Tab::Activity) ? "active" : "";
  html += "\">Activity</a>";
  html += "<a href=\"#\" class=\"sidebar-parent\" onclick=\"var m=document.getElementById('system-submenu');"
          "m.style.display=(m.style.display==='block')?'none':'block';return false;\">System</a>";
  html += "<div id=\"system-submenu\" class=\"sidebar-submenu\" style=\"display:";
  html += systemOpen ? "block" : "none";
  html += ";\">";
  html += "<a href=\"/firmware\" class=\"";
  html += (active == Tab::Firmware) ? "active" : "";
  html += "\">Firmware</a>";
  html += "<a href=\"/maintenance\" class=\"";
  html += (active == Tab::Maintenance) ? "active" : "";
  html += "\">Maintenance</a>";
  html += "<a href=\"/storage\" class=\"";
  html += (active == Tab::Storage) ? "active" : "";
  html += "\">Storage</a>";
  html += "</div>";
  html += "<a href=\"/security\" class=\"";
  html += (active == Tab::Security) ? "active" : "";
  html += "\">Security</a>";
  html += "</nav>";

  html += "<main class=\"content\">";
  DashboardAuth currentAuth = loadDashboardAuth();
  if (currentAuth.username.length() == 0 || currentAuth.password.length() == 0) {
    html += "<div class=\"banner-warn\">No dashboard password is set - anyone on your LAN can view "
            "and change everything here, including WiFi/camera credentials, the Firmware page, the "
            "Maintenance page's reboot button, and the Storage page's erase-all-history button. "
            "<a href=\"/security\">Set one now</a>.</div>";
  }
  if (banner.length() > 0) html += "<div class=\"banner\">" + banner + "</div>";
  html += contentHtml;
  html += "</main></body></html>";
  return html;
}

// esp_restart() inside the still-sending request handler would tear down
// the connection before the client sees the response - reboot from a
// short-lived task instead, after send() returns. Shared by the Firmware
// page's OTA success path and the Maintenance page's manual reboot button
// below - nothing about the delay-then-restart itself is OTA-specific.
static void delayedRebootTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(1000));
  ESP.restart();
}

// ============================================================
// Firmware panel routing - upload a .bin over the dashboard instead of a
// USB reflash. Backed by ESP32's Update library, which writes into the
// currently-inactive OTA app partition (app0/app1 - see platformio.ini)
// and only marks it bootable once the checksum verifies, so a failed/
// aborted upload leaves the running firmware untouched. Kept here (not in
// webserver_firmware.cpp) since it's routing + upload-in-progress state,
// not page content.
// ============================================================

static bool   g_otaError = false;
static String g_otaErrorMsg;

void startWebServer(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates) {
  g_liveCameras = liveCameras;
  g_liveStates = liveStates;

  DashboardAuth auth = loadDashboardAuth();
  g_authMiddleware.setUsername(auth.username.c_str())
      .setPassword(auth.password.c_str())
      .setRealm("Camera Monitor")
      .setAuthMethod(BASIC_AUTH);
  g_rateLimitMiddleware.setAuth(&g_authMiddleware);
  // Rate limiter registered first - PsychicMiddlewareChain runs middleware
  // in the order added, so a locked-out IP is short-circuited here and
  // never reaches g_authMiddleware at all. Both apply to every route
  // registered below, including the Firmware upload - see each
  // middleware's own declaration comment.
  server.addMiddleware(&g_rateLimitMiddleware);
  server.addMiddleware(&g_authMiddleware);

  server.on("/", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    String landing = "<h1>Camera Monitor</h1><p class=\"hint\">Select a section from the left.</p>";
    return response->send(200, "text/html", renderShell(Tab::None, "", landing).c_str());
  });

  server.on("/network", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Network, "", renderNetworkPanel()).c_str());
  });

  server.on("/network/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String banner;
    handleSaveNetwork(request, banner);
    return response->send(200, "text/html", renderShell(Tab::Network, banner, renderNetworkPanel()).c_str());
  });

  server.on("/cameras", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    // note carries a one-time status banner across the POST-redirect-GET
    // from /cameras/save (see that route below) - saveCameraSubmission
    // already htmlEscape()s anything user-controlled (a camera name) that
    // goes into it before it's ever URL-encoded into the redirect, and
    // PsychicRequest url-decodes query params automatically (verified by
    // reading PsychicRequest::_addParams, not assumed) - so what comes
    // back out here is safe to hand to renderShell's own unescaped banner
    // parameter as-is, same as every other banner in this file.
    String note = request->getParam("note", "");
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, note, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates)).c_str());
  });

  server.on("/cameras/edit", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    for (auto& c : loadCameras()) {
      if (c.name.equalsIgnoreCase(name)) {
        CameraConfig prefill = c;
        prefill.pass = ""; // never populate a password field with the real value
        return response->send(
            200, "text/html",
            renderShell(Tab::Cameras, "", renderCamerasPanel(&prefill, true, g_liveCameras, g_liveStates)).c_str());
      }
    }
    return response->redirect("/cameras");
  });

  server.on("/cameras/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    CameraConfig submitted = parseCameraForm(request);
    String originalName = request->getParam("originalName", "");
    originalName.trim();

    String banner;
    String applyNote;
    if (!saveCameraSubmission(submitted, originalName, banner, applyNote, g_liveCameras, g_liveStates)) {
      submitted.pass = "";
      return response->send(
          200, "text/html",
          renderShell(Tab::Cameras, banner,
                      renderCamerasPanel(&submitted, originalName.length() > 0, g_liveCameras, g_liveStates))
              .c_str());
    }
    // Redirect (not render-in-place) even when there's a note to show, to
    // keep the usual POST-redirect-GET behavior (refreshing /cameras/save
    // itself would otherwise re-submit the form) - the note rides along
    // as a query param and the /cameras GET handler above picks it up.
    String redirectUrl = "/cameras";
    if (applyNote.length() > 0) redirectUrl += "?note=" + urlEncode(applyNote);
    return response->redirect(redirectUrl.c_str());
  });

  server.on("/cameras/test", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    CameraConfig submitted = parseCameraForm(request);
    String originalName = request->getParam("originalName", "");
    originalName.trim();
    bool isEdit = originalName.length() > 0;

    CameraConfig testCfg = submitted;
    if (isEdit && testCfg.pass.length() == 0) {
      for (auto& existing : loadCameras()) {
        if (existing.name.equalsIgnoreCase(originalName)) { testCfg.pass = existing.pass; break; }
      }
    }
    String banner = testCameraConnection(testCfg);

    submitted.pass = "";
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, banner, renderCamerasPanel(&submitted, isEdit, g_liveCameras, g_liveStates))
            .c_str());
  });

  // Serves one entry from the camera's snapshot history - SD-backed if
  // sdActive() (sd_store.h), else the PSRAM ring fallback; see
  // snapshot_history.h, the single place that decides which. age=0
  // (default) is the most recent. Goes through the same global middleware
  // chain as every other route (rate-limit, then auth) - deliberately not
  // exempted, since it's exposing camera footage.
  server.on("/cameras/snapshot", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    String name = request->getParam("name", "");
    long age = request->getParam("age", "0").toInt();
    int idx = -1;
    if (g_liveCameras) {
      for (size_t i = 0; i < g_liveCameras->size(); i++) {
        if ((*g_liveCameras)[i].name.equalsIgnoreCase(name)) { idx = (int)i; break; }
      }
    }
    if (idx < 0 || !g_liveStates || idx >= (int)g_liveStates->size() || age < 0) {
      return response->send(404, "text/plain", "No such camera.");
    }

    // readCameraSnapshot copies the bytes out itself (under whichever
    // lock/mutex its backing store uses) before returning - this route
    // never holds anything across the blocking network send() below.
    uint8_t* copy = nullptr;
    size_t len = 0;
    bool ok = readCameraSnapshot((*g_liveCameras)[idx], (*g_liveStates)[idx], (size_t)age, &copy, &len);
    if (!ok || !copy || len == 0) {
      return response->send(404, "text/plain", "No snapshot captured yet for this camera.");
    }
    esp_err_t result = response->send(200, "image/jpeg", copy, len);
    free(copy);
    return result;
  });

  server.on("/delete", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    deleteCamera(name);
    return response->redirect("/cameras");
  });

  server.on("/users", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Users, "", renderUsersPanel(nullptr, false)).c_str());
  });

  server.on("/users/edit", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    for (auto& u : loadTelegramUsers()) {
      if (u.name.equalsIgnoreCase(name)) {
        TelegramUser prefill = u;
        return response->send(200, "text/html",
                               renderShell(Tab::Users, "", renderUsersPanel(&prefill, true)).c_str());
      }
    }
    return response->redirect("/users");
  });

  server.on("/users/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    TelegramUser submitted = parseUserForm(request);
    String originalName = request->getParam("originalName", "");
    originalName.trim();

    String banner;
    if (!saveUserSubmission(submitted, originalName, banner)) {
      return response->send(200, "text/html",
                             renderShell(Tab::Users, banner,
                                         renderUsersPanel(&submitted, originalName.length() > 0)).c_str());
    }
    return response->redirect("/users");
  });

  server.on("/users/delete", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    deleteTelegramUser(name);
    return response->redirect("/users");
  });

  server.on("/activity", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Activity, "", renderActivityPanel()).c_str());
  });

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
    xTaskCreate(delayedRebootTask, "otaReboot", 2048, nullptr, 1, nullptr);
    return result;
  });
  server.on("/firmware/update", HTTP_POST, otaHandler);

  server.on("/maintenance", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Maintenance, "", renderMaintenancePanel()).c_str());
  });

  server.on("/maintenance/reboot", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    Serial.println("[Maintenance] Reboot requested via dashboard.");
    esp_err_t result = response->send(
        200, "text/html",
        renderShell(Tab::Maintenance, "Rebooting now - reconnect in about 15-20 seconds.",
                    renderMaintenancePanel())
            .c_str());
    xTaskCreate(delayedRebootTask, "maintReboot", 2048, nullptr, 1, nullptr);
    return result;
  });

  server.on("/storage", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Storage, "", renderStoragePanel()).c_str());
  });

  server.on("/storage/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    SdSettings settings;
    settings.enabled = request->hasParam("enabled");
    String banner = saveSdSettings(settings)
        ? "Saved - reboot the board to apply."
        : "Failed to save - NVS write error (see Serial log). Setting was NOT changed.";
    return response->send(200, "text/html", renderShell(Tab::Storage, banner, renderStoragePanel()).c_str());
  });

  server.on("/storage/check", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    SnapshotStorageCheckResult result = checkSnapshotStorage();
    String banner;
    if (!result.ranAtAll) {
      banner = "SD storage isn't active - nothing to check.";
    } else if (result.ok) {
      banner = "Checked " + String((unsigned)result.filesChecked) + " file(s) across " +
               String((unsigned)result.directoriesChecked) + " camera(s) - all readable.";
    } else {
      banner = "Checked " + String((unsigned)result.filesChecked) + " file(s) - " +
               String((unsigned)result.unreadableFiles) + " unreadable. See Serial log for which.";
    }
    return response->send(200, "text/html", renderShell(Tab::Storage, banner, renderStoragePanel()).c_str());
  });

  server.on("/storage/erase", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    Serial.println("[Storage] Erase all snapshot history requested via dashboard.");
    bool ok = eraseAllSnapshots();
    String banner = ok ? "All snapshot history erased." : "Erase completed with errors - see Serial log.";
    return response->send(200, "text/html", renderShell(Tab::Storage, banner, renderStoragePanel()).c_str());
  });

  server.on("/security", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Security, "", renderSecurityPanel()).c_str());
  });

  // Content-Disposition: attachment makes the browser download this as a
  // file instead of displaying it inline - buildConfigExport() (see its
  // own comment) never includes a password, so there's nothing here more
  // sensitive than what the Cameras/Users pages already show.
  server.on("/export", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    response->addHeader("Content-Disposition", "attachment; filename=\"camera-monitor-config.txt\"");
    return response->send(200, "text/plain", buildConfigExport().c_str());
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
        // Don't touch the live middleware if the write didn't actually
        // land - doing so would protect the dashboard for this boot only,
        // silently reverting to the old (or no) login on the next reboot
        // with nothing telling the user it happened. See auth_store.cpp.
        banner = "Failed to save - NVS write error (see Serial log). Login was NOT changed.";
      } else {
        g_authMiddleware.setUsername(newAuth.username.c_str()).setPassword(newAuth.password.c_str());
        banner = "Saved - a login is now required on every page, starting now.";
      }
    }
    return response->send(200, "text/html", renderShell(Tab::Security, banner, renderSecurityPanel()).c_str());
  });

  server.begin();
  Serial.println("[WebServer] Camera management UI listening on port 80.");
}
