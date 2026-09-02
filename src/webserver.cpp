#include "webserver.h"
#include "config.h" // SD_CHECK_INTERVAL_MAX_HOURS
#include "build_version.h" // FIRMWARE_VERSION
#include "webserver_network.h"
#include "webserver_cameras.h"
#include "webserver_users.h"
#include "webserver_firmware.h"
#include "webserver_maintenance.h"
#include "webserver_security.h"
#include "webserver_activity.h"
#include "webserver_gallery.h"
#include "webserver_storage.h"
#include "event_log_store.h"
#include "snapshot_history.h"
#include "sd_store.h"
#include "telegram_users.h"
#include "telegram.h" // setAllCamerasAlertState - /cameras/mute-all, /unmute-all
#include "auth_store.h"
#include "backoff.h"
#include "format_utils.h"
#include <PsychicHttp.h>
#include <Update.h>
#include <WiFi.h> // WiFi.localIP() - the startup "listening on" log line

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
// of its own, so without this a wrong-password guess costs nothing but
// one more request. Registered BEFORE g_authMiddleware in startWebServer()
// (PsychicMiddlewareChain runs middleware in registration order, verified
// against runChain()), so a locked-out IP never reaches the real
// credential check.
//
// Tracks consecutive failed logins per source IP; RATE_LIMIT_MAX_FAILURES
// in a row locks that IP out for an escalating duration (nextBackoffDelayMs,
// backoff.h - same helper WiFi reconnect/camera retry use), doubling on
// reoffense up to RATE_LIMIT_LOCKOUT_MAX_MS. One successful login fully
// forgives that IP.
//
// Applies to every route for that IP during a lockout, not just login -
// letting known-good credentials bypass it would partially defeat the
// point, and a legitimate user just waits out a rare, short window.
//
// In-RAM only, not persisted - a reboot clears every lockout, same as
// other per-boot state here. Tracks at most MAX_TRACKED_IPS addresses
// (a home LAN doesn't need more); the least-recently-seen entry is
// evicted once full.
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

enum class Tab { None, Network, Cameras, Users, Activity, Gallery, Firmware, Maintenance, Storage, Security };

static String renderShell(Tab active, const String& banner, const String& contentHtml) {
  String html;
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  // Inline base64 SVG data URI - a small camera-lens glyph in the same
  // blue (#2563eb) as the sidebar/buttons, so browser tabs get a real icon
  // instead of the default blank/globe placeholder. No separate asset file
  // or route needed - the whole icon lives in this one string, same
  // "self-contained, no external request" constraint as everything else
  // on this dashboard.
  html += "<link rel=\"icon\" type=\"image/svg+xml\" href=\"data:image/svg+xml;base64,"
          "PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAxMDAgMTAwIj48cmVjdCB3aWR0"
          "aD0iMTAwIiBoZWlnaHQ9IjEwMCIgcng9IjIyIiBmaWxsPSIjMjU2M2ViIi8+PGNpcmNsZSBjeD0iNTAiIGN5PSI0NiIgcj0i"
          "MjIiIGZpbGw9Im5vbmUiIHN0cm9rZT0iI2ZmZiIgc3Ryb2tlLXdpZHRoPSI3Ii8+PGNpcmNsZSBjeD0iNTAiIGN5PSI0NiIg"
          "cj0iOSIgZmlsbD0iI2ZmZiIvPjxyZWN0IHg9IjMwIiB5PSIyNCIgd2lkdGg9IjE2IiBoZWlnaHQ9IjkiIHJ4PSIzIiBmaWxs"
          "PSIjZmZmIi8+PC9zdmc+\">";
  html += "<title>Camera Monitor v" + String(FIRMWARE_VERSION) + "</title><style>";
  html += "*{box-sizing:border-box;}";
  // System font stack, not the plain "sans-serif" fallback - costs nothing
  // to serve (no font files, no external request - every name here is
  // either built into the OS or the browser silently skips to the next),
  // but reads as considerably less "unstyled default" than the generic
  // fallback ever does.
  html += "body{font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,Helvetica,Arial,"
          "sans-serif;margin:0;display:flex;min-height:100vh;color:#222;}";
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
  html += ".flipbook-img{display:none;max-width:160px;max-height:120px;vertical-align:middle;}";
  html += ".sidebar-parent{cursor:pointer;}";
  html += ".sidebar-submenu a{padding-left:36px;font-size:13px;}";
  // Real button styling instead of the browser's own default gray, chunky,
  // inconsistent-across-browsers rendering. Plain blue "primary" look by
  // default (most buttons here are ordinary save/run actions); .danger
  // overrides to red for the specific handful that are actually
  // destructive or disruptive (delete, erase-all, reboot, firmware flash) -
  // a visual reinforcement of the confirm() dialogs those already have,
  // not a replacement for them.
  html += "button{font-family:inherit;font-size:14px;padding:7px 14px;border-radius:6px;"
          "border:1px solid #2563eb;background:#2563eb;color:#fff;cursor:pointer;"
          "transition:background .15s,border-color .15s;}";
  html += "button:hover{background:#1d4ed8;border-color:#1d4ed8;}";
  html += "button:active{background:#1e40af;border-color:#1e40af;}";
  html += "button:disabled{background:#93c5fd;border-color:#93c5fd;cursor:not-allowed;}";
  html += "button.danger{background:#dc2626;border-color:#dc2626;}";
  html += "button.danger:hover{background:#b91c1c;border-color:#b91c1c;}";
  html += "button.danger:active{background:#991b1b;border-color:#991b1b;}";
  // Plain/outlined look for a minor, frequently-clicked toggle (the
  // Preview flipbook's Play/Stop button) that shouldn't visually compete
  // with an actual primary action on the same page - replaces that
  // button's old ad hoc reuse of .hint (which only ever set text color,
  // fine when buttons were unstyled, not once the base button rule above
  // gives every button a filled blue background by default).
  html += "button.secondary{background:#fff;color:#374151;border-color:#d1d5db;}";
  html += "button.secondary:hover{background:#f3f4f6;border-color:#9ca3af;}";
  // Small colored status pills - .badge-on (healthy/enabled, green),
  // .badge-warn (needs attention but not a hard failure - e.g. responding
  // but not subscribed, see telegram.cpp's checkSubscriptionHealth - amber),
  // .badge-offline (hard failure, red), .badge-off (a deliberate/intentional
  // state, not a problem - e.g. muted or disabled - neutral gray). Lets a
  // multi-camera table be scanned at a glance instead of reading a run-on
  // status sentence per row.
  html += ".badge{display:inline-block;padding:2px 8px;border-radius:10px;font-size:11px;"
          "font-weight:600;color:#fff;white-space:nowrap;}";
  html += ".badge-on{background:#16a34a;}";
  html += ".badge-warn{background:#d97706;}";
  html += ".badge-offline{background:#dc2626;}";
  html += ".badge-off{background:#6b7280;}";
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

  html += "<nav class=\"sidebar\"><div class=\"brand\">Camera Monitor v" + String(FIRMWARE_VERSION) + "</div>";
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
  html += "<a href=\"/gallery\" class=\"";
  html += (active == Tab::Gallery) ? "active" : "";
  html += "\">Gallery</a>";
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
  // ESP.restart() doesn't wait for other FreeRTOS tasks to finish
  // whatever they're doing - if a camera task is mid-write to SD at this
  // exact moment, an uncoordinated reset could corrupt more than just
  // that one file (FAT isn't a journaling filesystem). Covers both
  // callers of this function (OTA and Maintenance) automatically. See
  // waitForSdIdle's own comment (sd_store.h); no-op if SD isn't active.
  waitForSdIdle();
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

// Picks the right banner for a startXAsync() result - every background-job
// route below used to hardcode the "started" text unconditionally, even
// when tryStart() was actually a no-op (already running) or xTaskCreate
// failed (out of memory) - see BackgroundJobStartOutcome's own comment
// (background_job.h) for the incident that motivated splitting this out.
static String backgroundJobBanner(BackgroundJobStartOutcome outcome, const String& startedText,
                                   const String& alreadyRunningText, const String& failedText) {
  switch (outcome) {
    case BackgroundJobStartOutcome::AlreadyRunning: return alreadyRunningText;
    case BackgroundJobStartOutcome::FailedToStart:  return failedText;
    default:                                        return startedText;
  }
}

void startWebServer(std::vector<CameraConfig>* liveCameras, std::vector<CameraState>* liveStates) {
  g_liveCameras = liveCameras;
  g_liveStates = liveStates;

  DashboardAuth auth = loadDashboardAuth();
  g_authMiddleware.setUsername(auth.username.c_str())
      .setPassword(auth.password.c_str())
      .setRealm(("Camera Monitor v" + String(FIRMWARE_VERSION)).c_str())
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
    String landing = "<h1>Camera Monitor v" + String(FIRMWARE_VERSION) +
                      "</h1><p class=\"hint\">Select a section from the left.</p>";
    return response->send(200, "text/html", renderShell(Tab::None, "", landing).c_str());
  });

  server.on("/network", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    // prefillSsid arrives from a scanned-network "Add" link
    // (webserver_network.cpp's renderWifiScanStatus) - PsychicRequest
    // url-decodes query params automatically, and renderNetworkPanel
    // htmlEscape()s it before it ever reaches the page.
    String prefillSsid = request->getParam("prefillSsid", "");
    return response->send(
        200, "text/html", renderShell(Tab::Network, "", renderNetworkPanel(prefillSsid)).c_str());
  });

  server.on("/network/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String banner;
    handleSaveNetwork(request, banner);
    return response->send(200, "text/html", renderShell(Tab::Network, banner, renderNetworkPanel()).c_str());
  });

  server.on("/network/scan", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    // Kicks off a background task and returns immediately - see
    // startWifiScanAsync's own comment (webserver_network.h) for why this
    // must never run synchronously on this request-handling task.
    String banner = backgroundJobBanner(
        startWifiScanAsync(), "Scanning for WiFi networks in the background.",
        "A WiFi scan is already running in the background - reload in a moment to see its result.",
        "Could not start the WiFi scan - the device is low on memory right now. Try again in a moment.");
    return response->send(200, "text/html", renderShell(Tab::Network, banner, renderNetworkPanel()).c_str());
  });

  server.on("/cameras", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    // note carries a one-time status banner across the POST-redirect-GET
    // from /cameras/save (see that route below) - htmlEscape()d HERE,
    // the single point it's actually rendered into HTML, not earlier.
    // This route is reachable directly (a bare GET, not just via the
    // redirect this project itself issues), so it can never assume `note`
    // arrived pre-escaped from a trusted caller - a previous version did
    // assume exactly that, which made a direct request to
    // /cameras?note=<script>...</script> render completely unescaped
    // (reflected XSS, since PsychicRequest url-decodes query params
    // automatically). saveCameraSubmission (webserver_cameras.cpp) now
    // deliberately builds this value RAW, not pre-escaped, so it isn't
    // double-escaped here.
    String note = htmlEscape(request->getParam("note", ""));

    // prefillName/prefillUrl arrive from a discovered-camera "Add" link
    // (webserver_cameras.cpp's renderCameraDiscoveryStatus) - PsychicRequest
    // url-decodes query params automatically, and renderCameraForm below
    // htmlEscape()s both before they ever reach the page, same as every
    // other prefill path here (a failed save redisplay, an edit link).
    String prefillUrl = request->getParam("prefillUrl", "");
    CameraConfig prefill;
    CameraConfig* prefillPtr = nullptr;
    if (prefillUrl.length() > 0) {
      prefill.name = request->getParam("prefillName", "");
      prefill.deviceServiceUrl = prefillUrl;
      prefillPtr = &prefill;
    }
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, note, renderCamerasPanel(prefillPtr, false, g_liveCameras, g_liveStates))
            .c_str());
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
    // Kicks off a background task and returns immediately - see
    // startTestConnectionAsync's own comment (webserver_cameras.h) for why
    // this must never run synchronously on this request-handling task.
    // testCfg (not submitted - this one has the resolved password) is
    // heap-copied by startTestConnectionAsync, so it's safe to let it go
    // out of scope here.
    String banner = backgroundJobBanner(
        startTestConnectionAsync(testCfg),
        "Testing camera connection in the background - reload this page in a moment to see the result.",
        "A connection test is already running in the background - reload in a moment to see its result.",
        "Could not start the connection test - the device is low on memory right now. Try again in a moment.");

    submitted.pass = "";
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, banner, renderCamerasPanel(&submitted, isEdit, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/discover", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    // Kicks off a background task and returns immediately - see
    // startCameraDiscoveryAsync's own comment (webserver_cameras.h) for why
    // this must never run synchronously on this request-handling task.
    String banner = backgroundJobBanner(
        startCameraDiscoveryAsync(), "Searching the network for cameras in the background.",
        "A network search is already running in the background - reload in a moment to see its result.",
        "Could not start the network search - the device is low on memory right now. Try again in a moment.");
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, banner, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/test-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    // Kicks off a background task and returns immediately - see
    // startTestAllCamerasAsync's own comment (webserver_cameras.h) for why
    // this must never run synchronously on this request-handling task.
    String banner = backgroundJobBanner(
        startTestAllCamerasAsync(), "Test started in the background.",
        "A test is already running in the background - reload in a moment to see its result.",
        "Could not start the test - the device is low on memory right now. Try again in a moment.");
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, banner, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/mute-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String duration = request->getParam("duration", "");
    duration.trim();
    String result = setAllCamerasAlertState(g_liveCameras->data(), g_liveStates->data(), g_liveCameras->size(),
                                             false, duration, "the dashboard");
    // htmlEscape()d here, the one point this ever becomes HTML -
    // setAllCamerasAlertState's failure message (via resolveAlertTimer,
    // telegram.cpp) echoes the submitted duration text verbatim, which is
    // exactly right for its OTHER caller (a plain-text Telegram reply) but
    // was a raw reflected-XSS hole here: renderShell's banner is inserted
    // unescaped by design, same as every other banner that's pre-built
    // safe HTML - this is the one that wasn't. The success-path message
    // has nothing but fixed text/numbers in it either way, so escaping
    // unconditionally is a no-op there and doesn't need its own branch.
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, htmlEscape(result), renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/unmute-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    // No duration - "Unmute all" is a plain permanent cancel, same as a
    // bare /on all with no timer. Anyone wanting a timed unmute already has
    // Mute all's own duration field for the opposite direction, or /on all
    // <duration> via Telegram.
    String result = setAllCamerasAlertState(g_liveCameras->data(), g_liveStates->data(), g_liveCameras->size(),
                                             true, "", "the dashboard");
    // htmlEscape() for consistency with /cameras/mute-all above, even
    // though this call site always passes a fixed "" duration today (so
    // there's no actual user text to escape yet) - matching the same
    // "escape this result unconditionally" rule protects it if that ever
    // changes, rather than relying on today's call site staying that way.
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, htmlEscape(result), renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/quiet-hours-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String result = applyQuietHoursToAllCameras(request, g_liveCameras, g_liveStates);
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, result, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
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
    // Stop a running task BEFORE removing the NVS record - otherwise the
    // still-running task's own retry/pull loop would keep monitoring
    // (and alerting on) a camera the dashboard no longer even lists.
    stopLiveCameraIfRunning(name, g_liveCameras, g_liveStates);
    deleteCamera(name);
    return response->redirect("/cameras");
  });

  server.on("/users", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    // prefillChatId arrives from an unrecognized-chat-ID "Add" link
    // (webserver_users.cpp's renderUsersPanel) - PsychicRequest url-decodes
    // query params automatically, and renderTelegramUserForm's own
    // htmlEscape() covers it before it ever reaches the page, same as
    // every other prefill path here.
    String prefillChatId = request->getParam("prefillChatId", "");
    TelegramUser prefill;
    TelegramUser* prefillPtr = nullptr;
    if (prefillChatId.length() > 0) {
      prefill.chatId = prefillChatId;
      prefill.allCameras = true; // friendlier default for a brand-new user, same as the blank Add form
      prefillPtr = &prefill;
    }
    return response->send(200, "text/html", renderShell(Tab::Users, "", renderUsersPanel(prefillPtr, false)).c_str());
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

  server.on("/users/test", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    // Kicks off a background task and returns immediately - see
    // startTestMessageAsync's own comment (webserver_users.h) for why this
    // must never run synchronously on this request-handling task.
    String banner = backgroundJobBanner(
        startTestMessageAsync(), "Sending a test message in the background.",
        "A test message is already being sent in the background - reload in a moment to see its result.",
        "Could not start sending the test message - the device is low on memory right now. Try again in a "
        "moment.");
    return response->send(
        200, "text/html",
        renderShell(Tab::Users, banner, renderUsersPanel(nullptr, false)).c_str());
  });

  server.on("/activity", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Activity, "", renderActivityPanel()).c_str());
  });

  // Same Content-Disposition download pattern as /export below.
  server.on("/activity/download", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    String content;
    if (!readActivityLogFile(&content)) {
      return response->send(200, "text/plain", "SD storage isn't active - nothing persisted to download.");
    }
    response->addHeader("Content-Disposition", "attachment; filename=\"activity-log.txt\"");
    return response->send(200, "text/plain", content.c_str());
  });

  server.on("/gallery", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    String camera = request->getParam("camera", "");
    long page = request->getParam("page", "0").toInt();
    if (page < 0) page = 0; // a negative/garbage param falls back to the newest page, not undefined behavior
    return response->send(
        200, "text/html",
        renderShell(Tab::Gallery, "", renderGalleryPanel(camera, (size_t)page, g_liveCameras, g_liveStates))
            .c_str());
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
      // A dropped/failed connection mid-upload never delivers last=true
      // to this callback (the multipart parser never sees a final chunk),
      // so neither Update.end() nor Update.abort() below would ever run
      // for that attempt - Update.begin() then refuses every subsequent
      // attempt (it's still "running" from the abandoned one) with a
      // generic error, permanently blocking firmware updates until a
      // physical reboot. index==0 only ever fires once per upload, so
      // isRunning()==true here can only mean state left over from an
      // earlier, incomplete attempt - clean it up before starting fresh.
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
    // The response above already promised a reboot is coming, and
    // Update.end(true) already committed the new image as bootable - if
    // task creation fails here (out of memory, plausible right after a
    // multi-hundred-KB firmware upload), nothing else will ever call
    // ESP.restart() and the new firmware silently never takes effect until
    // some unrelated later reboot. No user-facing recovery is possible at
    // this point (the response is already sent) - logging loudly is the
    // best available: a manual reboot (Maintenance page, once memory frees
    // up) is what actually applies the update.
    if (xTaskCreate(delayedRebootTask, "otaReboot", 2048, nullptr, 1, nullptr) != pdPASS) {
      Serial.println("[Firmware] ERROR: failed to start the post-update reboot task (out of memory?) - "
                      "the new firmware is flashed but the board will NOT reboot on its own. Reboot "
                      "manually from the Maintenance page once memory frees up.");
    }
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
    // See the OTA reboot handler's own comment above (/firmware/update) -
    // same failure mode, no user-facing recovery possible once the
    // response above is already sent, so just log loudly.
    if (xTaskCreate(delayedRebootTask, "maintReboot", 2048, nullptr, 1, nullptr) != pdPASS) {
      Serial.println("[Maintenance] ERROR: failed to start the reboot task (out of memory?) - the "
                      "board will NOT reboot. Try again once memory frees up, or power-cycle manually.");
    }
    return result;
  });

  server.on("/storage", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Storage, "", renderStoragePanel()).c_str());
  });

  server.on("/storage/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    SdSettings settings;
    settings.enabled = request->hasParam("enabled");
    long intervalHours = request->getParam("checkIntervalHours", "0").toInt();
    if (intervalHours < 0) intervalHours = 0;
    if (intervalHours > (long)SD_CHECK_INTERVAL_MAX_HOURS) intervalHours = (long)SD_CHECK_INTERVAL_MAX_HOURS;
    settings.checkIntervalHours = (uint32_t)intervalHours;
    String banner = saveSdSettings(settings)
        ? "Saved - the enable/disable setting needs a reboot to apply; the check interval is active immediately."
        : "Failed to save - NVS write error (see Serial log). Setting was NOT changed.";
    return response->send(200, "text/html", renderShell(Tab::Storage, banner, renderStoragePanel()).c_str());
  });

  server.on("/storage/check", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    // Kicks off a background task and returns immediately - see
    // startStorageCheckAsync's own comment (webserver_storage.h) for why
    // this must never run synchronously on this request-handling task.
    String banner = backgroundJobBanner(
        startStorageCheckAsync(), "Checking storage in the background.",
        "A storage check is already running in the background - reload in a moment to see its result.",
        "Could not start the storage check - the device is low on memory right now. Try again in a moment.");
    return response->send(200, "text/html", renderShell(Tab::Storage, banner, renderStoragePanel()).c_str());
  });

  server.on("/storage/erase", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    Serial.println("[Storage] Erase all snapshot history requested via dashboard.");
    // Kicks off a background task and returns immediately - see
    // startEraseAllAsync's own comment (webserver_storage.h) for why this
    // must never run synchronously on this request-handling task.
    String banner = backgroundJobBanner(
        startEraseAllAsync(), "Erasing all snapshot history in the background.",
        "An erase is already running in the background - reload in a moment to see its result.",
        "Could not start the erase - the device is low on memory right now. Try again in a moment.");
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

  // A real multipart file upload, not a form field - see renderSecurityPanel's
  // own comment (webserver_security.cpp) on the Import fieldset for why: a
  // plain form field is bounded by PsychicHttp's default 16K
  // maxRequestBodySize (checked before this project's own route handlers ever
  // run), and a multi-camera export's newline/\x1F-heavy text can blow past
  // that once the browser percent-encodes it. importHandler streams in
  // FILE_CHUNK_SIZE pieces instead, bounded by the much larger maxUploadSize -
  // same mechanism startWebServer's otaHandler above already uses for the
  // firmware .bin, just accumulated into a String here (a config export is
  // KB-sized, not MB, so buffering the whole thing is fine) instead of
  // streamed straight to flash.
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
      g_importBanner = renderImportResultBanner(r);
    }
    return ESP_OK;
  });
  importHandler->onRequest([](PsychicRequest* request, PsychicResponse* response) -> esp_err_t {
    return response->send(200, "text/html", renderShell(Tab::Security, g_importBanner, renderSecurityPanel()).c_str());
  });
  server.on("/import", HTTP_POST, importHandler);

  // Downloads the snapshot applyConfigImport() (webserver_security.cpp)
  // automatically saves of whatever was stored just before the most recent
  // import - in the exact same format buildConfigExport() produces, so
  // undoing a bad import is just importing this file back. Same
  // Content-Disposition pattern as /export.
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
  Serial.printf("[WebServer] Camera management UI listening on http://%s:80/\n",
                WiFi.localIP().toString().c_str());
}
