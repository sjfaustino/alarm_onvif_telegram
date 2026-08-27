#include <unity.h>
#include <Arduino.h>
#include "nvs_chunk.h"

void setUp(void) {}
void tearDown(void) {}

void test_splitIntoChunks_empty_string_produces_zero_chunks(void) {
  auto chunks = splitIntoChunks("", 10);
  TEST_ASSERT_EQUAL_INT(0, (int)chunks.size());
}

void test_splitIntoChunks_shorter_than_max_is_one_chunk(void) {
  auto chunks = splitIntoChunks("hello", 100);
  TEST_ASSERT_EQUAL_INT(1, (int)chunks.size());
  TEST_ASSERT_EQUAL_STRING("hello", chunks[0].c_str());
}

void test_splitIntoChunks_exact_multiple_of_max(void) {
  auto chunks = splitIntoChunks("abcdefghij", 5); // 10 chars / 5 = exactly 2 chunks
  TEST_ASSERT_EQUAL_INT(2, (int)chunks.size());
  TEST_ASSERT_EQUAL_STRING("abcde", chunks[0].c_str());
  TEST_ASSERT_EQUAL_STRING("fghij", chunks[1].c_str());
}

void test_splitIntoChunks_remainder_becomes_final_shorter_chunk(void) {
  auto chunks = splitIntoChunks("abcdefg", 3); // 7 chars / 3 -> 3,3,1
  TEST_ASSERT_EQUAL_INT(3, (int)chunks.size());
  TEST_ASSERT_EQUAL_STRING("abc", chunks[0].c_str());
  TEST_ASSERT_EQUAL_STRING("def", chunks[1].c_str());
  TEST_ASSERT_EQUAL_STRING("g", chunks[2].c_str());
}

void test_splitIntoChunks_single_char_max(void) {
  auto chunks = splitIntoChunks("ab", 1);
  TEST_ASSERT_EQUAL_INT(2, (int)chunks.size());
  TEST_ASSERT_EQUAL_STRING("a", chunks[0].c_str());
  TEST_ASSERT_EQUAL_STRING("b", chunks[1].c_str());
}

// ---- joinChunks ----

void test_joinChunks_empty_vector_is_empty_string(void) {
  std::vector<String> chunks;
  TEST_ASSERT_EQUAL_STRING("", joinChunks(chunks).c_str());
}

void test_joinChunks_reassembles_in_order(void) {
  std::vector<String> chunks = {"abc", "def", "g"};
  TEST_ASSERT_EQUAL_STRING("abcdefg", joinChunks(chunks).c_str());
}

// ---- round trip ----

void test_round_trip_various_sizes(void) {
  const char* samples[] = {"", "x", "abcdefghij", "a very long string with several words in it, "
                                                    "meant to exercise a realistic multi-chunk case"};
  for (const char* s : samples) {
    String original = s;
    String rebuilt = joinChunks(splitIntoChunks(original, 7));
    TEST_ASSERT_EQUAL_STRING(original.c_str(), rebuilt.c_str());
  }
}

// Contains the same separator bytes camera_store.cpp/telegram_users.cpp
// join records with (0x1E) - chunking must be byte-transparent, not
// text-aware, so a chunk boundary landing mid-record can't corrupt it.
void test_round_trip_preserves_embedded_control_characters(void) {
  String original = String("rec1") + (char)0x1E + String("rec2") + (char)0x1E + String("rec3");
  String rebuilt = joinChunks(splitIntoChunks(original, 3));
  TEST_ASSERT_EQUAL_STRING(original.c_str(), rebuilt.c_str());
}

int main(int argc, char** argv) {
  UNITY_BEGIN();
  RUN_TEST(test_splitIntoChunks_empty_string_produces_zero_chunks);
  RUN_TEST(test_splitIntoChunks_shorter_than_max_is_one_chunk);
  RUN_TEST(test_splitIntoChunks_exact_multiple_of_max);
  RUN_TEST(test_splitIntoChunks_remainder_becomes_final_shorter_chunk);
  RUN_TEST(test_splitIntoChunks_single_char_max);
  RUN_TEST(test_joinChunks_empty_vector_is_empty_string);
  RUN_TEST(test_joinChunks_reassembles_in_order);
  RUN_TEST(test_round_trip_various_sizes);
  RUN_TEST(test_round_trip_preserves_embedded_control_characters);
  return UNITY_END();
}
