#include <unity.h>
#include <Arduino.h>
#include "snapshot_storage.h"

void setUp(void) {}
void tearDown(void) {}

// ---- sanitizeCameraDirName ----

void test_sanitizeCameraDirName_keeps_alnum_and_hyphen(void) {
  String dir = sanitizeCameraDirName("D01-FrontDoor");
  TEST_ASSERT_TRUE(dir.startsWith("D01-FrontDoor-"));
}

void test_sanitizeCameraDirName_strips_spaces_and_punctuation(void) {
  String dir = sanitizeCameraDirName("Front Door (Main)!");
  TEST_ASSERT_TRUE(dir.startsWith("FrontDoorMain-"));
}

void test_sanitizeCameraDirName_is_deterministic(void) {
  TEST_ASSERT_EQUAL_STRING(sanitizeCameraDirName("D01-FrontDoor").c_str(),
                            sanitizeCameraDirName("D01-FrontDoor").c_str());
}

// The exact scenario that makes lossy sanitization (e.g. this project's
// existing sanitizeHostname()) unsafe to reuse here - two different real
// camera names collapse to the same stripped prefix. The hash suffix must
// disambiguate them.
void test_sanitizeCameraDirName_disambiguates_names_with_same_stripped_prefix(void) {
  String a = sanitizeCameraDirName("Front Door");
  String b = sanitizeCameraDirName("FrontDoor");
  TEST_ASSERT_TRUE(a.startsWith("FrontDoor-"));
  TEST_ASSERT_TRUE(b.startsWith("FrontDoor-"));
  TEST_ASSERT_FALSE(a == b);
}

// The FAT-specific case: directory names are case-insensitive on FAT, so
// two names differing only by case need MORE than just a differently-cased
// suffix - the actual hex digits must differ, not just their letter case.
void test_sanitizeCameraDirName_case_variants_produce_different_hash_digits(void) {
  String a = sanitizeCameraDirName("Camera1");
  String b = sanitizeCameraDirName("camera1");
  String aLower = a; aLower.toLowerCase();
  String bLower = b; bLower.toLowerCase();
  TEST_ASSERT_FALSE(aLower == bLower); // must differ even after case-folding both
}

void test_sanitizeCameraDirName_handles_empty_name_without_crashing(void) {
  String dir = sanitizeCameraDirName("");
  TEST_ASSERT_TRUE(dir.length() > 0); // just the "-XXXX" hash suffix, but not empty
}

// ---- filesToPrune ----

static SnapshotFileInfo makeFile(const char* name, uint64_t size) {
  SnapshotFileInfo f;
  f.name = name;
  f.size = size;
  return f;
}

void test_filesToPrune_reclaims_oldest_first_until_enough(void) {
  std::vector<SnapshotFileInfo> files = {
      makeFile("a.jpg", 100), makeFile("b.jpg", 100), makeFile("c.jpg", 100)};
  auto result = filesToPrune(files, 150, 10);
  // 100 (a) isn't enough, +100 (b) = 200 >= 150 - stop before c.
  TEST_ASSERT_EQUAL_INT(2, (int)result.size());
  TEST_ASSERT_EQUAL_STRING("a.jpg", result[0].c_str());
  TEST_ASSERT_EQUAL_STRING("b.jpg", result[1].c_str());
}

// The existing "until enough" test above only ever lands PAST the target
// (100, then 200 past a 150 target) - never exactly ON it. A `<` -> `<=`
// mutation of the loop's `reclaimed < bytesNeeded` condition would pass
// that test unchanged but over-prune here: two 75-byte files exactly hit
// a 150-byte target after the second one, so a correct implementation
// stops there (2 files) - a `<=` bug would keep going into a third file
// it didn't need to delete.
void test_filesToPrune_stops_exactly_at_target_not_one_file_past(void) {
  std::vector<SnapshotFileInfo> files = {
      makeFile("a.jpg", 75), makeFile("b.jpg", 75), makeFile("c.jpg", 75)};
  auto result = filesToPrune(files, 150, 10);
  TEST_ASSERT_EQUAL_INT(2, (int)result.size());
  TEST_ASSERT_EQUAL_STRING("a.jpg", result[0].c_str());
  TEST_ASSERT_EQUAL_STRING("b.jpg", result[1].c_str());
}

void test_filesToPrune_zero_bytes_needed_deletes_nothing(void) {
  std::vector<SnapshotFileInfo> files = {makeFile("a.jpg", 100)};
  auto result = filesToPrune(files, 0, 10);
  TEST_ASSERT_TRUE(result.empty());
}

void test_filesToPrune_empty_file_list_returns_empty(void) {
  auto result = filesToPrune({}, 1000, 10);
  TEST_ASSERT_TRUE(result.empty());
}

// The core correctness property from this function's own design intent:
// even if reclaiming enough would need more files, it must never return
// more than maxFiles - that cap is what bounds the SD mutex hold time
// during a prune-then-write pass (see snapshot_storage.h's comment).
void test_filesToPrune_never_exceeds_maxFiles_even_if_not_enough_reclaimed(void) {
  std::vector<SnapshotFileInfo> files = {
      makeFile("a.jpg", 10), makeFile("b.jpg", 10), makeFile("c.jpg", 10), makeFile("d.jpg", 10)};
  auto result = filesToPrune(files, 1000000, 2); // way more than these 4 files could ever reclaim
  TEST_ASSERT_EQUAL_INT(2, (int)result.size());
  TEST_ASSERT_EQUAL_STRING("a.jpg", result[0].c_str());
  TEST_ASSERT_EQUAL_STRING("b.jpg", result[1].c_str());
}

void test_filesToPrune_maxFiles_zero_deletes_nothing(void) {
  std::vector<SnapshotFileInfo> files = {makeFile("a.jpg", 100)};
  auto result = filesToPrune(files, 1000, 0);
  TEST_ASSERT_TRUE(result.empty());
}

// ---- parseSnapshotTimestamp ----

void test_parseSnapshotTimestamp_parses_valid_filename(void) {
  time_t t = parseSnapshotTimestamp("20260315-143022_1234.jpg");
  TEST_ASSERT_NOT_EQUAL((time_t)-1, t);
}

void test_parseSnapshotTimestamp_same_date_parses_identically_regardless_of_suffix(void) {
  // The millis+extension suffix must never affect the parsed date/time -
  // only the first 15 characters matter.
  time_t a = parseSnapshotTimestamp("20260315-143022_1.jpg");
  time_t b = parseSnapshotTimestamp("20260315-143022_999999.jpg");
  TEST_ASSERT_EQUAL((long)a, (long)b);
}

void test_parseSnapshotTimestamp_rejects_too_short(void) {
  TEST_ASSERT_EQUAL((time_t)-1, parseSnapshotTimestamp("20260315-1430"));
}

void test_parseSnapshotTimestamp_rejects_missing_dash(void) {
  TEST_ASSERT_EQUAL((time_t)-1, parseSnapshotTimestamp("20260315X143022_1.jpg"));
}

void test_parseSnapshotTimestamp_rejects_non_digit_in_date_portion(void) {
  TEST_ASSERT_EQUAL((time_t)-1, parseSnapshotTimestamp("2026031X-143022_1.jpg"));
}

void test_parseSnapshotTimestamp_rejects_non_digit_in_time_portion(void) {
  TEST_ASSERT_EQUAL((time_t)-1, parseSnapshotTimestamp("20260315-14302X_1.jpg"));
}

void test_parseSnapshotTimestamp_rejects_empty_string(void) {
  TEST_ASSERT_EQUAL((time_t)-1, parseSnapshotTimestamp(""));
}

// ---- filesToExpire ----
//
// All test dates below stay within January (no DST transition in that
// range in any common timezone), so day-difference math via mktime stays
// exactly N*86400 seconds regardless of the host machine's local timezone -
// this test suite must pass identically on any CI runner's TZ setting.

void test_filesToExpire_zero_retentionDays_keeps_everything(void) {
  time_t now = parseSnapshotTimestamp("20260131-120000_0.jpg");
  std::vector<SnapshotFileInfo> files = {makeFile("20200101-000000_0.jpg", 100)}; // ancient
  auto result = filesToExpire(files, 0, now);
  TEST_ASSERT_TRUE(result.empty());
}

void test_filesToExpire_deletes_files_older_than_retention(void) {
  time_t now = parseSnapshotTimestamp("20260131-120000_0.jpg"); // Jan 31, noon
  std::vector<SnapshotFileInfo> files = {
      makeFile("20260115-120000_0.jpg", 100), // 16 days old - keep
      makeFile("20260101-115959_0.jpg", 100), // 30 days + 1s old - expire
  };
  auto result = filesToExpire(files, 30, now);
  TEST_ASSERT_EQUAL_INT(1, (int)result.size());
  TEST_ASSERT_EQUAL_STRING("20260101-115959_0.jpg", result[0].c_str());
}

// A file exactly retentionDays old (to the second) is "not yet older than"
// the limit - "older than N days", not "at least N days" - a `<` -> `<=`
// mutation in the cutoff comparison would delete it one tick too early.
void test_filesToExpire_exactly_at_boundary_is_kept(void) {
  time_t now = parseSnapshotTimestamp("20260131-120000_0.jpg");
  std::vector<SnapshotFileInfo> files = {makeFile("20260101-120000_0.jpg", 100)}; // exactly 30 days old
  auto result = filesToExpire(files, 30, now);
  TEST_ASSERT_TRUE(result.empty());
}

// A malformed filename mixed in among valid ones must be left alone
// (skipped), not guessed at or crash the whole sweep.
void test_filesToExpire_skips_unparseable_filenames(void) {
  time_t now = parseSnapshotTimestamp("20260131-120000_0.jpg");
  std::vector<SnapshotFileInfo> files = {
      makeFile("not-a-timestamp.jpg", 100),
      makeFile("20200101-000000_0.jpg", 100), // genuinely ancient - expires
  };
  auto result = filesToExpire(files, 30, now);
  TEST_ASSERT_EQUAL_INT(1, (int)result.size());
  TEST_ASSERT_EQUAL_STRING("20200101-000000_0.jpg", result[0].c_str());
}

void test_filesToExpire_empty_file_list_returns_empty(void) {
  time_t now = parseSnapshotTimestamp("20260131-120000_0.jpg");
  auto result = filesToExpire({}, 30, now);
  TEST_ASSERT_TRUE(result.empty());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_sanitizeCameraDirName_keeps_alnum_and_hyphen);
  RUN_TEST(test_sanitizeCameraDirName_strips_spaces_and_punctuation);
  RUN_TEST(test_sanitizeCameraDirName_is_deterministic);
  RUN_TEST(test_sanitizeCameraDirName_disambiguates_names_with_same_stripped_prefix);
  RUN_TEST(test_sanitizeCameraDirName_case_variants_produce_different_hash_digits);
  RUN_TEST(test_sanitizeCameraDirName_handles_empty_name_without_crashing);
  RUN_TEST(test_filesToPrune_reclaims_oldest_first_until_enough);
  RUN_TEST(test_filesToPrune_stops_exactly_at_target_not_one_file_past);
  RUN_TEST(test_filesToPrune_zero_bytes_needed_deletes_nothing);
  RUN_TEST(test_filesToPrune_empty_file_list_returns_empty);
  RUN_TEST(test_filesToPrune_never_exceeds_maxFiles_even_if_not_enough_reclaimed);
  RUN_TEST(test_filesToPrune_maxFiles_zero_deletes_nothing);
  RUN_TEST(test_parseSnapshotTimestamp_parses_valid_filename);
  RUN_TEST(test_parseSnapshotTimestamp_same_date_parses_identically_regardless_of_suffix);
  RUN_TEST(test_parseSnapshotTimestamp_rejects_too_short);
  RUN_TEST(test_parseSnapshotTimestamp_rejects_missing_dash);
  RUN_TEST(test_parseSnapshotTimestamp_rejects_non_digit_in_date_portion);
  RUN_TEST(test_parseSnapshotTimestamp_rejects_non_digit_in_time_portion);
  RUN_TEST(test_parseSnapshotTimestamp_rejects_empty_string);
  RUN_TEST(test_filesToExpire_zero_retentionDays_keeps_everything);
  RUN_TEST(test_filesToExpire_deletes_files_older_than_retention);
  RUN_TEST(test_filesToExpire_exactly_at_boundary_is_kept);
  RUN_TEST(test_filesToExpire_skips_unparseable_filenames);
  RUN_TEST(test_filesToExpire_empty_file_list_returns_empty);
  return UNITY_END();
}
