#pragma once
#include <Arduino.h>
#include "telegram_users.h" // TelegramLang

// Full-text translation catalog for every message this project sends TO
// Telegram - alerts and command replies alike. Each function takes the
// recipient's TelegramLang plus whatever dynamic values that message
// embeds, and returns the fully composed String (same inline-concatenation
// style this project already uses, just duplicated per language inside
// each function) - a two-language, closed set of well under 100 known
// messages doesn't need a generic template/catalog-file i18n framework.
//
// Deliberately NOT used for logEvent()'s Activity Log text: that text is
// shown verbatim on the (English-only) web dashboard's Activity page, so
// it stays English everywhere it appears, including when replayed by the
// /log command - only /log's own header/empty-state text and the
// elapsed-time prefix in front of each entry are translated (trLogHeader,
// trLogEmpty, trElapsedSince below), never the stored entry text itself.
//
// Also not used for describeResetReason() (main.cpp) - kept out of this
// (natively-tested) lib deliberately, same reasoning as that function's
// own comment: esp_reset_reason_t is an ESP-IDF type unavailable under the
// native test environment. main.cpp has its own small
// describeResetReasonLocalized() sitting next to the English original.

// ---- Camera alerts (broadcast to each subscribed recipient, one call per recipient) ----

// Motion/pet-detected photo caption. isPetEvent picks the paw-print
// wording; the "(i/N)" burst suffix (numbers/slash only) is appended by
// the caller, not part of this - it's language-neutral.
String trMotionCaption(TelegramLang lang, const String& cameraName, const String& timestamp, bool isPetEvent);
// Pet alert, text-only delivery mode (CameraConfig::petAlertsTextOnly) - no photo.
String trPetAlertText(TelegramLang lang, const String& cameraName, const String& timestamp);
String trTimelapseCaption(TelegramLang lang, const String& cameraName, const String& timestamp);
String trTamperCaption(TelegramLang lang, const String& cameraName, const String& timestamp);
String trSignalLossMessage(TelegramLang lang, const String& cameraName, const String& timestamp);
String trMotionDigest(TelegramLang lang, const String& cameraName, uint32_t count, unsigned long elapsedSec);

// ---- System-message broadcasts (sendTelegramMessage's systemMessages recipients) ----

String trCameraOffline(TelegramLang lang, const String& cameraName, unsigned long minutes);
String trCameraBackOnline(TelegramLang lang, const String& cameraName);
String trSubscriptionLost(TelegramLang lang, const String& cameraName, unsigned long minutes);
String trMotionWatchdogTripped(TelegramLang lang, const String& cameraName, unsigned hours);
String trNvsUsageWarning(TelegramLang lang, unsigned pct);
String trWifiWeakWarning(TelegramLang lang, int rssi);
String trHeapLowWarning(TelegramLang lang, uint32_t baselineBytes, uint32_t maxAllocBytes);
String trCameraTaskSpawnFailure(TelegramLang lang, const String& cameraName);
String trInternetOutageAlert(TelegramLang lang);
String trBridgeOutageAlert(TelegramLang lang);
String trSdFailure(TelegramLang lang, const String& reason);
String trSdCheckWarning(TelegramLang lang, size_t unreadableFiles, size_t filesChecked);
// afterLiveEdit: false = discovered at camera-task startup, true = discovered
// after a live dashboard edit removed the credentials (camera.cpp's two sites).
String trMissingCredentials(TelegramLang lang, const String& cameraName, bool afterLiveEdit);
String trTestMessage(TelegramLang lang);

// ---- Heartbeat / boot message building blocks ----

String trHeartbeatHeader(TelegramLang lang, const String& firmwareVersion);
// "Uptime: ..." - shared by the heartbeat, /uptime, and /health.
String trUptimeLine(TelegramLang lang, unsigned long ms);
String trFreeHeapLine(TelegramLang lang, uint32_t freeBytes, uint32_t minEverBytes);
String trNvsUsageLine(TelegramLang lang, unsigned pct);
String trWifiSignalLine(TelegramLang lang, int rssi);
// One fully-composed, trailing-newline-free heartbeat line for one camera -
// mirrors main.cpp's current inline subscribed/OFFLINE/alerts-note assembly.
String trHeartbeatCameraLine(TelegramLang lang, const String& cameraName, bool subscribed, bool offline,
                              bool alertsEnabled, bool revertPending, bool revertToOn, const String& untilTime);
String trBootHeader(TelegramLang lang, const String& firmwareVersion);
String trRebootReasonLine(TelegramLang lang, const String& reasonText);
String trEnabledCamerasLine(TelegramLang lang, size_t enabledCount, size_t total);
String trConfiguredCamerasHeader(TelegramLang lang);
String trSdBootCheckWarning(TelegramLang lang, size_t unreadableFiles, size_t directoriesChecked);

// ---- Command replies (sender already known at composition time) ----

String trNotAuthorized(TelegramLang lang, const String& commandName);
String trRateLimited(TelegramLang lang);
String trStatusHeader(TelegramLang lang);
// Composes the whole "{name}: ON/OFF[ - OFFLINE][ (auto ... in ...)][ ~Nms]"
// line; timerSuffix is pre-built via trTimerSuffix (""=none), latencyMs<0
// means "no latency data yet".
String trStatusCameraLine(TelegramLang lang, const String& cameraName, bool alertsEnabled, bool offline,
                            const String& timerSuffix, long avgLatencyMs);
String trRebootingNow(TelegramLang lang);
String trHelpText(TelegramLang lang, uint16_t eventLogCapacity, uint16_t maxDurationMinutes,
                    bool canCommand, bool canSnap, bool canReset);
String trHealthHeader(TelegramLang lang);
String trFreePsramLine(TelegramLang lang, uint32_t freeBytes);
// "SD storage: {sdDetailText}" - sdDetailText is one of the three below, pre-localized by the caller.
String trSdStorageLine(TelegramLang lang, const String& sdDetailText);
String trSdDisabledDetail(TelegramLang lang);
String trSdNotDetectedDetail(TelegramLang lang);
String trSdDetail(TelegramLang lang, const String& cardTypeName, double usedMB, double totalMB);
String trLogHeader(TelegramLang lang);
String trLogEmpty(TelegramLang lang);
// Same "just now" / "Xh Ym ago" shape as format_utils.h's formatElapsedSince
// (which stays English-only for the web dashboard) - a separate function
// here rather than adding a language parameter to that shared, web-facing one.
String trElapsedSince(TelegramLang lang, unsigned long eventMs, unsigned long nowMs);
String trAmbiguousCamera(TelegramLang lang, const String& name, const String& matchList);
String trUnknownCamera(TelegramLang lang, const String& name);
String trNoCamerasToChoose(TelegramLang lang);
String trCameraPickerPrompt(TelegramLang lang, const String& commandDisplayName);
String trCallbackDataTooLong(TelegramLang lang, size_t skipped, const String& commandDisplayName);
String trCallbackUnrecognized(TelegramLang lang);
String trCallbackNotAuthorized(TelegramLang lang);
String trCallbackCameraGone(TelegramLang lang);
String trCameraNoLongerAvailable(TelegramLang lang, const String& target);
String trNoSnapshotUriYet(TelegramLang lang, const String& cameraName);
String trSnapshotFetchFailed(TelegramLang lang, const String& cameraName);
String trDurationParseError(TelegramLang lang, const String& durationText, uint16_t maxMinutes);
// " (auto ON/OFF in ...)" suffix - shared by resolveAlertTimer and /status's
// own independent (but identically-worded) pending-timer display.
String trTimerSuffix(TelegramLang lang, bool turnOn, unsigned long durationMs);
// "{subject} alerts: ON/OFF{suffix}" - subject is a camera name or "All N
// camera(s)"; shared by applyOnOffToCamera, setAllCamerasAlertState, and
// checkScheduledAlertReverts.
String trAlertsState(TelegramLang lang, const String& subject, bool turnOn, const String& suffix);
// " (timer expired)" - the suffix checkScheduledAlertReverts appends when
// an /on|/off timer's automatic revert fires (distinct from trTimerSuffix,
// which describes a timer still PENDING, not one that just fired).
String trTimerExpiredSuffix(TelegramLang lang);
// "All N camera(s)" - the `subject` passed to trAlertsState for the /on
// all, /off all, and dashboard Mute-all/Unmute-all paths.
String trAllCamerasSubject(TelegramLang lang, size_t count);
String trNoEnabledCameras(TelegramLang lang);
