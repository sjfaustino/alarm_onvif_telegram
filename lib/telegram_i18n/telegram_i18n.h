#pragma once
#include <Arduino.h>
#include "telegram_users.h" // TelegramLang

// Every message sent to Telegram, in each supported language. Each function
// takes the recipient's language plus the dynamic values and returns the
// composed text; for two languages and <100 messages, a template framework
// isn't worth it.
//
// Activity log text stays English (the dashboard shows it verbatim), and
// reset-reason text lives in boot_checks.cpp (ESP-IDF types aren't available
// to native tests).

// ---- Camera alerts (broadcast to each subscribed recipient, one call per recipient) ----

// Which detection fired. Person wins over Vehicle when both arrive together;
// Generic covers plain motion.
enum class MotionDetectionKind { Generic, Person, Vehicle };

// Photo caption. isPetEvent picks pet wording (kind ignored); otherwise kind
// adds a per-type emoji/keyword that phone automations can match. The caller
// appends the "(i/N)" burst suffix.
String trMotionCaption(TelegramLang lang, const String& cameraName, const String& timestamp, bool isPetEvent,
                        MotionDetectionKind kind = MotionDetectionKind::Generic);
// Pet alert, text-only delivery mode (CameraConfig::petAlertsTextOnly) - no photo.
String trPetAlertText(TelegramLang lang, const String& cameraName, const String& timestamp);
String trTimelapseCaption(TelegramLang lang, const String& cameraName, const String& timestamp);
String trTamperCaption(TelegramLang lang, const String& cameraName, const String& timestamp);
String trSignalLossMessage(TelegramLang lang, const String& cameraName, const String& timestamp);
String trMotionDigest(TelegramLang lang, const String& cameraName, uint32_t count, unsigned long elapsedSec);
// Summary of several cameras alerting together; cameraList is pre-joined.
String trMultiCameraDigest(TelegramLang lang, uint32_t count, const String& cameraList);
// Periodic per-camera detection counts. The caller only adds lines for cameras
// with a non-zero count.
String trDailyDigestHeader(TelegramLang lang);
String trDailyDigestCameraLine(TelegramLang lang, const String& cameraName, uint32_t personCount,
                                uint32_t vehicleCount, uint32_t petCount, uint32_t motionCount);
// Test alert caption - same keyword/emoji as the real kind (so phone
// automations can be tested) but always labelled as a test.
String trTestAlertCaption(TelegramLang lang, const String& cameraName, const String& timestamp,
                           MotionDetectionKind kind = MotionDetectionKind::Generic, bool isPetEvent = false);

// ---- System-message broadcasts (sendTelegramMessage's systemMessages recipients) ----

String trCameraOffline(TelegramLang lang, const String& cameraName, unsigned long minutes);
String trCameraBackOnline(TelegramLang lang, const String& cameraName);
String trSubscriptionLost(TelegramLang lang, const String& cameraName, unsigned long minutes);
String trMotionWatchdogTripped(TelegramLang lang, const String& cameraName, unsigned hours);
String trNvsUsageWarning(TelegramLang lang, unsigned pct);
// Sent before writes fail, once usage crosses SD_USAGE_WARN_PERCENT.
String trSdUsageWarning(TelegramLang lang, unsigned pct);
String trWifiWeakWarning(TelegramLang lang, int rssi);
String trHeapLowWarning(TelegramLang lang, uint32_t baselineBytes, uint32_t maxAllocBytes);
String trCameraTaskSpawnFailure(TelegramLang lang, const String& cameraName);
// hasFallbackTime: RTC or router time still in use vs. no time source at all.
String trNtpSyncFailed(TelegramLang lang, bool hasFallbackTime);
String trInternetOutageAlert(TelegramLang lang);
String trBridgeOutageAlert(TelegramLang lang);
// Sent once WAN is back. sinceTime is local "HH:MM" computed by the caller; ""
// (clock unsynced then) omits the "since" clause.
String trInternetRecovered(TelegramLang lang, const String& sinceTime, unsigned long outageDurationMs);
String trBridgeRecovered(TelegramLang lang, const String& sinceTime, unsigned long outageDurationMs);
// Mains monitor: the status line goes in the boot notice, lost/restored are
// sent on change.
String trPowerStatusLine(TelegramLang lang, bool present);
String trPowerLost(TelegramLang lang);
String trPowerRestored(TelegramLang lang);
String trSdFailure(TelegramLang lang, const String& reason);
// Boot-notice line when SD is enabled but didn't mount. The reason is shown on
// the Storage page.
String trSdNotAvailableAtBoot(TelegramLang lang);
String trSdCheckWarning(TelegramLang lang, size_t unreadableFiles, size_t filesChecked);
// Sent once, on the first boot after an OTA update confirms healthy.
String trOtaConfirmedHealthy(TelegramLang lang);
// Sent once when the bootloader rolled back a failed update
// (invalidPartitionLabel = the slot that failed).
String trOtaRolledBack(TelegramLang lang, const String& invalidPartitionLabel);
// afterLiveEdit: found at task start (false) or after a dashboard edit (true).
String trMissingCredentials(TelegramLang lang, const String& cameraName, bool afterLiveEdit);
String trTestMessage(TelegramLang lang);

// ---- Heartbeat / boot message building blocks ----

String trHeartbeatHeader(TelegramLang lang, const String& firmwareVersion);
// "Uptime: ..." - shared by the heartbeat, /uptime, and /health.
String trUptimeLine(TelegramLang lang, unsigned long ms);
String trFreeHeapLine(TelegramLang lang, uint32_t freeBytes, uint32_t minEverBytes);
String trNvsUsageLine(TelegramLang lang, unsigned pct);
String trWifiSignalLine(TelegramLang lang, int rssi);
// One camera's heartbeat line, no trailing newline.
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
// Whole /status line; timerSuffix from trTimerSuffix ("" = none); latencyMs <
// 0 = no data.
String trStatusCameraLine(TelegramLang lang, const String& cameraName, bool alertsEnabled, bool offline,
                            const String& timerSuffix, long avgLatencyMs);
String trRebootingNow(TelegramLang lang);
// /backup caption, and the fallback text if the upload fails (just retry; not
// queued).
String trBackupCaption(TelegramLang lang);
String trBackupFailed(TelegramLang lang);
// /restore: prompt for the file, and the window-expired reply.
String trRestorePrompt(TelegramLang lang);
String trRestoreExpired(TelegramLang lang);
// resultSummary is the same import summary the dashboard shows.
String trRestoreResult(TelegramLang lang, const String& resultSummary);
String trHelpText(TelegramLang lang, uint16_t eventLogCapacity, uint16_t maxDurationMinutes,
                    bool canCommand, bool canSnap, bool canReset, bool canBackup, bool canRestore);
String trHealthHeader(TelegramLang lang);
String trFreePsramLine(TelegramLang lang, uint32_t freeBytes);
// sdDetailText is one of the three below, already localised.
String trSdStorageLine(TelegramLang lang, const String& sdDetailText);
String trSdDisabledDetail(TelegramLang lang);
String trSdNotDetectedDetail(TelegramLang lang);
String trSdDetail(TelegramLang lang, const String& cardTypeName, double usedMB, double totalMB);
String trLogHeader(TelegramLang lang);
String trLogEmpty(TelegramLang lang);
// Localised twin of format_utils' formatElapsedSince (English, web-only).
String trElapsedSince(TelegramLang lang, unsigned long eventMs, unsigned long nowMs);
String trAmbiguousCamera(TelegramLang lang, const String& name, const String& matchList);
String trUnknownCamera(TelegramLang lang, const String& name);
String trNoCamerasToChoose(TelegramLang lang);
String trCameraPickerPrompt(TelegramLang lang, const String& commandDisplayName);
// Label only; the "all" callback token itself is protocol and never
// translated.
String trAllButtonLabel(TelegramLang lang);
String trCallbackDataTooLong(TelegramLang lang, size_t skipped, const String& commandDisplayName);
String trCallbackUnrecognized(TelegramLang lang);
String trCallbackNotAuthorized(TelegramLang lang);
String trCallbackCameraGone(TelegramLang lang);
String trCameraNoLongerAvailable(TelegramLang lang, const String& target);
String trNoSnapshotUriYet(TelegramLang lang, const String& cameraName);
String trSnapshotFetchFailed(TelegramLang lang, const String& cameraName);
String trDurationParseError(TelegramLang lang, const String& durationText, uint16_t maxMinutes);
// Pending-timer suffix, shared by /on|/off replies and /status.
String trTimerSuffix(TelegramLang lang, bool turnOn, unsigned long durationMs);
// subject is a camera name or trAllCamerasSubject.
String trAlertsState(TelegramLang lang, const String& subject, bool turnOn, const String& suffix);
// Appended when a timer's revert has just fired.
String trTimerExpiredSuffix(TelegramLang lang);
String trAllCamerasSubject(TelegramLang lang, size_t count);
String trNoEnabledCameras(TelegramLang lang);

// ---- /lang command (self-service language switch) ----

// Shown above the "English"/"Português" inline keyboard for a bare /lang.
String trLanguagePickerPrompt(TelegramLang lang);
// Written in the newly selected language.
String trLanguageChanged(TelegramLang newLang);
// In the sender's current language.
String trUnknownLanguageArg(TelegramLang lang, const String& arg);
String trLanguageChangeFailed(TelegramLang lang);
