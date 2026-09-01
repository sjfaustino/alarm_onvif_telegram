#pragma once
#include <Arduino.h>
#include "secrets.h"

// WiFi and Telegram credentials live in secrets.h (gitignored). Nothing
// sensitive belongs in this file, since this one IS meant to be committed.
//
// Cameras are managed at runtime - see camera_store.h (CameraConfig, NVS
// load/save) and webserver.h (the web UI). secrets.h's CAMERA_SEED only
// seeds NVS on first boot; after that the web UI is the only way to
// change them.

// FIRMWARE_VERSION lives in build_version.h, not here, even though this
// is where every other project-wide constant lives - see that header's
// own comment for why (short version: it changes on every build, and
// config.h is included by nearly everything in src/, so baking it in
// here would force a full rebuild on every single `pio run`/upload).

// ============================================================
// Timing (all in ms unless noted)
// ============================================================
// Default for CameraConfig::pollIntervalMs (Cameras page) - the per-camera
// poll cadence, editable per camera since some cameras' embedded HTTP
// stacks tolerate more frequent polling than others. This value is only
// CameraConfig's own default member initializer now (an existing camera
// loaded from a pre-pollIntervalMs schema record - see camera_serialize.h's
// CAMERA_SCHEMA_VERSION comment - gets this too, so nothing changes for it
// without an explicit edit); camera.cpp's actual poll-due check reads
// cfg.pollIntervalMs (clamped - see safePollIntervalMs, camera.cpp), never
// this constant directly.
static const unsigned long PULL_INTERVAL_MS         = 2000UL;
static const unsigned long SUBSCRIPTION_LIFETIME_MS = 4UL * 60UL * 1000UL;
static const unsigned long RENEW_MARGIN_MS          = 60UL * 1000UL;
static const unsigned long RETRY_INTERVAL_MS        = 10000UL;
// See CameraState::pullAmbiguousStreak's own comment - how many
// consecutive genuinely-unrecognized PullMessages responses in a row
// force a resubscribe. At the default 2s poll cadence, 5 is ~10s of
// tolerance for a transient hiccup before treating the pullpoint as dead -
// proportionally longer in real time for a camera configured with a
// longer pollIntervalMs, which is an acceptable tradeoff for whoever
// deliberately chose that.
static const uint8_t        PULL_MESSAGES_AMBIGUOUS_LIMIT = 5;
// Clamp for CameraConfig::pollIntervalMs - webserver_cameras.cpp's
// parseCameraForm clamps user input to this range; camera.cpp's
// safePollIntervalMs re-clamps at the actual point of use, same
// "hand-edited/imported NVS blob bypasses the form entirely" reasoning as
// this project's other per-camera clamps (CAMERA_ALERT_COOLDOWN_MAX_MS
// etc., above). The floor matters here specifically: every SOAP call in
// this project sends "Connection: close" (onvif_soap.cpp's soapPost), so
// an interval near 0 wouldn't just poll aggressively, it would open and
// tear down a fresh TCP connection to the camera in a tight loop - real
// hammering, not just "frequent," on a device whose embedded HTTP stack
// may only tolerate 1-2 connections at all (see camera_tasks.h's own
// staggered-boot comment for a real incident from exactly that class of
// overload).
static const unsigned long CAMERA_POLL_INTERVAL_MIN_MS = 250UL;
static const unsigned long CAMERA_POLL_INTERVAL_MAX_MS = 30000UL; // 30s
// See CameraState::lastSnapshotUriRetryMs's own comment - how often a
// subscribed camera with a still-unresolved snapshotUri retries
// GetProfiles/GetSnapshotUri. Not too aggressive: a camera whose Media
// service is genuinely absent (not just a transient failure) will keep
// failing this cheaply but pointlessly forever, so this stays well above
// the poll cadence.
static const unsigned long SNAPSHOT_URI_RETRY_INTERVAL_MS = 5UL * 60UL * 1000UL; // 5 minutes
static const uint16_t      HTTP_TIMEOUT_MS          = 10000;
static const unsigned long HEARTBEAT_INTERVAL_MS    = 6UL * 60UL * 60UL * 1000UL; // liveness ping cadence
static const unsigned long TELEGRAM_COMMAND_POLL_MS = 5000UL;        // /on, /off, /status polling cadence
// How long a task waits to acquire telegram.cpp's g_telegramNetMutex
// before treating a Telegram send/poll as failed rather than blocking
// indefinitely - see that mutex's own comment for the real incident this
// fixes (concurrent TLS sessions across cameras exhausting internal RAM
// during a multi-camera motion burst). Sized to comfortably outlast one
// queued-behind predecessor's worst-case single send (~35-40s - see
// TelegramNetLock's own comment) while staying well under RENEW_MARGIN_MS
// below: a camera task blocked here is also blocked from servicing its
// own ONVIF subscription renewal.
static const unsigned long TELEGRAM_NET_MUTEX_TIMEOUT_MS = 45000UL;
// How often main.cpp's loop() re-checks NVS usage (checkNvsUsage) - see
// NVS_USAGE_WARN_PERCENT's own comment for why this exists at all.
// Independent of HEARTBEAT_INTERVAL_MS's much longer cadence: NVS usage
// only grows from deliberate dashboard edits (adding cameras/users), never
// silently on its own between checks, so there's no harm in checking more
// often than the heartbeat - this just gets the warning out sooner than
// waiting for the next 6-hourly heartbeat to happen to mention it.
static const unsigned long NVS_USAGE_CHECK_INTERVAL_MS = 60UL * 60UL * 1000UL; // 1 hour
// NVS is entry-based (fixed ~32-byte slots), not a raw byte pool - this
// project has hit a real incident before where camera records silently
// failed to persist once NVS filled up (see camera_store.cpp's
// NVS_KEY_LIST_LEGACY comment). Shared by the Firmware page's own hint
// (webserver_firmware.cpp) and checkNvsUsage's proactive Telegram alert
// (main.cpp) so both agree on what "getting full" means.
static const unsigned NVS_USAGE_WARN_PERCENT = 80;
// How often main.cpp's loop() re-checks WiFi.RSSI() (checkWifiSignal) -
// shorter than NVS_USAGE_CHECK_INTERVAL_MS since signal strength can
// genuinely drift within minutes (something moved, a neighbor's channel
// got busier), unlike NVS usage which only ever changes from a deliberate
// dashboard edit.
static const unsigned long WIFI_RSSI_CHECK_INTERVAL_MS = 15UL * 60UL * 1000UL; // 15 minutes
// RSSI (dBm, always negative - closer to 0 is stronger) at or below which
// checkWifiSignal() proactively alerts. -75dBm is a common "reliable but
// starting to struggle" line for 2.4GHz WiFi - well before typical
// disconnect territory (usually past -85 to -90dBm), so this is meant to
// catch a board drifting toward real connectivity trouble while there's
// still time to do something about it (move the board/AP, reconsider
// channel/placement), not just note that it already happened.
static const int WIFI_RSSI_WARN_DBM = -75;
// Free-heap threshold (bytes, ESP.getMinFreeHeap() - internal SRAM, not
// PSRAM) below which checkHeapHealth() (main.cpp) sends a one-time Telegram
// alert the first time a new lifetime-low record crosses it. mbedTLS/
// WiFiClientSecure allocate from this same pool (see g_telegramNetMutex's
// own comment, telegram.cpp), so a genuinely low reading here is real
// allocation-failure territory, not a sanity nicety - 20KB is comfortably
// above a single TLS handshake's typical needs, so crossing it is an early
// warning, not already-crashed. No re-arm: this is a running minimum
// (never increases within a boot), so "already alerted" only ever resets
// on the next reboot.
static const uint32_t HEAP_LOW_WARN_BYTES = 20000;
// How often main.cpp's loop() re-checks ESP.getMinFreeHeap() - cheap (a
// stored-value read, no computation), checked every loop() tick rather
// than gated behind an interval like the checks above: this is a running
// watermark the IDF already tracks continuously, so checking often costs
// nothing and only improves how closely the resulting Activity log
// timestamp lines up with whatever actually caused the drop.
static const size_t        SNAPSHOT_MAX_BYTES       = 100000;        // internal-RAM fallback cap - see note below
static const size_t        SNAPSHOT_MAX_BYTES_PSRAM = 2000000UL;     // PSRAM buffer cap - generous; real snapshots are far smaller
static const bool          VERBOSE_SOAP_LOG         = false;         // flip true to debug one camera at a time
// Hides the routine "[camera] [SOAP] Action HTTP=200 len=N" line for a
// call that succeeded and wasn't a SOAP fault - most of the serial
// output during steady-state polling. Faults and non-200/negative codes
// always print regardless, so nothing actionable gets hidden. Flip to
// false to see every call again (e.g. while troubleshooting timing).
static const bool          SUPPRESS_SOAP_SUCCESS_LOG = true;

// PSRAM is a hard requirement - setup() refuses to start if
// ESP.getPsramSize() is 0, since a snapshot alert going to multiple
// Telegram users needs the JPEG buffered once in RAM and resent per
// recipient. SNAPSHOT_MAX_BYTES is only telegram.cpp's fallback cap for
// the rare case a PSRAM allocation itself fails (fragmentation).
//
// camera.cpp's per-camera FreeRTOS tasks are pinned to core 1, requiring a
// genuine second core - would need plain xTaskCreate if ever flashed to a
// single-core chip.

// ============================================================
// Optional SD card storage (sd_store.h/.cpp) - a generic SPI microSD
// breakout module, entirely optional and off by default (see
// SdSettings::enabled, dashboard Storage page). When enabled AND a card
// is actually detected at boot, snapshot history (webserver.cpp's
// /cameras/snapshot, the Cameras page's preview strip) is stored here
// instead of the small PSRAM ring, with far more history retained and
// persisting across reboots. If disabled, or enabled but no module/card
// responds at boot, monitoring is entirely unaffected - snapshot history
// just falls back to the existing PSRAM ring, exactly as it already
// works today.
//
// *** VERIFY AND ADJUST these for your actual wiring before flashing ***
// These are common ESP32-S3 default SPI2 pins, not guaranteed for your
// specific dev board - check your board's pinout/datasheet. Nothing else
// in this project uses SPI, so any four free GPIOs work; these are just a
// reasonable starting point.
static const int SD_CS_PIN   = 10;
static const int SD_SCK_PIN  = 12;
static const int SD_MISO_PIN = 13;
static const int SD_MOSI_PIN = 11;

// Safety margin kept free on the card at all times - a write that would
// drop free space below this triggers pruning first (see sd_store.cpp's
// writeSnapshot). Not precisely tuned to any card size on purpose: large
// enough to comfortably fit several more snapshots plus filesystem
// overhead, small enough not to waste meaningful capacity on any card
// worth using for this.
static const uint64_t SD_FREE_SPACE_RESERVE_BYTES = 50UL * 1024UL * 1024UL; // 50MB

// Per-camera fairness ceiling, independent of the free-space reserve
// above - without this, one chatty camera could fill most of the card
// and crowd out a quiet camera's retained history, since pruning is
// deliberately per-camera (see sd_store.cpp) rather than a global,
// cross-directory walk.
static const size_t SD_MAX_FILES_PER_CAMERA = 300;

// Caps how many files a single write's prune pass deletes, even if that
// isn't enough to clear the reserve/ceiling above - bounds how long one
// write can hold the SD mutex (blocking every other camera's own writes)
// during pruning. If one call's cap isn't enough, the next write's own
// prune pass continues the job.
static const size_t SD_PRUNE_MAX_FILES_PER_WRITE = 5;

// How long waitForSdIdle() (sd_store.h) waits for an in-flight SD
// operation to finish before a deliberate reboot proceeds anyway.
// Generous enough to cover a legitimately slow erase-all pass across
// several cameras' worth of files on a slow card, short enough that a
// genuinely wedged SD operation doesn't leave someone who pressed
// "reboot" stuck waiting indefinitely.
static const unsigned long SD_IDLE_WAIT_TIMEOUT_MS = 10000UL;

// Clamp for the dashboard's "automatic full storage check" interval
// (SdSettings::checkIntervalHours, Storage page) - just a sanity bound on
// the number field, same idea as CameraConfig's snapshotBurstCount clamp.
// 720h = 30 days.
static const uint32_t SD_CHECK_INTERVAL_MAX_HOURS = 720;

// Clamp for the dashboard's NTP resync interval (WifiCredentials::
// ntpSyncIntervalMs, Network page) - webserver_network.cpp's
// handleSaveNetwork clamps user input to this, and main.cpp's setupTime
// re-clamps at the actual esp_sntp_set_sync_interval() call (see its own
// comment for why a form-only clamp isn't enough - same "hand-edited/
// imported NVS blob bypasses the form entirely" reasoning as
// SD_CHECK_INTERVAL_MAX_HOURS/telegram.cpp's motionWatchdogHours clamp).
// 43200min = 30 days.
static const unsigned long NTP_SYNC_MAX_MINUTES = 43200UL;

// Clamps for CameraConfig's three alert-throttling fields (Cameras page) -
// same "hand-edited/imported NVS blob bypasses the form entirely" reasoning
// as the constants above. webserver_cameras.cpp's parseCameraForm clamps
// user input to these; telegram.cpp re-clamps at each point of use (see
// its own comments - an unclamped alertCooldownMs/snapshotBurstCount pair
// is exactly the multi-camera Telegram burst class this project has
// already been burned by once, see git history around "Serialize Telegram
// TLS sends to fix multi-camera burst SSL failures").
static const unsigned long CAMERA_ALERT_COOLDOWN_MAX_MS = 86400000UL;    // 24h
static const unsigned long CAMERA_OFFLINE_THRESHOLD_MAX_MS = 604800000UL; // 7 days
static const unsigned int CAMERA_SNAPSHOT_BURST_MAX = 10;

// Cap on /activity.log (sd_store.cpp's appendActivityLogLine) - the SD-
// persisted mirror of the in-memory Activity log (event_log_store.h).
// Once a line's append would push the file past this, it's deleted and
// the next append starts fresh - bounded, lossy-by-design, same pragmatic
// "just wipe and restart" precedent eraseAllSnapshots() already set. Kept
// small enough that reading the whole file back in one shot (the
// dashboard's download route) stays a simple one-off String, no
// streaming needed.
static const size_t ACTIVITY_LOG_MAX_BYTES = 65536; // 64KB

// Caps how many thumbnails the Gallery page (webserver_gallery.cpp) shows
// per camera in one page load - each one independently re-triggers a full
// SD directory listing (see that file's own comment on the accepted
// cost), so this also bounds how many times that happens per load.
static const size_t GALLERY_PAGE_SIZE = 30;
