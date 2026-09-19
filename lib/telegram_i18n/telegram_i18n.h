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

// Which ONVIF detection topic actually fired, for trMotionCaption's
// wording below - camera.cpp's parseEvents decides this from
// CameraEventClassification (lib/camera_parse), preferring Person over
// Vehicle when a camera reports both in the same event batch (not a
// meaningful priority otherwise, just a tie-break - see parseEvents' own
// comment). Generic covers plain MotionAlarm/CellMotionDetector, or a
// camera that doesn't support AI person/vehicle classification at all.
enum class MotionDetectionKind { Generic, Person, Vehicle };

// Motion/pet-detected photo caption. isPetEvent picks the paw-print
// wording (kind is ignored entirely in that case - pets have their own
// fixed wording, independent of the Generic/Person/Vehicle a DogCatDetect
// topic never reports); otherwise kind (default Generic) picks a
// distinct emoji/keyword per detection type - see MotionDetectionKind's
// own comment for why this exists at all (a phone-side notification
// automation, e.g. MacroDroid/Tasker, playing a different sound per
// detection type by matching this message's text, without this project
// needing to know anything about sounds). The "(i/N)" burst suffix
// (numbers/slash only) is appended by the caller, not part of this -
// it's language-neutral either way.
String trMotionCaption(TelegramLang lang, const String& cameraName, const String& timestamp, bool isPetEvent,
                        MotionDetectionKind kind = MotionDetectionKind::Generic);
// Pet alert, text-only delivery mode (CameraConfig::petAlertsTextOnly) - no photo.
String trPetAlertText(TelegramLang lang, const String& cameraName, const String& timestamp);
String trTimelapseCaption(TelegramLang lang, const String& cameraName, const String& timestamp);
String trTamperCaption(TelegramLang lang, const String& cameraName, const String& timestamp);
String trSignalLossMessage(TelegramLang lang, const String& cameraName, const String& timestamp);
String trMotionDigest(TelegramLang lang, const String& cameraName, uint32_t count, unsigned long elapsedSec);
// Cross-camera correlation summary (telegram.cpp's checkMultiCameraAlertDigest) -
// distinct from trMotionDigest above, which is about repeated motion on
// ONE camera during its own cooldown. cameraList is already comma-joined
// by the caller - language-neutral, not part of this.
String trMultiCameraDigest(TelegramLang lang, uint32_t count, const String& cameraList);
// Periodic activity-volume summary (main.cpp's loop(), telegram.cpp's
// checkDailyActivityDigest) - a per-camera detection count over the
// interval since the last digest, distinct from trMotionDigest above (one
// camera's own cooldown-triggered follow-up) and trMultiCameraDigest
// (several cameras correlated together in one short window): this is a
// periodic volume rollup sent regardless of whether any single alert's
// own cooldown ever fired a digest of its own. trDailyDigestHeader is the
// message's first line; trDailyDigestCameraLine is one camera's own line
// within it (only cameras with at least one non-zero count get a line -
// the caller skips the rest).
String trDailyDigestHeader(TelegramLang lang);
String trDailyDigestCameraLine(TelegramLang lang, const String& cameraName, uint32_t personCount,
                                uint32_t vehicleCount, uint32_t petCount, uint32_t motionCount);
// Manual "Send test alert" button (Cameras dashboard page) - a real photo
// send through the same recipient list a real motion alert would use,
// clearly labeled so it's never mistaken for one.
// kind (default Generic) lets the test alert embed the SAME
// emoji/keyword a real Person/Vehicle detection caption would
// (trMotionCaption) - the whole point being to verify a phone-side
// notification automation (MacroDroid/Tasker) actually fires for that
// specific kind, without waiting for a real detection. isPetEvent
// (default false) is checked first, same precedence as trMotionCaption's
// own isPetEvent/kind pair - Pet is a separate signal from
// MotionDetectionKind, not one of its values, since a real pet detection
// (camera.cpp's DogCatDetect handling) is independent of the
// person/vehicle classification. Still clearly labeled "TEST ALERT"/
// "ALERTA DE TESTE" regardless of kind/isPetEvent, so it's never mistaken
// for a real one when reviewing chat history later.
String trTestAlertCaption(TelegramLang lang, const String& cameraName, const String& timestamp,
                           MotionDetectionKind kind = MotionDetectionKind::Generic, bool isPetEvent = false);

// ---- System-message broadcasts (sendTelegramMessage's systemMessages recipients) ----

String trCameraOffline(TelegramLang lang, const String& cameraName, unsigned long minutes);
String trCameraBackOnline(TelegramLang lang, const String& cameraName);
String trSubscriptionLost(TelegramLang lang, const String& cameraName, unsigned long minutes);
String trMotionWatchdogTripped(TelegramLang lang, const String& cameraName, unsigned hours);
String trNvsUsageWarning(TelegramLang lang, unsigned pct);
// Proactive counterpart to trSdFailure below - fires BEFORE a write
// actually fails, once usage crosses SD_USAGE_WARN_PERCENT (config.h).
String trSdUsageWarning(TelegramLang lang, unsigned pct);
String trWifiWeakWarning(TelegramLang lang, int rssi);
String trHeapLowWarning(TelegramLang lang, uint32_t baselineBytes, uint32_t maxAllocBytes);
String trCameraTaskSpawnFailure(TelegramLang lang, const String& cameraName);
// hasFallbackTime picks whether the message reassures that a fallback
// time (RTC-seeded, or seeded from the router's own HTTP Date header) is
// still in use, or warns that the system clock has no time source at
// all - see main.cpp's setupTime() for when each applies.
String trNtpSyncFailed(TelegramLang lang, bool hasFallbackTime);
String trInternetOutageAlert(TelegramLang lang);
String trBridgeOutageAlert(TelegramLang lang);
// Sent once connectivity is confirmed restored, not when the outage began
// or crossed the pulse threshold - see NetWatchdogCheckResult's own
// comment (net_watchdog.h) for why. sinceTime is a pre-formatted "HH:MM"
// local clock string (formatLocalClockTime, telegram.h - not callable
// from this native-testable lib, so the caller in main.cpp computes it);
// "" means the clock wasn't synced when the outage started, in which case
// the message omits the "since HH:MM" clause and reports only the
// duration.
String trInternetRecovered(TelegramLang lang, const String& sinceTime, unsigned long outageDurationMs);
String trBridgeRecovered(TelegramLang lang, const String& sinceTime, unsigned long outageDurationMs);
// 220V mains power monitor (power_monitor.h) - trPowerStatusLine is folded
// into the boot/online message (main.cpp); trPowerLost/trPowerRestored are
// sent standalone on a confirmed state change.
String trPowerStatusLine(TelegramLang lang, bool present);
String trPowerLost(TelegramLang lang);
String trPowerRestored(TelegramLang lang);
String trSdFailure(TelegramLang lang, const String& reason);
// Folded into the boot message (main.cpp), same as trPowerStatusLine above -
// unlike trSdFailure (a mid-session I/O failure disabling SD for the rest
// of the session), this is SD never having come up in the first place at
// boot (initSdStorage/sd_store.cpp's three failure reasons: no module, no
// card, or the /snapshots directory couldn't be created) - previously only
// a Serial.println nobody watching the dashboard would ever see.
// Deliberately just says something's wrong, not why - see this function's
// own comment (telegram_i18n.cpp) for where the specific reason goes
// instead.
String trSdNotAvailableAtBoot(TelegramLang lang);
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
// /backup command - trBackupCaption is the document's own caption text;
// trBackupFailed is sent as a plain text fallback if the send itself fails
// (e.g. WAN blip mid-upload - the export text is cheap to regenerate, so
// this just tells the sender to try again rather than being retry-queued).
String trBackupCaption(TelegramLang lang);
String trBackupFailed(TelegramLang lang);
// /restore command - trRestorePrompt asks the sender to send the config
// file now (armed by a bare "/restore"); trRestoreExpired covers the
// pending-window-lapsed case. Not-authorized reuses trNotAuthorized above
// (commandDisplayName(TelegramCommand::Restore)) rather than a dedicated
// function - same message shape as every other command's rejection. See
// telegram.cpp's own comment on the full two-step (or one-step,
// caption="/restore") design.
String trRestorePrompt(TelegramLang lang);
String trRestoreExpired(TelegramLang lang);
// resultSummary is already-composed, language-neutral prose (counts of
// what was imported/skipped) - same webserver_security.h
// renderImportResultBanner text the dashboard's own Import shows, reused
// as-is rather than re-translated a second time.
String trRestoreResult(TelegramLang lang, const String& resultSummary);
String trHelpText(TelegramLang lang, uint16_t eventLogCapacity, uint16_t maxDurationMinutes,
                    bool canCommand, bool canSnap, bool canReset, bool canBackup, bool canRestore);
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
// The picker keyboard's own "apply to every camera" button label -
// distinct from trAllCamerasSubject (the "All N camera(s) alerts: ON"
// reply text after tapping it/using /on all) and from the literal "all"
// callback_data token (sendCameraPickerKeyboard, telegram.cpp), which is
// a protocol identifier matched case-insensitively in
// handleTelegramCallbackQuery and must NOT be translated.
String trAllButtonLabel(TelegramLang lang);
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

// ---- /lang command (self-service language switch) ----

// Shown above the "English"/"Português" inline keyboard for a bare /lang.
String trLanguagePickerPrompt(TelegramLang lang);
// Confirmation after a language change - phrased in the NEWLY selected
// language (newLang), not whatever the user was on before switching: the
// whole point of switching is to see the very next message in it.
String trLanguageChanged(TelegramLang newLang);
// "/lang xyz" where xyz isn't a known language code - phrased in the
// sender's CURRENT (unchanged) language.
String trUnknownLanguageArg(TelegramLang lang, const String& arg);
// NVS write failure persisting the change - phrased in the sender's
// CURRENT (unchanged, since the save failed) language.
String trLanguageChangeFailed(TelegramLang lang);
