#include "webserver.h"
#include "config.h" // SD_CHECK_INTERVAL_MAX_HOURS
#include "build_version.h" // FIRMWARE_VERSION
#include "webserver_network.h"
#include "webserver_cameras.h"
#include "webserver_users.h"
#include "webserver_firmware.h"
#include "webserver_maintenance.h"
#include "webserver_hardware.h"
#include "webserver_security.h"
#include "config_backup.h"
#include "webserver_activity.h"
#include "webserver_gallery.h"
#include "webserver_storage.h"
#include "webserver_capabilities.h"
#include "ui_settings.h"
#include "rtc_store.h"
#include "net_watchdog.h"
#include "bridge_watchdog.h"
#include "power_monitor.h"
#include "net_watchdog_logic.h" // isReservedOrUnsafePin, watchdogPinsConflict
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

// Routing, the dashboard shell and OTA upload state. Each panel's content
// lives in its own webserver_<panel>.cpp.

static PsychicHttpServer server;
static std::vector<CameraConfig>* g_liveCameras = nullptr;
static std::vector<CameraState>*  g_liveStates  = nullptr;

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

enum class Tab { None, Network, Cameras, Users, Activity, Gallery, Firmware, Maintenance, Storage, Security,
                  HardwareInternet, HardwareBridge, HardwarePower, Capabilities };

// Checked on every render so the page auto-refreshes while a job runs.
static bool tabHasActiveBackgroundJob(Tab active) {
  switch (active) {
    case Tab::Cameras: return cameraJobsInProgress();
    case Tab::Users:   return userJobsInProgress();
    case Tab::Network: return networkJobsInProgress();
    case Tab::Storage: return storageJobsInProgress();
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
    default:           return "/cameras"; // unreachable - see this function's own comment
  }
}

static String renderShell(Tab active, const String& banner, const String& contentHtml) {
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
                        active == Tab::HardwarePower);

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
static void delayedRebootTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(1000));
  // Don't restart mid SD write: FAT isn't journaled.
  waitForSdIdle();
  ESP.restart();
}

// ============================================================
// Firmware upload (OTA). Update writes to the inactive app partition and only
// marks it bootable once verified, so a failed upload changes nothing.
// ============================================================

static bool   g_otaError = false;
static String g_otaErrorMsg;

// Banner text matching what actually happened (started, already running, or
// failed to start).
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

  server.on("/network", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    // From a scan result's Add link; escaped when rendered.
    String prefillSsid = request->getParam("prefillSsid", "");
    return response->send(
        200, "text/html", renderShell(Tab::Network, "", renderNetworkPanel(prefillSsid)).c_str());
  });

  server.on("/network/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String banner;
    handleSaveNetwork(request, banner);
    return response->send(200, "text/html", renderShell(Tab::Network, banner, renderNetworkPanel()).c_str());
  });

  server.on("/network/rtc/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    RtcSettings settings;
    settings.enabled = request->hasParam("enabled");
    String banner = saveRtcSettings(settings)
        ? "Saved - reboot the board to apply."
        : "Failed to save - NVS write error (see Serial log). Setting was NOT changed.";
    return response->send(200, "text/html", renderShell(Tab::Network, banner, renderNetworkPanel()).c_str());
  });

  server.on("/network/scan", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    // Background task; returns immediately.
    String banner = backgroundJobBanner(
        startWifiScanAsync(), "Scanning for WiFi networks in the background.",
        "A WiFi scan is already running in the background - reload in a moment to see its result.",
        "Could not start the WiFi scan - the device is low on memory right now. Try again in a moment.");
    return response->send(200, "text/html", renderShell(Tab::Network, banner, renderNetworkPanel()).c_str());
  });

  server.on("/cameras", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    // One-time banner from /cameras/save. Escaped here, where it's rendered:
    // this route can be requested directly, and trusting a pre-escaped value
    // once allowed ?note=<script> (reflected XSS). The save builds it raw.
    String note = htmlEscape(request->getParam("note", ""));

    // From a discovery result's Add link; escaped when rendered.
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
    // Redirect (POST-redirect-GET) even with a note, so refresh doesn't
    // resubmit.
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
    // Background task; testCfg (with the resolved password) is copied.
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
    // Background task; returns immediately.
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
    // Background task; returns immediately.
    String banner = backgroundJobBanner(
        startTestAllCamerasAsync(), "Test started in the background.",
        "A test is already running in the background - reload in a moment to see its result.",
        "Could not start the test - the device is low on memory right now. Try again in a moment.");
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, banner, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/test-alert", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String name = request->getParam("name", "");
    name.trim();

    // Unknown values fall back to Generic; "pet" sets isPetEvent instead.
    String kindParam = request->getParam("kind", "generic");
    MotionDetectionKind kind = MotionDetectionKind::Generic;
    bool isPetEvent = false;
    if (kindParam == "person") kind = MotionDetectionKind::Person;
    else if (kindParam == "vehicle") kind = MotionDetectionKind::Vehicle;
    else if (kindParam == "pet") isPetEvent = true;

    CameraConfig cfg;
    bool found = false;
    for (auto& c : loadCameras()) {
      if (c.name.equalsIgnoreCase(name)) { cfg = c; found = true; break; }
    }

    int idx = -1;
    if (g_liveCameras) {
      for (size_t i = 0; i < g_liveCameras->size(); i++) {
        if ((*g_liveCameras)[i].name.equalsIgnoreCase(name)) { idx = (int)i; break; }
      }
    }

    String banner;
    if (!found || idx < 0 || !g_liveStates || idx >= (int)g_liveStates->size() || !(*g_liveCameras)[idx].enabled) {
      // No live CameraState to send through.
      banner = "Camera \"" + htmlEscape(name) + "\" isn't currently running (disabled, or added since "
               "the last reboot) - nothing to test. Not sent.";
    } else {
      // Background task. Passes the live CameraState (not a copy) for its
      // resolved snapshot URI and credentials.
      banner = backgroundJobBanner(
          startTestAlertAsync(cfg, (*g_liveStates)[idx], kind, isPetEvent),
          "Sending test alert in the background - reload this page in a moment to see the result.",
          "A test alert is already being sent in the background - reload in a moment to see its result.",
          "Could not start sending the test alert - the device is low on memory right now. Try again in "
          "a moment.");
    }
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, banner, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/mute-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String duration = request->getParam("duration", "");
    duration.trim();
    String result = setAllCamerasAlertState(g_liveCameras->data(), g_liveStates->data(), g_liveCameras->size(),
                                             false, duration, "the dashboard", TelegramLang::English);
    // Escape the result: a failure echoes the submitted duration text, and
    // banners are inserted unescaped (this was a reflected XSS).
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, htmlEscape(result), renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/unmute-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    // Unmute all is permanent, like a bare /on all.
    String result = setAllCamerasAlertState(g_liveCameras->data(), g_liveStates->data(), g_liveCameras->size(),
                                             true, "", "the dashboard", TelegramLang::English);
    // Escaped for consistency with mute-all, in case a duration is ever added.
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

  server.on("/cameras/person-alerts-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String result = applyPersonAlertsToAllCameras(request, g_liveCameras, g_liveStates);
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, result, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/vehicle-alerts-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String result = applyVehicleAlertsToAllCameras(request, g_liveCameras, g_liveStates);
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, result, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  server.on("/cameras/pet-alerts-all", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String result = applyPetAlertsToAllCameras(request, g_liveCameras, g_liveStates);
    return response->send(
        200, "text/html",
        renderShell(Tab::Cameras, result, renderCamerasPanel(nullptr, false, g_liveCameras, g_liveStates))
            .c_str());
  });

  // One history entry (SD or PSRAM ring, per snapshot_history.h); age 0 =
  // newest. Behind rate limiting and auth like every route - it's camera
  // footage.
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

    // The bytes are copied out, so nothing is held during send().
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
    // Stop the task before removing the record, or it keeps alerting on a
    // camera the dashboard no longer lists.
    stopLiveCameraIfRunning(name, g_liveCameras, g_liveStates);
    deleteCamera(name);
    return response->redirect("/cameras");
  });

  server.on("/users", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    // From an unknown-chat Add link; escaped when rendered.
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
    // Background task; returns immediately.
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
    String source = request->getParam("source", "");
    String date = request->getParam("date", "");
    long page = request->getParam("page", "0").toInt();
    if (page < 0) page = 0; // a negative/garbage param falls back to the newest page, not undefined behavior
    return response->send(
        200, "text/html",
        renderShell(Tab::Gallery, "",
                    renderGalleryPanel(camera, (size_t)page, source, date, g_liveCameras, g_liveStates))
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
    // Same as the OTA reboot above: log only.
    if (xTaskCreate(delayedRebootTask, "maintReboot", 2048, nullptr, 1, nullptr) != pdPASS) {
      Serial.println("[Maintenance] ERROR: failed to start the reboot task (out of memory?) - the "
                      "board will NOT reboot. Try again once memory frees up, or power-cycle manually.");
    }
    return result;
  });

  server.on("/hardware/internet", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareInternet, "", renderInternetWatchdogPage()).c_str());
  });

  server.on("/hardware/internet/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    NetWatchdogSettings settings;
    settings.enabled = request->hasParam("enabled");
    settings.activeLow = request->hasParam("activeLow");

    // Fallback matches NET_WATCHDOG_PIN_DEFAULT (getParam takes a const
    // char*).
    settings.pin = request->getParam("pin", "4").toInt();

    // Clamped here and again at use in net_watchdog.cpp.
    long thresholdMinutes = request->getParam("outageThresholdMinutes", "5").toInt();
    if (thresholdMinutes < 1) thresholdMinutes = 1;
    unsigned long thresholdMs = (unsigned long)thresholdMinutes * 60000UL;
    if (thresholdMs > NET_WATCHDOG_THRESHOLD_MAX_MS) thresholdMs = NET_WATCHDOG_THRESHOLD_MAX_MS;
    settings.outageThresholdMs = thresholdMs;

    long pulseSeconds = request->getParam("pulseSeconds", "10").toInt();
    if (pulseSeconds < 1) pulseSeconds = 1;
    unsigned long pulseMs = (unsigned long)pulseSeconds * 1000UL;
    if (pulseMs > NET_WATCHDOG_PULSE_MAX_MS) pulseMs = NET_WATCHDOG_PULSE_MAX_MS;
    settings.pulseDurationMs = pulseMs;

    String banner;
    BridgeWatchdogSettings bridgeSettings = loadBridgeWatchdogSettings();
    PowerMonitorSettings powerSettings = loadPowerMonitorSettings();
    if (settings.enabled && isReservedOrUnsafePin(settings.pin)) {
      banner = "Pin " + String(settings.pin) + " is reserved by another peripheral on this board or "
               "unsafe to use for general GPIO - pick a different one. Not saved.";
    } else if (watchdogPinsConflict(settings.enabled, settings.pin, bridgeSettings.enabled, bridgeSettings.pin)) {
      banner = "Pin " + String(settings.pin) + " is already used by the Camera Bridge Watchdog - pick "
               "a different one. Not saved.";
    } else if (watchdogPinsConflict(settings.enabled, settings.pin, powerSettings.enabled, powerSettings.pin)) {
      banner = "Pin " + String(settings.pin) + " is already used by the 220V Power Monitor - pick a "
               "different one. Not saved.";
    } else {
      banner = saveNetWatchdogSettings(settings)
          ? "Saved - the enable checkbox and pin need a reboot to apply; the threshold and pulse "
            "duration are active immediately."
          : "Failed to save - NVS write error (see Serial log). Setting was NOT changed.";
    }
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareInternet, banner, renderInternetWatchdogPage()).c_str());
  });

  server.on("/hardware/internet/pulse", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String banner = netWatchdogManualPulse()
        ? "Relay pulsed."
        : "Internet Watchdog isn't active (disabled, or the configured pin wasn't accepted at boot) - nothing to pulse.";
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareInternet, banner, renderInternetWatchdogPage()).c_str());
  });

  server.on("/hardware/bridge", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareBridge, "", renderBridgeWatchdogPage(g_liveCameras)).c_str());
  });

  server.on("/hardware/bridge/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    BridgeWatchdogSettings settings;
    settings.enabled = request->hasParam("enabled");
    settings.activeLow = request->hasParam("activeLow");
    settings.cameraA = request->getParam("cameraA", "");
    settings.cameraB = request->getParam("cameraB", "");

    // Fallback matches BRIDGE_WATCHDOG_PIN_DEFAULT.
    settings.pin = request->getParam("pin", "5").toInt();

    // Clamped here and again at use in bridge_watchdog.cpp.
    long thresholdMinutes = request->getParam("outageThresholdMinutes", "5").toInt();
    if (thresholdMinutes < 1) thresholdMinutes = 1;
    unsigned long thresholdMs = (unsigned long)thresholdMinutes * 60000UL;
    if (thresholdMs > BRIDGE_WATCHDOG_THRESHOLD_MAX_MS) thresholdMs = BRIDGE_WATCHDOG_THRESHOLD_MAX_MS;
    settings.outageThresholdMs = thresholdMs;

    long pulseSeconds = request->getParam("pulseSeconds", "10").toInt();
    if (pulseSeconds < 1) pulseSeconds = 1;
    unsigned long pulseMs = (unsigned long)pulseSeconds * 1000UL;
    if (pulseMs > BRIDGE_WATCHDOG_PULSE_MAX_MS) pulseMs = BRIDGE_WATCHDOG_PULSE_MAX_MS;
    settings.pulseDurationMs = pulseMs;

    String banner;
    NetWatchdogSettings netSettings = loadNetWatchdogSettings();
    PowerMonitorSettings powerSettings = loadPowerMonitorSettings();
    if (settings.enabled && (settings.cameraA.length() == 0 || settings.cameraB.length() == 0)) {
      banner = "Both Camera A and Camera B must be selected. Not saved.";
    } else if (settings.enabled && settings.cameraA.equalsIgnoreCase(settings.cameraB)) {
      banner = "Camera A and Camera B must be two different cameras. Not saved.";
    } else if (settings.enabled && isReservedOrUnsafePin(settings.pin)) {
      banner = "Pin " + String(settings.pin) + " is reserved by another peripheral on this board or "
               "unsafe to use for general GPIO - pick a different one. Not saved.";
    } else if (watchdogPinsConflict(settings.enabled, settings.pin, netSettings.enabled, netSettings.pin)) {
      banner = "Pin " + String(settings.pin) + " is already used by the Internet Watchdog - pick a "
               "different one. Not saved.";
    } else if (watchdogPinsConflict(settings.enabled, settings.pin, powerSettings.enabled, powerSettings.pin)) {
      banner = "Pin " + String(settings.pin) + " is already used by the 220V Power Monitor - pick a "
               "different one. Not saved.";
    } else {
      banner = saveBridgeWatchdogSettings(settings)
          ? "Saved - the enable checkbox, cameras, and pin need a reboot to apply; the threshold and "
            "pulse duration are active immediately."
          : "Failed to save - NVS write error (see Serial log). Setting was NOT changed.";
    }
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareBridge, banner, renderBridgeWatchdogPage(g_liveCameras)).c_str());
  });

  server.on("/hardware/bridge/pulse", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    String banner = bridgeWatchdogManualPulse()
        ? "Relay pulsed."
        : "Camera Bridge Watchdog isn't active (disabled, or the configured pin wasn't accepted at boot) - nothing to pulse.";
    return response->send(200, "text/html",
                           renderShell(Tab::HardwareBridge, banner, renderBridgeWatchdogPage(g_liveCameras)).c_str());
  });

  server.on("/hardware/power", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html",
                           renderShell(Tab::HardwarePower, "", renderPowerMonitorPage()).c_str());
  });

  server.on("/hardware/power/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    PowerMonitorSettings settings;
    settings.enabled = request->hasParam("enabled");
    settings.activeHigh = request->hasParam("activeHigh");

    // Fallback matches POWER_MONITOR_PIN_DEFAULT.
    settings.pin = request->getParam("pin", "6").toInt();

    String banner;
    NetWatchdogSettings netSettings = loadNetWatchdogSettings();
    BridgeWatchdogSettings bridgeSettings = loadBridgeWatchdogSettings();
    if (settings.enabled && isReservedOrUnsafePin(settings.pin)) {
      banner = "Pin " + String(settings.pin) + " is reserved by another peripheral on this board or "
               "unsafe to use for general GPIO - pick a different one. Not saved.";
    } else if (watchdogPinsConflict(settings.enabled, settings.pin, netSettings.enabled, netSettings.pin)) {
      banner = "Pin " + String(settings.pin) + " is already used by the Internet Watchdog - pick a "
               "different one. Not saved.";
    } else if (watchdogPinsConflict(settings.enabled, settings.pin, bridgeSettings.enabled, bridgeSettings.pin)) {
      banner = "Pin " + String(settings.pin) + " is already used by the Camera Bridge Watchdog - pick "
               "a different one. Not saved.";
    } else {
      banner = savePowerMonitorSettings(settings)
          ? "Saved - the enable checkbox and pin need a reboot to apply; the wiring polarity is active "
            "immediately."
          : "Failed to save - NVS write error (see Serial log). Setting was NOT changed.";
    }
    return response->send(200, "text/html",
                           renderShell(Tab::HardwarePower, banner, renderPowerMonitorPage()).c_str());
  });

  server.on("/storage", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(200, "text/html", renderShell(Tab::Storage, "", renderStoragePanel()).c_str());
  });

  server.on("/capabilities", HTTP_GET, [](PsychicRequest* request, PsychicResponse* response) {
    return response->send(
        200, "text/html",
        renderShell(Tab::Capabilities, "", renderCapabilitiesPanel(g_liveCameras, g_liveStates)).c_str());
  });

  server.on("/storage/save", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    SdSettings settings;
    settings.enabled = request->hasParam("enabled");
    long intervalHours = request->getParam("checkIntervalHours", "0").toInt();
    if (intervalHours < 0) intervalHours = 0;
    if (intervalHours > (long)SD_CHECK_INTERVAL_MAX_HOURS) intervalHours = (long)SD_CHECK_INTERVAL_MAX_HOURS;
    settings.checkIntervalHours = (uint32_t)intervalHours;
    // Clamped here and again at use. Fallback matches
    // SD_RETENTION_DAYS_DEFAULT.
    long retentionDays = request->getParam("retentionDays", "30").toInt();
    if (retentionDays < 0) retentionDays = 0;
    if (retentionDays > (long)SD_RETENTION_MAX_DAYS) retentionDays = (long)SD_RETENTION_MAX_DAYS;
    settings.retentionDays = (uint16_t)retentionDays;
    String banner = saveSdSettings(settings)
        ? "Saved - the enable/disable setting needs a reboot to apply; the check interval and "
          "retention setting are active immediately."
        : "Failed to save - NVS write error (see Serial log). Setting was NOT changed.";
    return response->send(200, "text/html", renderShell(Tab::Storage, banner, renderStoragePanel()).c_str());
  });

  server.on("/storage/check", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    // Background task; returns immediately.
    String banner = backgroundJobBanner(
        startStorageCheckAsync(), "Checking storage in the background.",
        "A storage check is already running in the background - reload in a moment to see its result.",
        "Could not start the storage check - the device is low on memory right now. Try again in a moment.");
    return response->send(200, "text/html", renderShell(Tab::Storage, banner, renderStoragePanel()).c_str());
  });

  server.on("/storage/erase", HTTP_POST, [](PsychicRequest* request, PsychicResponse* response) {
    Serial.println("[Storage] Erase all snapshot history requested via dashboard.");
    // Background task; returns immediately.
    String banner = backgroundJobBanner(
        startEraseAllAsync(), "Erasing all snapshot history in the background.",
        "An erase is already running in the background - reload in a moment to see its result.",
        "Could not start the erase - the device is low on memory right now. Try again in a moment.");
    return response->send(200, "text/html", renderShell(Tab::Storage, banner, renderStoragePanel()).c_str());
  });

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
