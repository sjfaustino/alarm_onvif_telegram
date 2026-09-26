#include <unity.h>
#include "config_import_summary.h"

void setUp(void) {}
void tearDown(void) {}

static ConfigImportApplyResult camerasOnly(bool backupSaved) {
  ConfigImportApplyResult r;
  r.anyDomainFound = true;
  r.camerasImported = true;
  r.cameraCount = 2;
  r.backupSaved = backupSaved;
  return r;
}

void test_no_domain_found(void) {
  ConfigImportApplyResult r;
  TEST_ASSERT_EQUAL_STRING(
      "No valid configuration sections found in this file - nothing was changed. "
      "(Only files exported by this build or later can be restored - an older export "
      "has nothing for Import to read.)",
      summarizeImportResult(r, ImportSummaryFormat::Html).c_str());
}

void test_html_banner_exact(void) {
  // Pinned byte-for-byte: this is the dashboard banner's text from before
  // the summary moved out of webserver_security.cpp.
  TEST_ASSERT_EQUAL_STRING(
      "Imported 2 camera(s). Telegram Users, Network, SD Settings, Internet Watchdog, "
      "Camera Bridge Watchdog, 220V Power Monitor not found in this file (or failed to save) - "
      "left unchanged. A backup of what was stored just before this import was "
      "saved automatically - <a href=\"/import/backup\">download it</a> if you need to undo "
      "this. Reboot the board (Maintenance page) to apply.",
      summarizeImportResult(camerasOnly(true), ImportSummaryFormat::Html).c_str());
}

void test_plain_text_differs_only_in_backup_pointer(void) {
  String html = summarizeImportResult(camerasOnly(true), ImportSummaryFormat::Html);
  String plain = summarizeImportResult(camerasOnly(true), ImportSummaryFormat::PlainText);
  // What telegram.cpp used to do to the HTML banner by hand.
  html.replace("<a href=\"/import/backup\">download it</a>", "download it from the dashboard's Security page");
  TEST_ASSERT_EQUAL_STRING(html.c_str(), plain.c_str());
  TEST_ASSERT_EQUAL(-1, plain.indexOf('<'));
}

void test_backup_failed_has_no_link_in_either_format(void) {
  String html = summarizeImportResult(camerasOnly(false), ImportSummaryFormat::Html);
  String plain = summarizeImportResult(camerasOnly(false), ImportSummaryFormat::PlainText);
  TEST_ASSERT_EQUAL_STRING(html.c_str(), plain.c_str());
  TEST_ASSERT_TRUE(html.indexOf("FAILED to save") >= 0);
  TEST_ASSERT_EQUAL(-1, html.indexOf("<a "));
}

void test_rejected_duplicate_is_not_listed_as_skipped(void) {
  ConfigImportApplyResult r;
  r.anyDomainFound = true;
  r.camerasRejectedDuplicate = true;
  r.usersImported = true;
  r.userCount = 1;
  r.backupSaved = true;
  String s = summarizeImportResult(r, ImportSummaryFormat::PlainText);
  TEST_ASSERT_TRUE(s.startsWith("Imported 1 Telegram user(s)."));
  TEST_ASSERT_TRUE(s.indexOf("Cameras section REJECTED") >= 0);
  TEST_ASSERT_EQUAL(-1, s.indexOf("Cameras, "));
}

void test_nothing_imported_has_no_reboot_or_backup_line(void) {
  ConfigImportApplyResult r;
  r.anyDomainFound = true;
  r.backupSaved = true;
  String s = summarizeImportResult(r, ImportSummaryFormat::Html);
  TEST_ASSERT_TRUE(s.startsWith("Nothing was imported."));
  TEST_ASSERT_EQUAL(-1, s.indexOf("Reboot"));
  TEST_ASSERT_EQUAL(-1, s.indexOf("backup"));
}

void test_network_import_warns_about_wifi_password(void) {
  ConfigImportApplyResult r;
  r.anyDomainFound = true;
  r.networkImported = true;
  r.backupSaved = true;
  String s = summarizeImportResult(r, ImportSummaryFormat::PlainText);
  TEST_ASSERT_TRUE(s.indexOf("Network was imported WITHOUT a WiFi password") >= 0);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_no_domain_found);
  RUN_TEST(test_html_banner_exact);
  RUN_TEST(test_plain_text_differs_only_in_backup_pointer);
  RUN_TEST(test_backup_failed_has_no_link_in_either_format);
  RUN_TEST(test_rejected_duplicate_is_not_listed_as_skipped);
  RUN_TEST(test_nothing_imported_has_no_reboot_or_backup_line);
  RUN_TEST(test_network_import_warns_about_wifi_password);
  return UNITY_END();
}
