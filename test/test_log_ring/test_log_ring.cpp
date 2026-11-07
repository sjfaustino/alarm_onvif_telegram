#include <unity.h>
#include <cstring>
#include <string>
#include "log_ring.h"

void setUp(void) {}
void tearDown(void) {}

static std::string readAll(const LogRing& r, uint32_t from, uint32_t* next = nullptr) {
  char out[64];
  size_t n = r.read(from, out, sizeof(out), next);
  return std::string(out, n);
}

void test_reads_back_what_was_written(void) {
  char buf[8];
  LogRing r(buf, sizeof(buf));
  r.write("abc", 3);
  uint32_t next = 0;
  TEST_ASSERT_EQUAL_STRING("abc", readAll(r, 0, &next).c_str());
  TEST_ASSERT_EQUAL_UINT32(3, next);
}

void test_poll_returns_only_new_bytes(void) {
  char buf[8];
  LogRing r(buf, sizeof(buf));
  r.write("abc", 3);
  uint32_t next = 0;
  readAll(r, 0, &next);
  TEST_ASSERT_EQUAL_STRING("", readAll(r, next, &next).c_str());
  r.write("de", 2);
  TEST_ASSERT_EQUAL_STRING("de", readAll(r, next, &next).c_str());
  TEST_ASSERT_EQUAL_UINT32(5, next);
}

void test_wrap_keeps_newest_bytes_in_order(void) {
  char buf[4];
  LogRing r(buf, sizeof(buf));
  r.write("abcdef", 6);
  TEST_ASSERT_EQUAL_UINT32(2, r.oldest());
  TEST_ASSERT_EQUAL_STRING("cdef", readAll(r, 2).c_str());
  r.write("g", 1);
  TEST_ASSERT_EQUAL_STRING("defg", readAll(r, 3).c_str());
}

void test_evicted_position_reads_from_oldest(void) {
  char buf[4];
  LogRing r(buf, sizeof(buf));
  r.write("abcdef", 6);
  uint32_t next = 0;
  TEST_ASSERT_EQUAL_STRING("cdef", readAll(r, 0, &next).c_str());
  TEST_ASSERT_EQUAL_UINT32(6, next);
}

void test_future_position_reads_from_oldest(void) {
  char buf[8];
  LogRing r(buf, sizeof(buf));
  r.write("abc", 3);
  TEST_ASSERT_EQUAL_STRING("abc", readAll(r, 100).c_str());
}

void test_max_len_is_respected_and_resumes(void) {
  char buf[8];
  LogRing r(buf, sizeof(buf));
  r.write("abcdef", 6);
  char out[4];
  uint32_t next = 0;
  TEST_ASSERT_EQUAL(4, (int)r.read(0, out, 4, &next));
  TEST_ASSERT_EQUAL_MEMORY("abcd", out, 4);
  TEST_ASSERT_EQUAL_UINT32(4, next);
  TEST_ASSERT_EQUAL_STRING("ef", readAll(r, next).c_str());
}

void test_write_larger_than_capacity_advances_position_fully(void) {
  char buf[4];
  LogRing r(buf, sizeof(buf));
  r.write("x", 1);
  r.write("abcdefghij", 10);
  TEST_ASSERT_EQUAL_UINT32(11, r.end());
  TEST_ASSERT_EQUAL_STRING("ghij", readAll(r, r.oldest()).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reads_back_what_was_written);
  RUN_TEST(test_poll_returns_only_new_bytes);
  RUN_TEST(test_wrap_keeps_newest_bytes_in_order);
  RUN_TEST(test_evicted_position_reads_from_oldest);
  RUN_TEST(test_future_position_reads_from_oldest);
  RUN_TEST(test_max_len_is_respected_and_resumes);
  RUN_TEST(test_write_larger_than_capacity_advances_position_fully);
  return UNITY_END();
}
