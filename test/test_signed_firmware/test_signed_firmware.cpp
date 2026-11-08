#include <unity.h>
#include <vector>
#include "signed_firmware.h"

void setUp(void) {}
void tearDown(void) {}

// The layout scripts/board.py writes.
static std::vector<uint8_t> signedFile(size_t signatureBytes, size_t imageBytes) {
  std::vector<uint8_t> f = {'A', 'O', 'T', 'S', 'I', 'G', '1', '\n', (uint8_t)signatureBytes, 0, 0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < 72; i++) f.push_back(i < signatureBytes ? (uint8_t)(0x30 + i) : 0);
  for (size_t i = 0; i < imageBytes; i++) f.push_back(i == 0 ? 0xE9 : (uint8_t)i);
  return f;
}

// Feeds `file` in chunks of `chunk` bytes, returning the image bytes passed on.
static std::vector<uint8_t> feedAll(SignedFirmwareReader& r, const std::vector<uint8_t>& file, size_t chunk) {
  std::vector<uint8_t> image;
  for (size_t i = 0; i < file.size(); i += chunk) {
    size_t n = file.size() - i < chunk ? file.size() - i : chunk;
    const uint8_t* out;
    size_t outLen;
    r.feed(file.data() + i, n, out, outLen);
    if (outLen) image.insert(image.end(), out, out + outLen);
  }
  return image;
}

void test_header_size_matches_the_layout(void) {
  TEST_ASSERT_EQUAL((int)SIGNED_FIRMWARE_HEADER_BYTES, (int)signedFile(70, 0).size());
}

void test_image_comes_through_whole_at_every_chunk_size(void) {
  std::vector<uint8_t> file = signedFile(71, 300);
  std::vector<uint8_t> expected(file.begin() + SIGNED_FIRMWARE_HEADER_BYTES, file.end());
  for (size_t chunk = 1; chunk <= file.size(); chunk++) {
    SignedFirmwareReader r;
    r.reset(false);
    std::vector<uint8_t> image = feedAll(r, file, chunk);
    TEST_ASSERT_TRUE(r.headerComplete());
    TEST_ASSERT_EQUAL((int)expected.size(), (int)image.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), image.data(), expected.size());
    TEST_ASSERT_EQUAL(71, (int)r.signatureLength());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(file.data() + 16, r.signature(), 71);
  }
}

void test_a_plain_app_image_is_refused(void) {
  std::vector<uint8_t> plain = {0xE9, 1, 2, 3, 4};
  SignedFirmwareReader r;
  r.reset(false);
  std::vector<uint8_t> image = feedAll(r, plain, 2);
  TEST_ASSERT_TRUE(r.rejected());
  TEST_ASSERT_EQUAL(0, (int)image.size());
}

void test_a_plain_app_image_passes_when_allowed(void) {
  std::vector<uint8_t> plain = {0xE9, 1, 2, 3, 4};
  SignedFirmwareReader r;
  r.reset(true);
  std::vector<uint8_t> image = feedAll(r, plain, 2);
  TEST_ASSERT_TRUE(r.isUnsigned());
  TEST_ASSERT_FALSE(r.headerComplete());
  TEST_ASSERT_EQUAL(5, (int)image.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(plain.data(), image.data(), 5);
}

void test_a_damaged_header_is_refused_even_when_unsigned_is_allowed(void) {
  // The allowance is for files with no header, not for a header that's wrong.
  std::vector<uint8_t> file = signedFile(70, 10);
  file[3] = 'X';
  SignedFirmwareReader r;
  r.reset(true);
  TEST_ASSERT_EQUAL(0, (int)feedAll(r, file, 13).size());
  TEST_ASSERT_TRUE(r.rejected());
}

void test_impossible_signature_lengths_are_refused(void) {
  for (size_t bad : {(size_t)0, (size_t)7, (size_t)73, (size_t)255}) {
    std::vector<uint8_t> file = signedFile(70, 10);
    file[8] = (uint8_t)bad;
    SignedFirmwareReader r;
    r.reset(false);
    TEST_ASSERT_EQUAL(0, (int)feedAll(r, file, 40).size());
    TEST_ASSERT_TRUE(r.rejected());
  }
}

void test_shortest_and_longest_signatures_are_accepted(void) {
  for (size_t ok : {(size_t)8, (size_t)72}) {
    SignedFirmwareReader r;
    r.reset(false);
    feedAll(r, signedFile(ok, 10), 40);
    TEST_ASSERT_TRUE(r.headerComplete());
  }
}

void test_reserved_bytes_must_be_zero(void) {
  std::vector<uint8_t> file = signedFile(70, 10);
  file[15] = 1;
  SignedFirmwareReader r;
  r.reset(false);
  feedAll(r, file, 40);
  TEST_ASSERT_TRUE(r.rejected());
}

void test_a_file_cut_off_in_the_header_is_not_complete(void) {
  std::vector<uint8_t> file = signedFile(70, 0);
  file.resize(40);
  SignedFirmwareReader r;
  r.reset(false);
  TEST_ASSERT_EQUAL(0, (int)feedAll(r, file, 40).size());
  TEST_ASSERT_FALSE(r.headerComplete());
  TEST_ASSERT_FALSE(r.rejected());
}

void test_nothing_passes_after_a_rejection(void) {
  SignedFirmwareReader r;
  r.reset(false);
  uint8_t chunk[4] = {0xE9, 1, 2, 3};
  const uint8_t* out;
  size_t outLen = 99;
  r.feed(chunk, 4, out, outLen);
  r.feed(chunk, 4, out, outLen);
  TEST_ASSERT_EQUAL(0, (int)outLen);
  TEST_ASSERT_NULL(out);
}

void test_reset_starts_a_new_upload_cleanly(void) {
  SignedFirmwareReader r;
  r.reset(false);
  std::vector<uint8_t> plain = {0xE9, 1};
  feedAll(r, plain, 2);
  TEST_ASSERT_TRUE(r.rejected());
  r.reset(false);
  std::vector<uint8_t> file = signedFile(70, 5);
  TEST_ASSERT_EQUAL(5, (int)feedAll(r, file, 9).size());
  TEST_ASSERT_TRUE(r.headerComplete());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_header_size_matches_the_layout);
  RUN_TEST(test_image_comes_through_whole_at_every_chunk_size);
  RUN_TEST(test_a_plain_app_image_is_refused);
  RUN_TEST(test_a_plain_app_image_passes_when_allowed);
  RUN_TEST(test_a_damaged_header_is_refused_even_when_unsigned_is_allowed);
  RUN_TEST(test_impossible_signature_lengths_are_refused);
  RUN_TEST(test_shortest_and_longest_signatures_are_accepted);
  RUN_TEST(test_reserved_bytes_must_be_zero);
  RUN_TEST(test_a_file_cut_off_in_the_header_is_not_complete);
  RUN_TEST(test_nothing_passes_after_a_rejection);
  RUN_TEST(test_reset_starts_a_new_upload_cleanly);
  return UNITY_END();
}
