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

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_sanitizeCameraDirName_keeps_alnum_and_hyphen);
  RUN_TEST(test_sanitizeCameraDirName_strips_spaces_and_punctuation);
  RUN_TEST(test_sanitizeCameraDirName_is_deterministic);
  RUN_TEST(test_sanitizeCameraDirName_disambiguates_names_with_same_stripped_prefix);
  RUN_TEST(test_sanitizeCameraDirName_case_variants_produce_different_hash_digits);
  RUN_TEST(test_sanitizeCameraDirName_handles_empty_name_without_crashing);
  RUN_TEST(test_filesToPrune_reclaims_oldest_first_until_enough);
  RUN_TEST(test_filesToPrune_zero_bytes_needed_deletes_nothing);
  RUN_TEST(test_filesToPrune_empty_file_list_returns_empty);
  RUN_TEST(test_filesToPrune_never_exceeds_maxFiles_even_if_not_enough_reclaimed);
  RUN_TEST(test_filesToPrune_maxFiles_zero_deletes_nothing);
  return UNITY_END();
}
