#include <unity.h>
#include <Arduino.h>
#include "telegram_i18n.h"

void setUp(void) {}
void tearDown(void) {}

// Generic check reused throughout: English and Portuguese must never be
// byte-identical for a real sentence (a copy-paste-and-forget-to-translate
// bug), and both must actually contain the dynamic value passed in, not
// just some fixed template with the placeholder silently dropped.
static void assertDiffersAndContains(const String& en, const String& pt, const String& mustContain) {
  TEST_ASSERT_TRUE(en != pt);
  TEST_ASSERT_TRUE(en.indexOf(mustContain) >= 0);
  TEST_ASSERT_TRUE(pt.indexOf(mustContain) >= 0);
}

void test_trMotionCaption_pet_vs_motion_and_language(void) {
  String enPet = trMotionCaption(TelegramLang::English, "D01", "2024-01-01 10:00:00", true);
  String ptPet = trMotionCaption(TelegramLang::Portuguese, "D01", "2024-01-01 10:00:00", true);
  assertDiffersAndContains(enPet, ptPet, "D01");
  TEST_ASSERT_TRUE(enPet.indexOf("pet") >= 0);
  TEST_ASSERT_TRUE(ptPet.indexOf("animal") >= 0);

  // Plain motion (not a pet event) is name+timestamp only - identical in
  // both languages since there's no prose to translate.
  String enMotion = trMotionCaption(TelegramLang::English, "D01", "TS", false);
  String ptMotion = trMotionCaption(TelegramLang::Portuguese, "D01", "TS", false);
  TEST_ASSERT_EQUAL_STRING(enMotion.c_str(), ptMotion.c_str());
}

void test_trCameraOffline_contains_name_and_minutes(void) {
  String en = trCameraOffline(TelegramLang::English, "D07-PortariaEsq", 15);
  String pt = trCameraOffline(TelegramLang::Portuguese, "D07-PortariaEsq", 15);
  assertDiffersAndContains(en, pt, "D07-PortariaEsq");
  TEST_ASSERT_TRUE(en.indexOf("15") >= 0);
  TEST_ASSERT_TRUE(pt.indexOf("15") >= 0);
}

void test_trCameraBackOnline(void) {
  String en = trCameraBackOnline(TelegramLang::English, "D01");
  String pt = trCameraBackOnline(TelegramLang::Portuguese, "D01");
  assertDiffersAndContains(en, pt, "D01");
}

void test_trSubscriptionLost(void) {
  String en = trSubscriptionLost(TelegramLang::English, "D01", 10);
  String pt = trSubscriptionLost(TelegramLang::Portuguese, "D01", 10);
  assertDiffersAndContains(en, pt, "D01");
  TEST_ASSERT_TRUE(en.indexOf("10") >= 0);
  TEST_ASSERT_TRUE(pt.indexOf("10") >= 0);
}

void test_trMotionWatchdogTripped(void) {
  String en = trMotionWatchdogTripped(TelegramLang::English, "D01", 24);
  String pt = trMotionWatchdogTripped(TelegramLang::Portuguese, "D01", 24);
  assertDiffersAndContains(en, pt, "24");
}

void test_trNvsUsageWarning(void) {
  String en = trNvsUsageWarning(TelegramLang::English, 85);
  String pt = trNvsUsageWarning(TelegramLang::Portuguese, 85);
  assertDiffersAndContains(en, pt, "85");
}

void test_trWifiWeakWarning(void) {
  String en = trWifiWeakWarning(TelegramLang::English, -80);
  String pt = trWifiWeakWarning(TelegramLang::Portuguese, -80);
  assertDiffersAndContains(en, pt, "-80");
}

void test_trHeapLowWarning(void) {
  String en = trHeapLowWarning(TelegramLang::English, 12345, 6789);
  String pt = trHeapLowWarning(TelegramLang::Portuguese, 12345, 6789);
  assertDiffersAndContains(en, pt, "12345");
}

void test_trCameraTaskSpawnFailure(void) {
  String en = trCameraTaskSpawnFailure(TelegramLang::English, "D01");
  String pt = trCameraTaskSpawnFailure(TelegramLang::Portuguese, "D01");
  assertDiffersAndContains(en, pt, "D01");
}

void test_trInternetOutageAlert_and_trBridgeOutageAlert(void) {
  TEST_ASSERT_TRUE(trInternetOutageAlert(TelegramLang::English) != trInternetOutageAlert(TelegramLang::Portuguese));
  TEST_ASSERT_TRUE(trBridgeOutageAlert(TelegramLang::English) != trBridgeOutageAlert(TelegramLang::Portuguese));
}

void test_trPowerStatusLine_on_vs_off(void) {
  String enOn = trPowerStatusLine(TelegramLang::English, true);
  String enOff = trPowerStatusLine(TelegramLang::English, false);
  TEST_ASSERT_TRUE(enOn != enOff);
  TEST_ASSERT_TRUE(enOn.indexOf("ON") >= 0);
  TEST_ASSERT_TRUE(enOff.indexOf("OFF") >= 0);
  String ptOn = trPowerStatusLine(TelegramLang::Portuguese, true);
  String ptOff = trPowerStatusLine(TelegramLang::Portuguese, false);
  TEST_ASSERT_TRUE(ptOn != ptOff);
  TEST_ASSERT_TRUE(enOn != ptOn);
}

void test_trPowerLost_and_trPowerRestored(void) {
  TEST_ASSERT_TRUE(trPowerLost(TelegramLang::English) != trPowerLost(TelegramLang::Portuguese));
  TEST_ASSERT_TRUE(trPowerRestored(TelegramLang::English) != trPowerRestored(TelegramLang::Portuguese));
  TEST_ASSERT_TRUE(trPowerLost(TelegramLang::English) != trPowerRestored(TelegramLang::English));
}

void test_trSdFailure_and_trSdCheckWarning(void) {
  String en = trSdFailure(TelegramLang::English, "mount failed");
  String pt = trSdFailure(TelegramLang::Portuguese, "mount failed");
  assertDiffersAndContains(en, pt, "mount failed");

  String en2 = trSdCheckWarning(TelegramLang::English, 3, 100);
  String pt2 = trSdCheckWarning(TelegramLang::Portuguese, 3, 100);
  assertDiffersAndContains(en2, pt2, "100");
}

void test_trMissingCredentials_both_variants(void) {
  String enStartup = trMissingCredentials(TelegramLang::English, "D01", false);
  String enEdit = trMissingCredentials(TelegramLang::English, "D01", true);
  TEST_ASSERT_TRUE(enStartup != enEdit); // the two variants must actually differ
  String ptStartup = trMissingCredentials(TelegramLang::Portuguese, "D01", false);
  String ptEdit = trMissingCredentials(TelegramLang::Portuguese, "D01", true);
  TEST_ASSERT_TRUE(ptStartup != ptEdit);
  assertDiffersAndContains(enStartup, ptStartup, "D01");
  assertDiffersAndContains(enEdit, ptEdit, "D01");
}

void test_trTestMessage(void) {
  TEST_ASSERT_TRUE(trTestMessage(TelegramLang::English) != trTestMessage(TelegramLang::Portuguese));
}

void test_trHeartbeatHeader_and_trBootHeader(void) {
  String en = trHeartbeatHeader(TelegramLang::English, "1.2.3");
  String pt = trHeartbeatHeader(TelegramLang::Portuguese, "1.2.3");
  assertDiffersAndContains(en, pt, "1.2.3");

  String enBoot = trBootHeader(TelegramLang::English, "1.2.3");
  String ptBoot = trBootHeader(TelegramLang::Portuguese, "1.2.3");
  assertDiffersAndContains(enBoot, ptBoot, "1.2.3");
}

void test_trUptimeLine_matches_formatUptime(void) {
  // 90 minutes -> "1h 30m" via formatUptime - both languages must embed it verbatim.
  unsigned long ms = 90UL * 60UL * 1000UL;
  String en = trUptimeLine(TelegramLang::English, ms);
  String pt = trUptimeLine(TelegramLang::Portuguese, ms);
  assertDiffersAndContains(en, pt, "1h 30m");
}

void test_trFreeHeapLine_and_trNvsUsageLine_and_trWifiSignalLine(void) {
  assertDiffersAndContains(trFreeHeapLine(TelegramLang::English, 1000, 500),
                            trFreeHeapLine(TelegramLang::Portuguese, 1000, 500), "1000");
  assertDiffersAndContains(trNvsUsageLine(TelegramLang::English, 42),
                            trNvsUsageLine(TelegramLang::Portuguese, 42), "42");
  assertDiffersAndContains(trWifiSignalLine(TelegramLang::English, -55),
                            trWifiSignalLine(TelegramLang::Portuguese, -55), "-55");
}

void test_trHeartbeatCameraLine_alerts_off_permanent(void) {
  String en = trHeartbeatCameraLine(TelegramLang::English, "D01", true, false, false, false, false, "");
  String pt = trHeartbeatCameraLine(TelegramLang::Portuguese, "D01", true, false, false, false, false, "");
  assertDiffersAndContains(en, pt, "D01");
  TEST_ASSERT_TRUE(en.indexOf("alerts OFF") >= 0);
  TEST_ASSERT_TRUE(en.indexOf("until") < 0); // no timer pending - must not fabricate a time
}

void test_trHeartbeatCameraLine_alerts_off_until_time(void) {
  String en = trHeartbeatCameraLine(TelegramLang::English, "D07", true, false, false, true, true, "06:21");
  String pt = trHeartbeatCameraLine(TelegramLang::Portuguese, "D07", true, false, false, true, true, "06:21");
  assertDiffersAndContains(en, pt, "06:21");
  TEST_ASSERT_TRUE(en.indexOf("until 06:21") >= 0);
}

void test_trHeartbeatCameraLine_subscribed_and_offline_flags(void) {
  String notSubscribed = trHeartbeatCameraLine(TelegramLang::English, "D01", false, false, true, false, false, "");
  TEST_ASSERT_TRUE(notSubscribed.indexOf("NOT subscribed") >= 0);
  String offline = trHeartbeatCameraLine(TelegramLang::English, "D01", true, true, true, false, false, "");
  TEST_ASSERT_TRUE(offline.indexOf("OFFLINE") >= 0);
}

void test_trRebootReasonLine(void) {
  assertDiffersAndContains(trRebootReasonLine(TelegramLang::English, "power-on"),
                            trRebootReasonLine(TelegramLang::Portuguese, "power-on"), "power-on");
}

void test_trEnabledCamerasLine_and_trConfiguredCamerasHeader(void) {
  assertDiffersAndContains(trEnabledCamerasLine(TelegramLang::English, 3, 5),
                            trEnabledCamerasLine(TelegramLang::Portuguese, 3, 5), "3/5");
  TEST_ASSERT_TRUE(trConfiguredCamerasHeader(TelegramLang::English) !=
                    trConfiguredCamerasHeader(TelegramLang::Portuguese));
}

void test_trSdBootCheckWarning(void) {
  assertDiffersAndContains(trSdBootCheckWarning(TelegramLang::English, 2, 4),
                            trSdBootCheckWarning(TelegramLang::Portuguese, 2, 4), "2");
}

void test_trNotAuthorized_and_trRateLimited(void) {
  assertDiffersAndContains(trNotAuthorized(TelegramLang::English, "/reset"),
                            trNotAuthorized(TelegramLang::Portuguese, "/reset"), "/reset");
  TEST_ASSERT_TRUE(trRateLimited(TelegramLang::English) != trRateLimited(TelegramLang::Portuguese));
}

void test_trStatusHeader_and_trStatusCameraLine(void) {
  TEST_ASSERT_TRUE(trStatusHeader(TelegramLang::English) != trStatusHeader(TelegramLang::Portuguese));

  String en = trStatusCameraLine(TelegramLang::English, "D01", true, false, "", -1);
  TEST_ASSERT_EQUAL_STRING("D01: ON", en.c_str());
  String pt = trStatusCameraLine(TelegramLang::Portuguese, "D01", true, false, "", -1);
  TEST_ASSERT_EQUAL_STRING("D01: LIGADO", pt.c_str());

  String withLatency = trStatusCameraLine(TelegramLang::English, "D01", true, false, "", 42);
  TEST_ASSERT_TRUE(withLatency.indexOf("~42ms") >= 0);

  String offline = trStatusCameraLine(TelegramLang::English, "D01", false, true, "", -1);
  TEST_ASSERT_EQUAL_STRING("D01: OFF - OFFLINE", offline.c_str());
}

void test_trRebootingNow(void) {
  TEST_ASSERT_TRUE(trRebootingNow(TelegramLang::English) != trRebootingNow(TelegramLang::Portuguese));
}

void test_trHelpText_contains_dynamic_values_and_permissions(void) {
  String en = trHelpText(TelegramLang::English, 50, 720, true, false, true);
  TEST_ASSERT_TRUE(en.indexOf("50") >= 0);
  TEST_ASSERT_TRUE(en.indexOf("720") >= 0);
  TEST_ASSERT_TRUE(en.indexOf("canCommand=yes") >= 0);
  TEST_ASSERT_TRUE(en.indexOf("canSnap=no") >= 0);
  TEST_ASSERT_TRUE(en.indexOf("canReset=yes") >= 0);

  String pt = trHelpText(TelegramLang::Portuguese, 50, 720, true, false, true);
  TEST_ASSERT_TRUE(pt.indexOf("50") >= 0);
  TEST_ASSERT_TRUE(pt.indexOf("canCommand=sim") >= 0);
  TEST_ASSERT_TRUE(pt.indexOf("canSnap=n\xC3\xA3o") >= 0);
  TEST_ASSERT_TRUE(en != pt);
}

void test_trHealthHeader_and_trFreePsramLine_and_trSdStorageLine(void) {
  TEST_ASSERT_TRUE(trHealthHeader(TelegramLang::English) != trHealthHeader(TelegramLang::Portuguese));
  assertDiffersAndContains(trFreePsramLine(TelegramLang::English, 999),
                            trFreePsramLine(TelegramLang::Portuguese, 999), "999");
  assertDiffersAndContains(trSdStorageLine(TelegramLang::English, "SDHC, 1.0 MB / 2.0 MB used"),
                            trSdStorageLine(TelegramLang::Portuguese, "SDHC, 1.0 MB / 2.0 MB used"), "SDHC");
}

void test_trSdDisabledDetail_and_trSdNotDetectedDetail_and_trSdDetail(void) {
  TEST_ASSERT_TRUE(trSdDisabledDetail(TelegramLang::English) != trSdDisabledDetail(TelegramLang::Portuguese));
  TEST_ASSERT_TRUE(trSdNotDetectedDetail(TelegramLang::English) != trSdNotDetectedDetail(TelegramLang::Portuguese));
  assertDiffersAndContains(trSdDetail(TelegramLang::English, "SDHC", 1.5, 8.0),
                            trSdDetail(TelegramLang::Portuguese, "SDHC", 1.5, 8.0), "SDHC");
}

void test_trLogHeader_and_trLogEmpty(void) {
  TEST_ASSERT_TRUE(trLogHeader(TelegramLang::English) != trLogHeader(TelegramLang::Portuguese));
  TEST_ASSERT_TRUE(trLogEmpty(TelegramLang::English) != trLogEmpty(TelegramLang::Portuguese));
}

void test_trElapsedSince_just_now_and_ago(void) {
  String enJustNow = trElapsedSince(TelegramLang::English, 1000, 1500); // 500ms elapsed
  TEST_ASSERT_EQUAL_STRING("just now", enJustNow.c_str());
  String ptJustNow = trElapsedSince(TelegramLang::Portuguese, 1000, 1500);
  TEST_ASSERT_EQUAL_STRING("agora mesmo", ptJustNow.c_str());

  unsigned long longAgo = 5UL * 60UL * 1000UL; // 5 minutes elapsed
  String enAgo = trElapsedSince(TelegramLang::English, 0, longAgo);
  TEST_ASSERT_TRUE(enAgo.indexOf("ago") >= 0);
  String ptAgo = trElapsedSince(TelegramLang::Portuguese, 0, longAgo);
  TEST_ASSERT_TRUE(ptAgo.indexOf("atr") >= 0); // "atrás"
}

void test_trAmbiguousCamera_and_trUnknownCamera(void) {
  assertDiffersAndContains(trAmbiguousCamera(TelegramLang::English, "D0", "D01, D02"),
                            trAmbiguousCamera(TelegramLang::Portuguese, "D0", "D01, D02"), "D01, D02");
  assertDiffersAndContains(trUnknownCamera(TelegramLang::English, "D99"),
                            trUnknownCamera(TelegramLang::Portuguese, "D99"), "D99");
}

void test_trNoCamerasToChoose_and_trCameraPickerPrompt(void) {
  TEST_ASSERT_TRUE(trNoCamerasToChoose(TelegramLang::English) != trNoCamerasToChoose(TelegramLang::Portuguese));
  assertDiffersAndContains(trCameraPickerPrompt(TelegramLang::English, "/on"),
                            trCameraPickerPrompt(TelegramLang::Portuguese, "/on"), "/on");
}

void test_trAllButtonLabel(void) {
  TEST_ASSERT_TRUE(trAllButtonLabel(TelegramLang::English) != trAllButtonLabel(TelegramLang::Portuguese));
  TEST_ASSERT_EQUAL_STRING("All", trAllButtonLabel(TelegramLang::English).c_str());
}

void test_trCallbackDataTooLong(void) {
  assertDiffersAndContains(trCallbackDataTooLong(TelegramLang::English, 2, "/on"),
                            trCallbackDataTooLong(TelegramLang::Portuguese, 2, "/on"), "/on");
}

void test_trCallback_toast_texts(void) {
  TEST_ASSERT_TRUE(trCallbackUnrecognized(TelegramLang::English) != trCallbackUnrecognized(TelegramLang::Portuguese));
  TEST_ASSERT_TRUE(trCallbackNotAuthorized(TelegramLang::English) != trCallbackNotAuthorized(TelegramLang::Portuguese));
  TEST_ASSERT_TRUE(trCallbackCameraGone(TelegramLang::English) != trCallbackCameraGone(TelegramLang::Portuguese));
}

void test_trCameraNoLongerAvailable(void) {
  assertDiffersAndContains(trCameraNoLongerAvailable(TelegramLang::English, "D01"),
                            trCameraNoLongerAvailable(TelegramLang::Portuguese, "D01"), "D01");
}

void test_trNoSnapshotUriYet_and_trSnapshotFetchFailed(void) {
  assertDiffersAndContains(trNoSnapshotUriYet(TelegramLang::English, "D01"),
                            trNoSnapshotUriYet(TelegramLang::Portuguese, "D01"), "D01");
  assertDiffersAndContains(trSnapshotFetchFailed(TelegramLang::English, "D01"),
                            trSnapshotFetchFailed(TelegramLang::Portuguese, "D01"), "D01");
}

void test_trDurationParseError(void) {
  assertDiffersAndContains(trDurationParseError(TelegramLang::English, "xyz", 720),
                            trDurationParseError(TelegramLang::Portuguese, "xyz", 720), "xyz");
}

void test_trTimerSuffix_turnOn_true_means_auto_off_later(void) {
  unsigned long ms = 30UL * 60UL * 1000UL;
  String en = trTimerSuffix(TelegramLang::English, true, ms);
  TEST_ASSERT_TRUE(en.indexOf("OFF") >= 0);
  TEST_ASSERT_TRUE(en.indexOf("ON") < 0 || en.indexOf("OFF") < en.indexOf("ON")); // OFF appears, not ON as the verb
  String pt = trTimerSuffix(TelegramLang::Portuguese, true, ms);
  TEST_ASSERT_TRUE(pt.indexOf("DESLIGA") >= 0);

  String enOn = trTimerSuffix(TelegramLang::English, false, ms);
  TEST_ASSERT_TRUE(enOn.indexOf("ON") >= 0);
  String ptOn = trTimerSuffix(TelegramLang::Portuguese, false, ms);
  TEST_ASSERT_TRUE(ptOn.indexOf("LIGA") >= 0);
}

void test_trAlertsState_and_trAllCamerasSubject(void) {
  String en = trAlertsState(TelegramLang::English, "D01", true, "");
  TEST_ASSERT_EQUAL_STRING("D01 alerts: ON", en.c_str());
  String pt = trAlertsState(TelegramLang::Portuguese, "D01", true, "");
  TEST_ASSERT_EQUAL_STRING("D01 - alertas: LIGADOS", pt.c_str());

  String enOff = trAlertsState(TelegramLang::English, "D01", false, " (timer expired)");
  TEST_ASSERT_EQUAL_STRING("D01 alerts: OFF (timer expired)", enOff.c_str());

  assertDiffersAndContains(trAllCamerasSubject(TelegramLang::English, 3),
                            trAllCamerasSubject(TelegramLang::Portuguese, 3), "3");
}

void test_trTimerExpiredSuffix(void) {
  TEST_ASSERT_TRUE(trTimerExpiredSuffix(TelegramLang::English) != trTimerExpiredSuffix(TelegramLang::Portuguese));
}

void test_trNoEnabledCameras(void) {
  TEST_ASSERT_TRUE(trNoEnabledCameras(TelegramLang::English) != trNoEnabledCameras(TelegramLang::Portuguese));
}

void test_trLanguagePickerPrompt(void) {
  TEST_ASSERT_TRUE(trLanguagePickerPrompt(TelegramLang::English) != trLanguagePickerPrompt(TelegramLang::Portuguese));
}

// trLanguageChanged is phrased in the NEWLY selected language, not the
// sender's old one - the parameter IS the language to confirm in.
void test_trLanguageChanged_uses_the_new_language(void) {
  String toEnglish = trLanguageChanged(TelegramLang::English);
  TEST_ASSERT_TRUE(toEnglish.indexOf("English") >= 0);
  String toPortuguese = trLanguageChanged(TelegramLang::Portuguese);
  TEST_ASSERT_TRUE(toPortuguese.indexOf("portugu") >= 0);
  TEST_ASSERT_TRUE(toEnglish != toPortuguese);
}

void test_trUnknownLanguageArg_contains_the_bad_argument(void) {
  String en = trUnknownLanguageArg(TelegramLang::English, "xx");
  String pt = trUnknownLanguageArg(TelegramLang::Portuguese, "xx");
  TEST_ASSERT_TRUE(en.indexOf("xx") >= 0);
  TEST_ASSERT_TRUE(pt.indexOf("xx") >= 0);
  TEST_ASSERT_TRUE(en != pt);
}

void test_trLanguageChangeFailed(void) {
  TEST_ASSERT_TRUE(trLanguageChangeFailed(TelegramLang::English) != trLanguageChangeFailed(TelegramLang::Portuguese));
}

void test_trHelpText_mentions_lang_command(void) {
  String en = trHelpText(TelegramLang::English, 50, 720, true, true, true);
  TEST_ASSERT_TRUE(en.indexOf("/lang") >= 0);
  String pt = trHelpText(TelegramLang::Portuguese, 50, 720, true, true, true);
  TEST_ASSERT_TRUE(pt.indexOf("/lang") >= 0);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_trMotionCaption_pet_vs_motion_and_language);
  RUN_TEST(test_trCameraOffline_contains_name_and_minutes);
  RUN_TEST(test_trCameraBackOnline);
  RUN_TEST(test_trSubscriptionLost);
  RUN_TEST(test_trMotionWatchdogTripped);
  RUN_TEST(test_trNvsUsageWarning);
  RUN_TEST(test_trWifiWeakWarning);
  RUN_TEST(test_trHeapLowWarning);
  RUN_TEST(test_trCameraTaskSpawnFailure);
  RUN_TEST(test_trInternetOutageAlert_and_trBridgeOutageAlert);
  RUN_TEST(test_trPowerStatusLine_on_vs_off);
  RUN_TEST(test_trPowerLost_and_trPowerRestored);
  RUN_TEST(test_trSdFailure_and_trSdCheckWarning);
  RUN_TEST(test_trMissingCredentials_both_variants);
  RUN_TEST(test_trTestMessage);
  RUN_TEST(test_trHeartbeatHeader_and_trBootHeader);
  RUN_TEST(test_trUptimeLine_matches_formatUptime);
  RUN_TEST(test_trFreeHeapLine_and_trNvsUsageLine_and_trWifiSignalLine);
  RUN_TEST(test_trHeartbeatCameraLine_alerts_off_permanent);
  RUN_TEST(test_trHeartbeatCameraLine_alerts_off_until_time);
  RUN_TEST(test_trHeartbeatCameraLine_subscribed_and_offline_flags);
  RUN_TEST(test_trRebootReasonLine);
  RUN_TEST(test_trEnabledCamerasLine_and_trConfiguredCamerasHeader);
  RUN_TEST(test_trSdBootCheckWarning);
  RUN_TEST(test_trNotAuthorized_and_trRateLimited);
  RUN_TEST(test_trStatusHeader_and_trStatusCameraLine);
  RUN_TEST(test_trRebootingNow);
  RUN_TEST(test_trHelpText_contains_dynamic_values_and_permissions);
  RUN_TEST(test_trHealthHeader_and_trFreePsramLine_and_trSdStorageLine);
  RUN_TEST(test_trSdDisabledDetail_and_trSdNotDetectedDetail_and_trSdDetail);
  RUN_TEST(test_trLogHeader_and_trLogEmpty);
  RUN_TEST(test_trElapsedSince_just_now_and_ago);
  RUN_TEST(test_trAmbiguousCamera_and_trUnknownCamera);
  RUN_TEST(test_trNoCamerasToChoose_and_trCameraPickerPrompt);
  RUN_TEST(test_trAllButtonLabel);
  RUN_TEST(test_trCallbackDataTooLong);
  RUN_TEST(test_trCallback_toast_texts);
  RUN_TEST(test_trCameraNoLongerAvailable);
  RUN_TEST(test_trNoSnapshotUriYet_and_trSnapshotFetchFailed);
  RUN_TEST(test_trDurationParseError);
  RUN_TEST(test_trTimerSuffix_turnOn_true_means_auto_off_later);
  RUN_TEST(test_trAlertsState_and_trAllCamerasSubject);
  RUN_TEST(test_trTimerExpiredSuffix);
  RUN_TEST(test_trNoEnabledCameras);
  RUN_TEST(test_trLanguagePickerPrompt);
  RUN_TEST(test_trLanguageChanged_uses_the_new_language);
  RUN_TEST(test_trUnknownLanguageArg_contains_the_bad_argument);
  RUN_TEST(test_trLanguageChangeFailed);
  RUN_TEST(test_trHelpText_mentions_lang_command);
  return UNITY_END();
}
