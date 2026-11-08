#include "webserver.h"
#include "webserver_internal.h"
#include "build_version.h" // FIRMWARE_VERSION
#include "webserver_network.h"
#include "webserver_cameras.h"
#include "webserver_users.h"
#include "webserver_storage.h"
#include "webserver_hardware.h" // hardwareJobsInProgress
#include "ui_settings.h"
#include "auth_store.h"
#include "sd_store.h" // waitForSdIdle
#include "backoff.h"
#include "format_utils.h"
#include <PsychicHttp.h>
#include <WiFi.h> // WiFi.localIP() - the startup "listening on" log line

// Server setup, login middleware and the dashboard shell. Each page's content
// and routes live in its own webserver_<page>.cpp.

static PsychicHttpServer server;
std::vector<CameraConfig>* g_liveCameras = nullptr;
std::vector<CameraState>*  g_liveStates  = nullptr;

// Only enforces a login once username and password are both set, so an
// unconfigured board is open. Updated live by the Security page.
static AuthenticationMiddleware g_authMiddleware;

// ============================================================
// Login rate limiting (Basic Auth has none of its own). Registered before
// g_authMiddleware - middleware runs in registration order - so a locked-out
// IP never reaches the credential check.
//
// RATE_LIMIT_MAX_FAILURES consecutive failures lock an IP out for a doubling
// duration (capped); one success clears it. A lockout blocks every route for
// that IP, even with good credentials. RAM-only; at most MAX_TRACKED_IPS,
// least recently seen evicted.
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

    // Pre-check with isAllowed() (public, side-effect free) to know whether to
    // count a failure; g_authMiddleware still issues the real 401 via next().
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
  // Caller holds mutex_. Exact match, else a free slot, else the least
  // recently seen entry (reset).
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
// Dashboard shell: server-rendered pages, everything embedded in the firmware.
// ============================================================


// Checked on every render so the page auto-refreshes while a job runs.
static bool tabHasActiveBackgroundJob(Tab active) {
  switch (active) {
    case Tab::Cameras: return cameraJobsInProgress();
    case Tab::Users:   return userJobsInProgress();
    case Tab::Network: return networkJobsInProgress();
    case Tab::Storage: return storageJobsInProgress();
    case Tab::HardwareCameras: return hardwareJobsInProgress();
    default:           return false;
  }
}

// Auto-refresh goes to the tab's plain GET page, not location.reload():
// reloading a page reached by POST silently resubmits it, which restarted a
// discovery search on every poll.
static const char* tabListingUrl(Tab active) {
  switch (active) {
    case Tab::Cameras: return "/cameras";
    case Tab::Users:   return "/users";
    case Tab::Network: return "/network";
    case Tab::Storage: return "/storage";
    case Tab::HardwareCameras: return "/hardware/cameras";
    default:           return "/cameras"; // unreachable - see this function's own comment
  }
}

String renderShell(Tab active, const String& banner, const String& contentHtml) {
  String html;
  // Theme is a saved setting (ui_settings.h), not the OS preference.
  bool darkMode = loadUiSettings().darkMode;
  html += "<!DOCTYPE html><html";
  if (darkMode) html += " data-theme=\"dark\"";
  html += "><head><meta charset=\"utf-8\">";
  html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  bool autoRefresh = tabHasActiveBackgroundJob(active);
  // Inline SVG favicon; no extra route.
  html += "<link rel=\"icon\" type=\"image/svg+xml\" href=\"data:image/svg+xml;base64,"
          "PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAxMDAgMTAwIj48cmVjdCB3aWR0"
          "aD0iMTAwIiBoZWlnaHQ9IjEwMCIgcng9IjIyIiBmaWxsPSIjMjU2M2ViIi8+PGNpcmNsZSBjeD0iNTAiIGN5PSI0NiIgcj0i"
          "MjIiIGZpbGw9Im5vbmUiIHN0cm9rZT0iI2ZmZiIgc3Ryb2tlLXdpZHRoPSI3Ii8+PGNpcmNsZSBjeD0iNTAiIGN5PSI0NiIg"
          "cj0iOSIgZmlsbD0iI2ZmZiIvPjxyZWN0IHg9IjMwIiB5PSIyNCIgd2lkdGg9IjE2IiBoZWlnaHQ9IjkiIHJ4PSIzIiBmaWxs"
          "PSIjZmZmIi8+PC9zdmc+\">";
  html += "<title>Camera Monitor v" + String(FIRMWARE_VERSION) + "</title><style>";
  // Colours as CSS variables so dark mode just redefines them. The sidebar is
  // dark in both themes.
  html += ":root{--bg:#fff;--panel:#fff;--text:#222;--border:#ccc;--th-bg:#f0f0f0;--hint:#666;"
          "--sidebar-bg:#1f2937;--sidebar-text:#e5e7eb;--sidebar-link:#cbd5e1;--sidebar-hover:#374151;"
          "--accent:#2563eb;--accent-hover:#1d4ed8;--accent-active:#1e40af;--accent-disabled:#93c5fd;"
          "--on-accent:#fff;--danger:#dc2626;--danger-hover:#b91c1c;--danger-active:#991b1b;"
          "--secondary-text:#374151;--secondary-border:#d1d5db;--secondary-hover:#f3f4f6;"
          "--secondary-hover-border:#9ca3af;--badge-on:#16a34a;--badge-warn:#d97706;--badge-offline:#dc2626;"
          "--badge-off:#6b7280;--banner-bg:#fffae0;--banner-border:#e0d080;--banner-warn-bg:#fde2e1;"
          "--banner-warn-border:#e08080;}";
  html += "[data-theme=\"dark\"]{--bg:#111827;--panel:#1f2937;--text:#e5e7eb;--border:#374151;"
          "--th-bg:#1f2937;--hint:#9ca3af;--accent:#3b82f6;--accent-hover:#2563eb;--accent-active:#1d4ed8;"
          "--accent-disabled:#1e3a8a;--danger:#ef4444;--danger-hover:#dc2626;--danger-active:#b91c1c;"
          "--secondary-text:#d1d5db;--secondary-border:#4b5563;--secondary-hover:#374151;"
          "--secondary-hover-border:#6b7280;--banner-bg:#3f3512;--banner-border:#a1811f;"
          "--banner-warn-bg:#3f1e1e;--banner-warn-border:#a34343;}";
  html += "*{box-sizing:border-box;}";
  // System font stack: no font files, less "unstyled" than sans-serif.
  html += "body{font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,Helvetica,Arial,"
          "sans-serif;margin:0;display:flex;min-height:100vh;background:var(--bg);color:var(--text);}";
  html += ".sidebar{width:200px;flex-shrink:0;background:var(--sidebar-bg);color:var(--sidebar-text);"
          "padding:20px 0;display:flex;flex-direction:column;}";
  html += ".sidebar .brand{font-weight:bold;font-size:16px;padding:0 20px 20px;}";
  html += ".sidebar a{display:block;padding:10px 20px;color:var(--sidebar-link);text-decoration:none;"
          "font-size:14px;}";
  html += ".sidebar a:hover{background:var(--sidebar-hover);}";
  html += ".sidebar a.active{background:var(--accent);color:var(--on-accent);font-weight:bold;}";
  html += ".sidebar-footer{margin-top:auto;padding:12px 20px 0;display:flex;flex-direction:column;gap:8px;}";
  html += ".content{flex:1;padding:24px 28px;max-width:960px;}";
  html += "h1{font-size:20px;margin-top:0;}";
  html += "table{border-collapse:collapse;width:100%;margin-bottom:24px;}";
  html += "th,td{border:1px solid var(--border);padding:6px 8px;text-align:left;font-size:14px;"
          "vertical-align:top;}";
  html += "th{background:var(--th-bg);}";
  html += "form.inline{display:inline;}";
  html += "fieldset{margin-bottom:20px;}";
  html += "label{display:block;margin-top:10px;font-size:14px;}";
  html += "label.checkbox{display:flex;align-items:center;gap:6px;font-weight:normal;}";
  html += "label.checkbox input{width:auto;}";
  html += "input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:5px;"
          "margin-top:2px;background:var(--panel);color:var(--text);border:1px solid var(--border);}";
  html += ".camera-list{border:1px solid var(--border);padding:8px;max-height:180px;overflow-y:auto;"
          "margin-top:2px;}";
  html += ".camera-list label{margin-top:2px;}";
  html += ".banner{background:var(--banner-bg);border:1px solid var(--banner-border);padding:8px 12px;"
          "margin-bottom:16px;}";
  html += ".banner-warn{background:var(--banner-warn-bg);border:1px solid var(--banner-warn-border);"
          "padding:8px 12px;margin-bottom:16px;}";
  html += ".hint{color:var(--hint);font-size:13px;}";
  html += ".flipbook-img{display:none;max-width:160px;max-height:120px;vertical-align:middle;}";
  html += ".sidebar-parent{cursor:pointer;}";
  html += ".sidebar-submenu a{padding-left:36px;font-size:13px;}";
  // Sidebar footer controls get an outlined look to suit the dark sidebar.
  html += ".sidebar-footer button,.sidebar-footer a{display:block;width:100%;text-align:left;"
          "font-family:inherit;font-size:13px;padding:8px 12px;border-radius:6px;"
          "background:transparent;color:var(--sidebar-link);border:1px solid var(--sidebar-hover);"
          "cursor:pointer;text-decoration:none;box-sizing:border-box;}";
  html += ".sidebar-footer button:hover,.sidebar-footer a:hover{background:var(--sidebar-hover);color:#fff;}";
  // Blue primary buttons; .danger (red) marks destructive actions, alongside
  // their confirm() dialogs.
  html += "button{font-family:inherit;font-size:14px;padding:7px 14px;border-radius:6px;"
          "border:1px solid var(--accent);background:var(--accent);color:var(--on-accent);cursor:pointer;"
          "transition:background .15s,border-color .15s;}";
  html += "button:hover{background:var(--accent-hover);border-color:var(--accent-hover);}";
  html += "button:active{background:var(--accent-active);border-color:var(--accent-active);}";
  html += "button:disabled{background:var(--accent-disabled);border-color:var(--accent-disabled);"
          "cursor:not-allowed;}";
  html += "button.danger{background:var(--danger);border-color:var(--danger);}";
  html += "button.danger:hover{background:var(--danger-hover);border-color:var(--danger-hover);}";
  html += "button.danger:active{background:var(--danger-active);border-color:var(--danger-active);}";
  // Outlined style for minor toggles (e.g. flipbook Play).
  html += "button.secondary{background:var(--panel);color:var(--secondary-text);"
          "border-color:var(--secondary-border);}";
  html += "button.secondary:hover{background:var(--secondary-hover);border-color:var(--secondary-hover-border);}";
  // Same look for <a> links (Cancel), which don't get the button rule.
  html += "a.secondary{display:inline-block;font-family:inherit;font-size:14px;padding:7px 14px;"
          "border-radius:6px;text-decoration:none;cursor:pointer;background:var(--panel);"
          "color:var(--secondary-text);border:1px solid var(--secondary-border);}";
  html += "a.secondary:hover{background:var(--secondary-hover);border-color:var(--secondary-hover-border);}";
  // Status pills: on (green), warn (amber, e.g. not subscribed), offline
  // (red), off (grey, deliberate state). Same colours in both themes.
  html += ".badge{display:inline-block;padding:2px 8px;border-radius:10px;font-size:11px;"
          "font-weight:600;color:#fff;white-space:nowrap;}";
  html += ".badge-on{background:var(--badge-on);}";
  html += ".badge-warn{background:var(--badge-warn);}";
  html += ".badge-offline{background:var(--badge-offline);}";
  html += ".badge-off{background:var(--badge-off);}";
  // Compact icon buttons for row Edit/Delete, for both <a> and <button>.
  html += ".icon-btn{display:inline-flex;align-items:center;justify-content:center;"
          "width:30px;height:30px;padding:0;font-size:15px;line-height:1;border-radius:6px;"
          "border:1px solid var(--accent);background:var(--accent);color:var(--on-accent);cursor:pointer;"
          "text-decoration:none;transition:background .15s,border-color .15s;}";
  html += ".icon-btn:hover{background:var(--accent-hover);border-color:var(--accent-hover);}";
  html += ".icon-btn.danger{background:var(--danger);border-color:var(--danger);}";
  html += ".icon-btn.danger:hover{background:var(--danger-hover);border-color:var(--danger-hover);}";
  html += ".icon-btn.secondary{background:var(--panel);color:var(--secondary-text);"
          "border-color:var(--secondary-border);}";
  html += ".icon-btn.secondary:hover{background:var(--secondary-hover);"
          "border-color:var(--secondary-hover-border);}";
  html += ".row-actions{display:flex;gap:6px;}";
  html += "</style></head><body>";

  // Submenus start open when one of their pages is active, so the current
  // page's entry isn't hidden. Inline onclick toggles only.
  bool systemOpen = (active == Tab::Firmware || active == Tab::Maintenance || active == Tab::Storage ||
                      active == Tab::Capabilities);
  bool hardwareOpen = (active == Tab::HardwareInternet || active == Tab::HardwareBridge ||
                        active == Tab::HardwarePower || active == Tab::HardwareRtc ||
                        active == Tab::HardwareCameras);

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
  html += "<a href=\"/capabilities\" class=\"";
  html += (active == Tab::Capabilities) ? "active" : "";
  html += "\">Capabilities</a>";
  html += "</div>";
  html += "<a href=\"#\" class=\"sidebar-parent\" onclick=\"var m=document.getElementById('hardware-submenu');"
          "m.style.display=(m.style.display==='block')?'none':'block';return false;\">Hardware</a>";
  html += "<div id=\"hardware-submenu\" class=\"sidebar-submenu\" style=\"display:";
  html += hardwareOpen ? "block" : "none";
  html += ";\">";
  html += "<a href=\"/hardware/internet\" class=\"";
  html += (active == Tab::HardwareInternet) ? "active" : "";
  html += "\">Internet</a>";
  html += "<a href=\"/hardware/bridge\" class=\"";
  html += (active == Tab::HardwareBridge) ? "active" : "";
  html += "\">WiFi Bridge</a>";
  html += "<a href=\"/hardware/power\" class=\"";
  html += (active == Tab::HardwarePower) ? "active" : "";
  html += "\">220V Power</a>";
  html += "<a href=\"/hardware/rtc\" class=\"";
  html += (active == Tab::HardwareRtc) ? "active" : "";
  html += "\">External RTC</a>";
  html += "<a href=\"/hardware/cameras\" class=\"";
  html += (active == Tab::HardwareCameras) ? "active" : "";
  html += "\">Cameras</a>";
  html += "</div>";
  html += "<a href=\"/security\" class=\"";
  html += (active == Tab::Security) ? "active" : "";
  html += "\">Security</a>";

  DashboardAuth currentAuth = loadDashboardAuth();
  html += "<div class=\"sidebar-footer\">";
  html += "<form method=\"POST\" action=\"/ui/theme/toggle\"><button type=\"submit\">";
  html += darkMode ? "&#9728; Light mode" : "&#127769; Dark mode";
  html += "</button></form>";
  // Only when a login is enforced. Basic Auth has no server session, so logout
  // sends a fetch with wrong credentials to replace the browser's cached ones.
  if (currentAuth.username.length() > 0 && currentAuth.password.length() > 0) {
    html += "<a href=\"#\" onclick=\""
            "fetch(location.origin,{headers:{Authorization:'Basic eDp4'}})"
            ".catch(function(){}).then(function(){location.href='/';});return false;\">Logout</a>";
  }
  html += "</div>";
  html += "</nav>";

  html += "<main class=\"content\">";
  if (currentAuth.username.length() == 0 || currentAuth.password.length() == 0) {
    html += "<div class=\"banner-warn\">No dashboard password is set - anyone on your LAN can view "
            "and change everything here, including WiFi/camera credentials, the Firmware page, the "
            "Maintenance page's reboot button, and the Storage page's erase-all-history button. "
            "<a href=\"/security\">Set one now</a>.</div>";
  }
  if (banner.length() > 0) html += "<div class=\"banner\">" + banner + "</div>";
  html += contentHtml;
  // Poll while a background job runs, but skip the reload while a form field
  // has focus (a meta refresh would wipe typing). Navigates to the tab's GET
  // page (see tabListingUrl).
  if (autoRefresh) {
    html += "<script>setInterval(function(){"
            "var t=document.activeElement&&document.activeElement.tagName;"
            "if(t!=='INPUT'&&t!=='TEXTAREA'&&t!=='SELECT')location.href='" + String(tabListingUrl(active)) +
            "';"
            "},2000);</script>";
  }
  html += "</main></body></html>";
  return html;
}

// Restart from a short task after the response is sent; restarting inside the
// handler drops the connection first. Used by OTA and Maintenance.
void delayedRebootTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(1000));
  // Don't restart mid SD write: FAT isn't journaled.
  waitForSdIdle();
  ESP.restart();
}

// Banner text matching what actually happened (started, already running, or
// failed to start).
String backgroundJobBanner(BackgroundJobStartOutcome outcome, const String& startedText,
                                   const String& alreadyRunningText, const String& failedText) {
  switch (outcome) {
    case BackgroundJobStartOutcome::AlreadyRunning: return alreadyRunningText;
    case BackgroundJobStartOutcome::FailedToStart:  return failedText;
    default:                                        return startedText;
  }
}

void applyDashboardLogin(const DashboardAuth& auth) {
  g_authMiddleware.setUsername(auth.username.c_str()).setPassword(auth.password.c_str());
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
  // Rate limiter first so locked-out IPs never reach auth. Both cover every
  // route, including firmware upload.
  server.addMiddleware(&g_rateLimitMiddleware);
  server.addMiddleware(&g_authMiddleware);

  server.on("/", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    String landing = "<h1>Camera Monitor v" + String(FIRMWARE_VERSION) +
                      "</h1><p class=\"hint\">Select a section from the left.</p>";
    return response->send(200, "text/html", renderShell(Tab::None, "", landing).c_str());
  });

  // Returns to the referring page ("/" if none).
  server.on("/ui/theme/toggle", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    UiSettings settings = loadUiSettings();
    settings.darkMode = !settings.darkMode;
    saveUiSettings(settings);
    String redirectUrl = request->header("Referer");
    if (redirectUrl.length() == 0) redirectUrl = "/";
    return response->redirect(redirectUrl.c_str());
  });

  registerNetworkRoutes(server);
  registerCameraRoutes(server);
  registerUserRoutes(server);
  registerActivityRoutes(server);
  registerGalleryRoutes(server);
  registerFirmwareRoutes(server);
  registerMaintenanceRoutes(server);
  registerHardwareRoutes(server);
  registerStorageRoutes(server);
  registerCapabilitiesRoutes(server);
  registerSecurityRoutes(server);
  registerDebugRoutes(server);

  server.begin();
  Serial.printf("[WebServer] Camera management UI listening on http://%s:80/\n",
                WiFi.localIP().toString().c_str());
}
