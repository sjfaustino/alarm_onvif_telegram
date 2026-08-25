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
static const size_t        SNAPSHOT_MAX_BYTES       = 100000;        // no-PSRAM assumption, same as original
static const bool          VERBOSE_SOAP_LOG         = false;         // flip true to debug one camera at a time

// NOTE: arduino-esp32's WiFiClientSecure has no setBufferSizes() - that's a
// BearSSL (ESP8266/Arduino-Pico) API. On this chip mbedTLS's ~16KB RX + 16KB
// TX session buffers are fixed at build time in the precompiled core; they
// can't be shrunk at runtime for framework = arduino. That fixed cost is
// exactly what was failing at 54KB nominally-free-but-fragmented heap.
// The lever that IS available: never hold the whole JPEG in RAM at the same
// time as those TLS buffers. telegram.cpp now streams the photo straight
// from the camera's HTTP connection into Telegram's TLS connection
// STREAM_CHUNK_BYTES at a time, so only one small chunk (not the full
// 100KB snapshot) competes with the TLS buffers for heap.
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
  const char* user;
  const char* pass;
  bool        enabled;
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
    CAM0_USER, CAM0_PASS,
    false,                   // temporarily disconnected - flip back on once it's reachable
    true,                    // WS-Security digest is how this camera authenticates
    false, false,            // both OFF - this is what fixed ResourceUnknownFault
    "http://192.168.1.178/webcapture.jpg?command=snap&channel=0",
    nullptr
  },
  {
    "Vatilon H80",
    "http://192.168.8.148/onvif/device_service",
    CAM1_USER, CAM1_PASS,
    true,                    // enabled
    true,                    // standards-compliant default - GetProfiles/Capabilities/Properties all work with this
    false, false,            // CreatePullPointSubscription-only NotAuthorized -> same fix as the XM530
    nullptr,                // no known-broken snapshot URI yet -> try standard GetSnapshotUri first
    "sub"                   // substream: smaller JPEG -> less heap pressure for the Telegram TLS handshake
  },
};

static const size_t NUM_CAMERAS = sizeof(CAMERAS) / sizeof(CAMERAS[0]);
