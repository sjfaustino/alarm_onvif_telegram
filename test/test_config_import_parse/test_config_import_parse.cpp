#include <unity.h>
#include <Arduino.h>
#include "config_import_parse.h"

void setUp(void) {}
void tearDown(void) {}

static CameraConfig sampleCamera(const char* name) {
  CameraConfig c;
  c.name = name;
  c.deviceServiceUrl = "http://192.168.1.50/onvif/device_service";
  c.enabled = true;
  c.user = "admin";
  return c;
}

static TelegramUser sampleUser(const char* name) {
  TelegramUser u;
  u.name = name;
  u.chatId = "12345";
  u.allCameras = true;
  return u;
}

static WifiCredentials sampleNetwork() {
  WifiCredentials n;
  n.primary.ssid = "HomeWiFi";
  n.hostname = "cameramonitor";
  n.ntpServer = "pool.ntp.org";
  n.ntpSyncIntervalMs = 3600000UL;
  return n;
}

// A realistic full export's machine blocks - built from the real
// serializers, not hand-typed field strings, so this exercises the exact
// bytes buildConfigExport() would actually produce.
static String fullExportText() {
  String text = "=== Camera Monitor Configuration Export ===\n";
  text += "\n--- Cameras (2) ---\n[prose omitted in this test]\n";
  text += "### CAMERAS v" + String(CAMERA_SCHEMA_VERSION) + "\n";
  text += serializeCamera(sampleCamera("D01-Front")) + "\n";
  text += serializeCamera(sampleCamera("D02-Back")) + "\n";
  text += "\n--- Telegram Users (1) ---\n[prose omitted]\n";
  text += "### TELEGRAM_USERS v" + String(TELEGRAM_USER_SCHEMA_VERSION) + "\n";
  text += serializeUser(sampleUser("Admin")) + "\n";
  text += "\n--- Network ---\n[prose omitted]\n";
  text += "### NETWORK v" + String(NETWORK_SCHEMA_VERSION) + "\n";
  text += serializeNetworkConfig(sampleNetwork()) + "\n";
  text += "\n--- Storage ---\n[prose omitted]\n";
  text += "### SDSETTINGS v1\n";
  text += "1" + String((char)0x1F) + "168\n";
  return text;
}

void test_full_export_all_sections_found_and_parsed(void) {
  ConfigImportResult r = parseConfigImport(fullExportText());

  TEST_ASSERT_TRUE(r.camerasFound);
  TEST_ASSERT_EQUAL(2, (int)r.cameras.size());
  TEST_ASSERT_EQUAL_STRING("D01-Front", r.cameras[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("D02-Back", r.cameras[1].name.c_str());

  TEST_ASSERT_TRUE(r.usersFound);
  TEST_ASSERT_EQUAL(1, (int)r.users.size());
  TEST_ASSERT_EQUAL_STRING("Admin", r.users[0].name.c_str());

  TEST_ASSERT_TRUE(r.networkFound);
  TEST_ASSERT_EQUAL_STRING("HomeWiFi", r.network.primary.ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("", r.network.primary.password.c_str()); // never in the export

  TEST_ASSERT_TRUE(r.sdSettingsFound);
  TEST_ASSERT_TRUE(r.sdSettings.enabled);
  TEST_ASSERT_EQUAL_UINT32(168, r.sdSettings.checkIntervalHours);
}

// A file with only a Cameras section (e.g. hand-trimmed, or a future
// export mode that only covers cameras) still imports what it has -
// the other three domains are left untouched by the caller (webserver_
// security.cpp), signaled here simply by their *Found flags staying false.
void test_partial_export_only_cameras_section(void) {
  String text = "### CAMERAS v" + String(CAMERA_SCHEMA_VERSION) + "\n" +
                serializeCamera(sampleCamera("Solo")) + "\n";
  ConfigImportResult r = parseConfigImport(text);

  TEST_ASSERT_TRUE(r.camerasFound);
  TEST_ASSERT_EQUAL(1, (int)r.cameras.size());
  TEST_ASSERT_FALSE(r.usersFound);
  TEST_ASSERT_FALSE(r.networkFound);
  TEST_ASSERT_FALSE(r.sdSettingsFound);
}

// Prose-only text (an export from before this Import feature existed, or a
// completely unrelated file) has no marker lines at all - nothing found,
// nothing crashes.
void test_no_markers_finds_nothing(void) {
  ConfigImportResult r = parseConfigImport("Just some random text\nwith multiple lines\nno markers here.");
  TEST_ASSERT_FALSE(r.camerasFound);
  TEST_ASSERT_FALSE(r.usersFound);
  TEST_ASSERT_FALSE(r.networkFound);
  TEST_ASSERT_FALSE(r.sdSettingsFound);
}

void test_empty_input_finds_nothing(void) {
  ConfigImportResult r = parseConfigImport("");
  TEST_ASSERT_FALSE(r.camerasFound);
  TEST_ASSERT_FALSE(r.usersFound);
}

// A marker present but with zero data rows after it is a legitimate "this
// export genuinely had no cameras" - found stays true, list stays empty
// (see ConfigImportResult::camerasFound's own comment).
void test_cameras_marker_with_no_rows_is_found_but_empty(void) {
  ConfigImportResult r = parseConfigImport("### CAMERAS v" + String(CAMERA_SCHEMA_VERSION) + "\n");
  TEST_ASSERT_TRUE(r.camerasFound);
  TEST_ASSERT_EQUAL(0, (int)r.cameras.size());
}

// One malformed row inside an otherwise-good Cameras section is skipped,
// not fatal to the rest - same convention camera_store.cpp's own
// loadCameras() uses for a corrupted NVS record.
void test_one_malformed_camera_row_is_skipped_others_still_parsed(void) {
  String text = "### CAMERAS v" + String(CAMERA_SCHEMA_VERSION) + "\n" +
                serializeCamera(sampleCamera("Good1")) + "\n" +
                "not|enough|fields\n" +
                serializeCamera(sampleCamera("Good2")) + "\n";
  ConfigImportResult r = parseConfigImport(text);
  TEST_ASSERT_TRUE(r.camerasFound);
  TEST_ASSERT_EQUAL(2, (int)r.cameras.size());
  TEST_ASSERT_EQUAL_STRING("Good1", r.cameras[0].name.c_str());
  TEST_ASSERT_EQUAL_STRING("Good2", r.cameras[1].name.c_str());
}

// Network/SdSettings are single-record domains with no "some rows good"
// middle ground - a malformed line must NOT be trusted as found (applying
// a blank WifiCredentials/SdSettings would wipe good config with garbage).
void test_malformed_network_line_is_not_found(void) {
  ConfigImportResult r = parseConfigImport("### NETWORK v" + String(NETWORK_SCHEMA_VERSION) + "\n"
                                            "too|few|fields\n");
  TEST_ASSERT_FALSE(r.networkFound);
}

void test_malformed_sdsettings_line_is_not_found(void) {
  ConfigImportResult r = parseConfigImport("### SDSETTINGS v1\nnoSeparatorHere\n");
  TEST_ASSERT_FALSE(r.sdSettingsFound);
}

// An older schema version tag (e.g. a camera exported by earlier firmware,
// back when CAMERA_SCHEMA_VERSION was 1) still parses correctly today -
// the whole point of reusing deserializeCamera's own version tolerance.
void test_older_schema_version_still_parses(void) {
  // V1's exact 14-field layout (camera_serialize.cpp's deserializeCameraV1).
  String v1Fields = String("D09") + (char)0x1F + "http://192.168.1.60/onvif/device_service" + (char)0x1F +
                     "1" + (char)0x1F + "1" + (char)0x1F + "0" + (char)0x1F + "0" + (char)0x1F +
                     "" + (char)0x1F + "" + (char)0x1F + "user" + (char)0x1F + "pass" + (char)0x1F +
                     "notes" + (char)0x1F + "60000" + (char)0x1F + "300000" + (char)0x1F + "2";
  String text = "### CAMERAS v1\n" + v1Fields + "\n";
  ConfigImportResult r = parseConfigImport(text);
  TEST_ASSERT_TRUE(r.camerasFound);
  TEST_ASSERT_EQUAL(1, (int)r.cameras.size());
  TEST_ASSERT_EQUAL_STRING("D09", r.cameras[0].name.c_str());
}

// CRLF line endings (a file re-saved by a Windows text editor) must not
// break marker detection or record parsing.
void test_crlf_line_endings_tolerated(void) {
  String text = "### CAMERAS v" + String(CAMERA_SCHEMA_VERSION) + "\r\n" +
                serializeCamera(sampleCamera("CRLF-Cam")) + "\r\n";
  ConfigImportResult r = parseConfigImport(text);
  TEST_ASSERT_TRUE(r.camerasFound);
  TEST_ASSERT_EQUAL(1, (int)r.cameras.size());
  TEST_ASSERT_EQUAL_STRING("CRLF-Cam", r.cameras[0].name.c_str());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_full_export_all_sections_found_and_parsed);
  RUN_TEST(test_partial_export_only_cameras_section);
  RUN_TEST(test_no_markers_finds_nothing);
  RUN_TEST(test_empty_input_finds_nothing);
  RUN_TEST(test_cameras_marker_with_no_rows_is_found_but_empty);
  RUN_TEST(test_one_malformed_camera_row_is_skipped_others_still_parsed);
  RUN_TEST(test_malformed_network_line_is_not_found);
  RUN_TEST(test_malformed_sdsettings_line_is_not_found);
  RUN_TEST(test_older_schema_version_still_parses);
  RUN_TEST(test_crlf_line_endings_tolerated);
  return UNITY_END();
}
