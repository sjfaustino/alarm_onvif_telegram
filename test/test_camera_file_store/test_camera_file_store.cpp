#include <unity.h>
#include <map>
#include <string>
#include "camera_file_store.h"
#include "camera_serialize.h"

void setUp(void) {}
void tearDown(void) {}

// In-memory filesystem whose steps can be made to fail.
class FakeFs : public CameraFileOps {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  bool failReads = false;
  size_t shortWriteBy = 0;
  int renameFailures = 0;
  bool removeFails = false;
  int renames = 0;

  bool exists(const char* path) override { return files.count(path) > 0; }
  bool read(const char* path, std::vector<uint8_t>& out) override {
    if (failReads || !files.count(path)) return false;
    out = files[path];
    return true;
  }
  size_t write(const char* path, const std::vector<uint8_t>& data) override {
    size_t n = data.size() - shortWriteBy;
    files[path] = std::vector<uint8_t>(data.begin(), data.begin() + n);
    return n;
  }
  bool rename(const char* from, const char* to) override {
    renames++;
    if (renameFailures > 0) { renameFailures--; return false; }
    if (!files.count(from)) return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
  bool remove(const char* path) override {
    if (removeFails) return false;
    return files.erase(path) > 0;
  }
};

static CameraConfig cam(const char* name, const char* pass = "secret") {
  CameraConfig c;
  c.name = name;
  c.deviceServiceUrl = "http://10.0.0.1/onvif/device_service";
  c.user = "admin";
  c.pass = pass;
  return c;
}

static std::vector<CameraConfig> listOf(std::initializer_list<const char*> names) {
  std::vector<CameraConfig> out;
  for (const char* n : names) out.push_back(cam(n));
  return out;
}

static void ignoreDropped(const String&) {}

// What a board would load from this filesystem right now.
static std::vector<String> storedNames(FakeFs& fs) {
  uint16_t version = 0;
  String records;
  std::vector<String> names;
  if (readCameraListFile(fs, version, records) != CameraFileStatus::Loaded) return names;
  for (auto& c : parseCameraList(records, version, ignoreDropped).cameras) names.push_back(c.name);
  return names;
}

static void assertNames(std::initializer_list<const char*> expected, const std::vector<String>& actual) {
  TEST_ASSERT_EQUAL((int)expected.size(), (int)actual.size());
  size_t i = 0;
  for (const char* e : expected) TEST_ASSERT_EQUAL_STRING(e, actual[i++].c_str());
}

// ---- format ----

void test_file_starts_with_the_schema_version_little_endian(void) {
  std::vector<uint8_t> raw = encodeCameraList(listOf({"A"}));
  TEST_ASSERT_EQUAL_UINT8(CAMERA_SCHEMA_VERSION & 0xFF, raw[0]);
  TEST_ASSERT_EQUAL_UINT8(CAMERA_SCHEMA_VERSION >> 8, raw[1]);
}

void test_list_survives_a_save_and_load(void) {
  FakeFs fs;
  TEST_ASSERT_NULL(writeCameraListFile(fs, encodeCameraList(listOf({"D01", "D02", "D03"}))));
  assertNames({"D01", "D02", "D03"}, storedNames(fs));
  TEST_ASSERT_FALSE(fs.exists(CAMERA_FILE_TMP));
}

void test_empty_list_round_trips_as_loaded_and_empty(void) {
  FakeFs fs;
  TEST_ASSERT_NULL(writeCameraListFile(fs, encodeCameraList({})));
  uint16_t version = 0;
  String records;
  TEST_ASSERT_TRUE(readCameraListFile(fs, version, records) == CameraFileStatus::Loaded);
  TEST_ASSERT_EQUAL_UINT16(CAMERA_SCHEMA_VERSION, version);
  TEST_ASSERT_EQUAL(0, (int)parseCameraList(records, version, ignoreDropped).total);
}

// ---- saving ----

void test_short_write_keeps_the_old_list_and_cleans_up(void) {
  FakeFs fs;
  writeCameraListFile(fs, encodeCameraList(listOf({"OLD"})));
  fs.shortWriteBy = 3;
  TEST_ASSERT_NOT_NULL(writeCameraListFile(fs, encodeCameraList(listOf({"NEW"}))));
  fs.shortWriteBy = 0;
  assertNames({"OLD"}, storedNames(fs));
  TEST_ASSERT_FALSE(fs.exists(CAMERA_FILE_TMP));
  TEST_ASSERT_EQUAL(1, fs.renames); // only the first, successful save renamed
}

void test_rename_refused_falls_back_to_remove_then_rename(void) {
  FakeFs fs;
  writeCameraListFile(fs, encodeCameraList(listOf({"OLD"})));
  fs.renameFailures = 1;
  TEST_ASSERT_NULL(writeCameraListFile(fs, encodeCameraList(listOf({"NEW"}))));
  assertNames({"NEW"}, storedNames(fs));
}

void test_failed_remove_keeps_the_old_list(void) {
  FakeFs fs;
  writeCameraListFile(fs, encodeCameraList(listOf({"OLD"})));
  fs.renameFailures = 1;
  fs.removeFails = true;
  TEST_ASSERT_NOT_NULL(writeCameraListFile(fs, encodeCameraList(listOf({"NEW"}))));
  assertNames({"OLD"}, storedNames(fs));
}

void test_cut_off_between_remove_and_rename_recovers_from_the_temporary_file(void) {
  FakeFs fs;
  writeCameraListFile(fs, encodeCameraList(listOf({"OLD"})));
  fs.renameFailures = 2;
  TEST_ASSERT_NOT_NULL(writeCameraListFile(fs, encodeCameraList(listOf({"NEW"}))));
  TEST_ASSERT_FALSE(fs.exists(CAMERA_FILE));
  assertNames({"NEW"}, storedNames(fs));
}

// ---- loading ----

void test_the_real_file_wins_over_a_leftover_temporary_one(void) {
  FakeFs fs;
  writeCameraListFile(fs, encodeCameraList(listOf({"REAL"})));
  fs.files[CAMERA_FILE_TMP] = encodeCameraList(listOf({"LEFTOVER"}));
  assertNames({"REAL"}, storedNames(fs));
}

void test_no_files_is_missing(void) {
  FakeFs fs;
  uint16_t version = 0;
  String records;
  TEST_ASSERT_TRUE(readCameraListFile(fs, version, records) == CameraFileStatus::Missing);
}

void test_a_truncated_temporary_file_alone_is_missing(void) {
  FakeFs fs;
  fs.files[CAMERA_FILE_TMP] = {0x08};
  uint16_t version = 0;
  String records;
  TEST_ASSERT_TRUE(readCameraListFile(fs, version, records) == CameraFileStatus::Missing);
}

void test_real_file_that_cannot_be_read_is_unreadable_not_missing(void) {
  FakeFs fs;
  writeCameraListFile(fs, encodeCameraList(listOf({"D01"})));
  fs.failReads = true;
  uint16_t version = 0;
  String records;
  TEST_ASSERT_TRUE(readCameraListFile(fs, version, records) == CameraFileStatus::Unreadable);
}

void test_real_file_too_short_for_a_version_is_unreadable(void) {
  FakeFs fs;
  fs.files[CAMERA_FILE] = {0x08};
  uint16_t version = 0;
  String records;
  TEST_ASSERT_TRUE(readCameraListFile(fs, version, records) == CameraFileStatus::Unreadable);
}

void test_bad_records_are_reported_and_good_ones_kept(void) {
  String records = serializeCamera(cam("GOOD1"));
  records += '\x1E';
  records += "BAD\x1Ftoo-few-fields";
  records += '\x1E';
  records += serializeCamera(cam("GOOD2"));
  records += '\x1E'; // a trailing separator isn't a record

  std::vector<String> dropped;
  ParsedCameraList parsed =
      parseCameraList(records, CAMERA_SCHEMA_VERSION, [&](const String& r) { dropped.push_back(r); });
  TEST_ASSERT_EQUAL(3, parsed.total);
  TEST_ASSERT_EQUAL(1, parsed.dropped);
  TEST_ASSERT_EQUAL(2, (int)parsed.cameras.size());
  TEST_ASSERT_EQUAL(1, (int)dropped.size());
  TEST_ASSERT_TRUE(dropped[0].startsWith("BAD"));
}

// ---- when loading may write ----

void test_an_unreadable_list_is_never_rewritten(void) {
  // Old behaviour: nothing read, version 0, no bad records - so an empty list
  // was saved over a file that might have been fine.
  TEST_ASSERT_FALSE(shouldRewriteCameraList(false, false, 0, 0));
}

void test_a_list_with_bad_records_is_never_rewritten(void) {
  TEST_ASSERT_FALSE(shouldRewriteCameraList(true, false, 1, CAMERA_SCHEMA_VERSION - 1));
  TEST_ASSERT_FALSE(shouldRewriteCameraList(false, true, 1, 0));
}

void test_old_schema_and_nvs_lists_are_migrated(void) {
  TEST_ASSERT_TRUE(shouldRewriteCameraList(true, false, 0, CAMERA_SCHEMA_VERSION - 1));
  TEST_ASSERT_TRUE(shouldRewriteCameraList(false, true, 0, CAMERA_SCHEMA_VERSION));
}

void test_a_current_file_is_left_alone(void) {
  TEST_ASSERT_FALSE(shouldRewriteCameraList(true, false, 0, CAMERA_SCHEMA_VERSION));
}

// ---- edits ----

void test_add_rejects_a_name_differing_only_by_case(void) {
  std::vector<CameraConfig> cams = listOf({"D01"});
  TEST_ASSERT_TRUE(applyCameraAdd(cams, cam("d01")) == CameraSaveResult::NameTaken);
  TEST_ASSERT_EQUAL(1, (int)cams.size());
  TEST_ASSERT_TRUE(applyCameraAdd(cams, cam("D02")) == CameraSaveResult::Saved);
  TEST_ASSERT_EQUAL(2, (int)cams.size());
}

void test_update_renames_in_place(void) {
  std::vector<CameraConfig> cams = listOf({"D01", "D02"});
  TEST_ASSERT_TRUE(applyCameraUpdate(cams, "d02", cam("D09")) == CameraSaveResult::Saved);
  TEST_ASSERT_EQUAL_STRING("D09", cams[1].name.c_str());
  TEST_ASSERT_EQUAL(2, (int)cams.size());
}

void test_update_may_change_only_the_case_of_its_own_name(void) {
  std::vector<CameraConfig> cams = listOf({"front"});
  TEST_ASSERT_TRUE(applyCameraUpdate(cams, "front", cam("FRONT")) == CameraSaveResult::Saved);
  TEST_ASSERT_EQUAL_STRING("FRONT", cams[0].name.c_str());
}

void test_update_rejects_another_cameras_name_and_changes_nothing(void) {
  std::vector<CameraConfig> cams = listOf({"D01", "D02"});
  TEST_ASSERT_TRUE(applyCameraUpdate(cams, "D01", cam("d02")) == CameraSaveResult::NameTaken);
  TEST_ASSERT_EQUAL_STRING("D01", cams[0].name.c_str());
}

void test_update_of_a_missing_camera_is_not_found(void) {
  std::vector<CameraConfig> cams = listOf({"D01"});
  TEST_ASSERT_TRUE(applyCameraUpdate(cams, "D99", cam("D99")) == CameraSaveResult::NotFound);
  TEST_ASSERT_EQUAL(1, (int)cams.size());
}

void test_delete(void) {
  std::vector<CameraConfig> cams = listOf({"D01", "D02"});
  TEST_ASSERT_FALSE(applyCameraDelete(cams, "D99"));
  TEST_ASSERT_TRUE(applyCameraDelete(cams, "d01"));
  TEST_ASSERT_EQUAL(1, (int)cams.size());
  TEST_ASSERT_EQUAL_STRING("D02", cams[0].name.c_str());
}

void test_blank_password_keeps_the_stored_one_even_across_a_rename(void) {
  std::vector<CameraConfig> stored = {cam("OLD", "stored-pw")};
  CameraConfig edited = cam("NEW", "");
  keepStoredPasswordIfBlank(edited, "old", stored);
  TEST_ASSERT_EQUAL_STRING("stored-pw", edited.pass.c_str());
}

void test_typed_password_replaces_the_stored_one(void) {
  std::vector<CameraConfig> stored = {cam("D01", "stored-pw")};
  CameraConfig edited = cam("D01", "typed-pw");
  keepStoredPasswordIfBlank(edited, "D01", stored);
  TEST_ASSERT_EQUAL_STRING("typed-pw", edited.pass.c_str());
}

void test_live_action_after_save(void) {
  TEST_ASSERT_TRUE(liveActionAfterSave(true, true) == LiveCameraAction::Reload);
  TEST_ASSERT_TRUE(liveActionAfterSave(true, false) == LiveCameraAction::Stop);
  // Includes a camera still enabled whose task exited (no credentials): this
  // save is what restarts it.
  TEST_ASSERT_TRUE(liveActionAfterSave(false, true) == LiveCameraAction::Start);
  TEST_ASSERT_TRUE(liveActionAfterSave(false, false) == LiveCameraAction::None);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_file_starts_with_the_schema_version_little_endian);
  RUN_TEST(test_list_survives_a_save_and_load);
  RUN_TEST(test_empty_list_round_trips_as_loaded_and_empty);
  RUN_TEST(test_short_write_keeps_the_old_list_and_cleans_up);
  RUN_TEST(test_rename_refused_falls_back_to_remove_then_rename);
  RUN_TEST(test_failed_remove_keeps_the_old_list);
  RUN_TEST(test_cut_off_between_remove_and_rename_recovers_from_the_temporary_file);
  RUN_TEST(test_the_real_file_wins_over_a_leftover_temporary_one);
  RUN_TEST(test_no_files_is_missing);
  RUN_TEST(test_a_truncated_temporary_file_alone_is_missing);
  RUN_TEST(test_real_file_that_cannot_be_read_is_unreadable_not_missing);
  RUN_TEST(test_real_file_too_short_for_a_version_is_unreadable);
  RUN_TEST(test_bad_records_are_reported_and_good_ones_kept);
  RUN_TEST(test_an_unreadable_list_is_never_rewritten);
  RUN_TEST(test_a_list_with_bad_records_is_never_rewritten);
  RUN_TEST(test_old_schema_and_nvs_lists_are_migrated);
  RUN_TEST(test_a_current_file_is_left_alone);
  RUN_TEST(test_add_rejects_a_name_differing_only_by_case);
  RUN_TEST(test_update_renames_in_place);
  RUN_TEST(test_update_may_change_only_the_case_of_its_own_name);
  RUN_TEST(test_update_rejects_another_cameras_name_and_changes_nothing);
  RUN_TEST(test_update_of_a_missing_camera_is_not_found);
  RUN_TEST(test_delete);
  RUN_TEST(test_blank_password_keeps_the_stored_one_even_across_a_rename);
  RUN_TEST(test_typed_password_replaces_the_stored_one);
  RUN_TEST(test_live_action_after_save);
  return UNITY_END();
}
