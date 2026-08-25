#pragma once
#include <Arduino.h>

// ============================================================
// WiFi
// ============================================================
// static const char* WIFI_SSID     = "UZO-650EE0";
// static const char* WIFI_PASSWORD = "94bd7194c3";
static const char* WIFI_SSID     = "Fabrica";
static const char* WIFI_PASSWORD = "Goodlife";

// ============================================================
// Telegram
// ============================================================
static const char* TELEGRAM_BOT_TOKEN = "7795326342:AAFHTkr5no24dQtwWk0WeTAUh5xS1ekbHbk";
static const char* TELEGRAM_CHAT_ID   = "8897455184";

// ============================================================
// Timing (all in ms unless noted)
// ============================================================
static const unsigned long PULL_INTERVAL_MS         = 2000UL;        // per-camera poll cadence you asked for
static const unsigned long SUBSCRIPTION_LIFETIME_MS = 4UL * 60UL * 1000UL;
static const unsigned long RENEW_MARGIN_MS          = 60UL * 1000UL;
static const unsigned long RETRY_INTERVAL_MS        = 10000UL;
static const uint32_t      ALERT_COOLDOWN_MS        = 15000UL;       // per-camera cooldown
static const uint16_t      HTTP_TIMEOUT_MS          = 10000;
static const size_t        SNAPSHOT_MAX_BYTES       = 100000;        // no-PSRAM assumption, same as original
static const bool          VERBOSE_SOAP_LOG         = false;         // flip true to debug one camera at a time

// ============================================================
// Per-camera definition
//
// useWSSecurity: most ONVIF stacks require a WS-Security digest header on
// every request (what makeSecurityHeader() builds). Some cheaper/older
// stacks instead expect plain HTTP Basic Auth and choke on - or just
// ignore - the WSSE header. Set true for the standard case; if a camera's
// requests all come back with an auth fault, try false (falls back to
// HTTP Basic Auth via soapPost's optional credentials).
//
// includeInitialTerminationTime / includeReplyToAnonymous: workaround
// toggles from the XM530's original ResourceUnknownFault debugging.
// Xiongmai-derived stacks (XM530 etc.) needed both OFF to avoid faulting
// on CreatePullPointSubscription. A standards-compliant stack should be
// fine with both ON (the spec-correct way of doing it) - that's the
// default we're giving the Vatilon, since it's explicitly ONVIF 2.40
// versioned. If it faults on first boot, that's the first thing to flip.
//
// snapshotUriOverride: set this ONLY if, like the XM530, the camera's real
// ONVIF GetSnapshotUri response is broken/wrong and you've found a working
// direct snapshot URL by hand. Leave nullptr to use the standard ONVIF flow
// (GetProfiles -> GetSnapshotUri), which is what you should try first for
// the Vatilon.
//
// preferredProfileKeyword: when a camera exposes multiple profiles (e.g.
// "mainStream" / "subStream" on the Vatilon), this is matched
// case-insensitively against each profile's <Name> to pick which one to
// snapshot from. nullptr = just use whichever profile comes back first.
// ============================================================
struct CameraConfig {
  const char* name;
  const char* deviceServiceUrl;
  const char* user;
  const char* pass;
  bool        useWSSecurity;
  bool        includeInitialTerminationTime;
  bool        includeReplyToAnonymous;
  const char* snapshotUriOverride;
  const char* preferredProfileKeyword;
};

static const CameraConfig CAMERAS[] = {
  {
    "Front Gate Cam",                                            // XM530 - proven quirks
    "http://192.168.1.178:8899/onvif/device_service",
    "bufp", "4uatan",
    true,                    // WS-Security digest is how this camera authenticates
    false, false,            // both OFF - this is what fixed ResourceUnknownFault
    "http://192.168.1.178/webcapture.jpg?command=snap&channel=0",
    nullptr
  },
  {
    "Vatilon H80",
    "http://192.168.8.148/onvif/device_service",
    "admin", "CHANGE_ME",   // TODO: put the camera's real ONVIF username/password in here
    true,                   // standards-compliant default
    true, true,              // standards-compliant default; drop to false,false if it faults
    nullptr,                // no known-broken snapshot URI yet -> try standard GetSnapshotUri first
    "main"                  // prefer the "mainstream" profile for full-res alert photos
  },
};

static const size_t NUM_CAMERAS = sizeof(CAMERAS) / sizeof(CAMERAS[0]);
