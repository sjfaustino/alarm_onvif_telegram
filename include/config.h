#pragma once
#include <Arduino.h>
#include "secrets.h"

// WiFi, Telegram, and per-camera credentials live in secrets.h (gitignored -
// see secrets.h.example for the template). Nothing sensitive belongs in
// this file, since this one IS meant to be committed.

// ============================================================
// Timing (all in ms unless noted)
// ============================================================
static const unsigned long PULL_INTERVAL_MS         = 2000UL;        // per-camera poll cadence you asked for
static const unsigned long SUBSCRIPTION_LIFETIME_MS = 4UL * 60UL * 1000UL;
static const unsigned long RENEW_MARGIN_MS          = 60UL * 1000UL;
static const unsigned long RETRY_INTERVAL_MS        = 10000UL;
static const uint32_t      ALERT_COOLDOWN_MS        = 15000UL;       // per-camera cooldown
static const uint16_t      HTTP_TIMEOUT_MS          = 10000;
static const unsigned long HEARTBEAT_INTERVAL_MS    = 6UL * 60UL * 60UL * 1000UL; // liveness ping cadence
static const size_t        SNAPSHOT_MAX_BYTES       = 100000;        // internal-RAM buffer cap (no-PSRAM boards)
static const size_t        SNAPSHOT_MAX_BYTES_PSRAM = 2000000UL;     // PSRAM buffer cap - generous; real snapshots are far smaller
static const bool          VERBOSE_SOAP_LOG         = false;         // flip true to debug one camera at a time

// NOTE ON HARDWARE: this project has now been flashed to three different
// boards over the course of development - a no-PSRAM ESP32-S2, a no-PSRAM
// dual-core classic ESP32 (ESP32-D0WDQ6), and a dual-core ESP32-S3 with 8MB
// embedded PSRAM - each confirmed via `esptool.exe chip-id`. Rather than
// hardcode assumptions for whichever board happened to be connected at the
// time, the two things that actually varied across them are now handled at
// RUNTIME instead of via comments/config here:
//   - PSRAM: telegram.cpp checks ESP.getPsramSize() at the point of each
//     Telegram send and picks buffered-in-PSRAM vs. streamed-via-internal-
//     RAM accordingly (see telegram.cpp's top comment for the full
//     rationale). SNAPSHOT_MAX_BYTES_PSRAM above only applies on the
//     PSRAM path; SNAPSHOT_MAX_BYTES only applies on the non-PSRAM path.
//   - Core count: camera.cpp's per-camera FreeRTOS tasks are pinned to
//     core 1 in main.cpp. This requires a genuine second core - fine for
//     all three boards above (all dual-core), but would need changing to
//     plain xTaskCreate (no pinning) if this is ever flashed to a
//     single-core chip (e.g. an ESP32-C3, which came up mid-conversation
//     as a possible board before being ruled out - see platformio.ini for
//     the per-board build environments this project now keeps side by side).
// mbedTLS's ~16KB RX + 16KB TX TLS session buffers are fixed at build time
// on every variant - that part of the original fragmentation analysis
// holds regardless of which ESP32 this is.
static const size_t        STREAM_CHUNK_BYTES       = 2048;

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
// on CreatePullPointSubscription. Worth trying both ON first as the
// spec-correct default for a standards-compliant stack - but the Vatilon,
// despite being explicitly ONVIF 2.40, turned out to need both OFF too
// (NotAuthorized on CreatePullPointSubscription only, with every other
// Events-service call succeeding). So: not exclusively a Xiongmai quirk -
// try ON first on a new camera, flip to OFF if this exact symptom shows up.
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
//
// enabled: skip this camera entirely (no setup calls, no polling, no retry
// spam in the log) without deleting or commenting out its config. Flip back
// to true once it's reachable again.
// ============================================================
struct CameraConfig {
  const char* name;
  const char* deviceServiceUrl;
  bool        enabled;
  bool        useWSSecurity;
  bool        includeInitialTerminationTime;
  bool        includeReplyToAnonymous;
  const char* snapshotUriOverride;
  const char* preferredProfileKeyword;
};

// Credentials are NOT listed here anymore - they're resolved at task startup
// by matching `name` (exactly, case-sensitive) against CAMERA_SECRETS in
// secrets.h. See camera.h's resolveCameraCredentials() and secrets.h.example
// for the credential-side format. This means the `name` string below is now
// load-bearing (it's the join key to secrets.h), not just a display label -
// if you rename a camera here, rename its CAMERA_SECRETS entry to match.
static const CameraConfig CAMERAS[] = {
  {
    "2Lentes",                                            // XM530 - proven quirks
    "http://192.168.1.178:8899/onvif/device_service",
    false,                   // temporarily disconnected - flip back on once it's reachable
    true,                    // WS-Security digest is how this camera authenticates
    false, false,            // both OFF - this is what fixed ResourceUnknownFault
    "http://192.168.1.178/webcapture.jpg?command=snap&channel=0",
    nullptr
  },
  {
    "D01-FDir",
    "http://192.168.8.224/onvif/device_service",
    true,                    // enabled
    true,                    // standards-compliant default - GetProfiles/Capabilities/Properties all work with this
    false, false,            // CreatePullPointSubscription-only NotAuthorized -> same fix as the XM530
    nullptr,                // no known-broken snapshot URI yet -> try standard GetSnapshotUri first
    "sub"                   // substream: smaller JPEG -> less heap pressure for the Telegram TLS handshake
  },
  {
    "D02-FEsq",
    "http://192.168.8.231/onvif/device_service",
    false,                    // enabled
    true,                    // standards-compliant default - GetProfiles/Capabilities/Properties all work with this
    false, false,            // CreatePullPointSubscription-only NotAuthorized -> same fix as the XM530
    nullptr,                // no known-broken snapshot URI yet -> try standard GetSnapshotUri first
    "sub"                   // substream: smaller JPEG -> less heap pressure for the Telegram TLS handshake
  },
  {
    "D03-Escritorio",
    "http://192.168.8.195/onvif/device_service",
    true,                    // enabled
    true,                    // standards-compliant default - GetProfiles/Capabilities/Properties all work with this
    false, false,            // CreatePullPointSubscription-only NotAuthorized -> same fix as the XM530
    nullptr,                // no known-broken snapshot URI yet -> try standard GetSnapshotUri first
    "sub"                   // substream: smaller JPEG -> less heap pressure for the Telegram TLS handshake
  },
  {
    "D04-Pavilhao",
    "http://192.168.8.199/onvif/device_service",
    true,                    // enabled
    true,                    // standards-compliant default - GetProfiles/Capabilities/Properties all work with this
    false, false,            // CreatePullPointSubscription-only NotAuthorized -> same fix as the XM530
    nullptr,                // no known-broken snapshot URI yet -> try standard GetSnapshotUri first
    "sub"                   // substream: smaller JPEG -> less heap pressure for the Telegram TLS handshake
  },
  {
    "D05-Traseiras",
    "http://192.168.8.148/onvif/device_service",
    false,                    // enabled
    true,                    // standards-compliant default - GetProfiles/Capabilities/Properties all work with this
    false, false,            // CreatePullPointSubscription-only NotAuthorized -> same fix as the XM530
    nullptr,                // no known-broken snapshot URI yet -> try standard GetSnapshotUri first
    "sub"                   // substream: smaller JPEG -> less heap pressure for the Telegram TLS handshake
  },
  {
    "D06-Centro",
    "http://192.168.8.234/onvif/device_service",
    true,                    // enabled
    true,                    // standards-compliant default - GetProfiles/Capabilities/Properties all work with this
    false, false,            // CreatePullPointSubscription-only NotAuthorized -> same fix as the XM530
    nullptr,                // no known-broken snapshot URI yet -> try standard GetSnapshotUri first
    "sub"                   // substream: smaller JPEG -> less heap pressure for the Telegram TLS handshake
  },
  {
    "D08-PortariaDir",
    "http://192.168.8.222/onvif/device_service",
    true,                    // enabled
    true,                    // standards-compliant default - GetProfiles/Capabilities/Properties all work with this
    false, false,            // CreatePullPointSubscription-only NotAuthorized -> same fix as the XM530
    nullptr,                // no known-broken snapshot URI yet -> try standard GetSnapshotUri first
    "sub"                   // substream: smaller JPEG -> less heap pressure for the Telegram TLS handshake
  },
  {
    "D09-Portao",
    "http://192.168.8.223/onvif/device_service",
    true,                    // enabled
    true,                    // standards-compliant default - GetProfiles/Capabilities/Properties all work with this
    false, false,            // CreatePullPointSubscription-only NotAuthorized -> same fix as the XM530
    nullptr,                // no known-broken snapshot URI yet -> try standard GetSnapshotUri first
    "sub"                   // substream: smaller JPEG -> less heap pressure for the Telegram TLS handshake
  },
};

static const size_t NUM_CAMERAS = sizeof(CAMERAS) / sizeof(CAMERAS[0]);
